// SPDX-License-Identifier: MIT
//
// Runs one of FEX's assembly instruction tests against the translator core alone.
//
// Upstream's TestHarnessRunner does the same job but is built on the Linux process
// emulation layer, which the macOS port does not have. This runner needs only the core:
// it maps the test's memory, runs the guest code until it halts, and compares the final
// register state with the values the test file declares.
//
// Usage: FEXCoreASMTestRunner <test.bin> <test.config.bin>
// Exit code: 0 pass, 1 fail, 125 skipped (the test needs something this host lacks).

#include "Common/Config.h"
#include "Common/HostFeatures.h"
#include "Common/X86Features.h"
#include "CoreThreadSetup.h"
#include "DummyHandlers.h"
#include "HarnessHelpers.h"
#include "MacOSHostFaults.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/SignalScopeGuards.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/memory.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/unordered_map.h>
#include <FEXCore/fextl/vector.h>
#include <FEXHeaderUtils/Filesystem.h>

#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <set>
#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

namespace {
constexpr int ExitPass = 0;
constexpr int ExitFail = 1;
constexpr int ExitSkip = 125;

void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  fextl::fmt::print("{} {}\n", LogMan::DebugLevelStr(Level), Message);
}

void AssertHandler(const char* Message) {
  fextl::fmt::print("A {}\n", Message);
  fflush(nullptr);
}

const fextl::vector<std::pair<const char*, FEXCore::Config::ConfigOption>> EnvConfigLookup = {{
#define OPT_BASE(type, group, enum, json, default) {"FEX_" #enum, FEXCore::Config::ConfigOption::CONFIG_##enum},
#include <FEXCore/Config/ConfigValues.inl>
}};

// Applies the options a test file asks for, as if they were environment variables.
class TestEnvLoader final : public FEXCore::Config::Layer {
public:
  explicit TestEnvLoader(fextl::vector<std::pair<std::string_view, std::string_view>> _Env)
    : FEXCore::Config::Layer(FEXCore::Config::LayerType::LAYER_LOCAL_APP)
    , Env {std::move(_Env)} {
    Load();
  }

  void Load() override {
    fextl::unordered_map<std::string_view, std::string> EnvMap;
    for (auto& Option : Env) {
      std::string_view Key = Option.first;
      std::string_view Value_View = Option.second;
      std::optional<fextl::string> Value;
#define ENVLOADER
#include <FEXCore/Config/ConfigOptions.inl>
      if (Value) {
        EnvMap.insert_or_assign(Key, *Value);
      } else {
        EnvMap.insert_or_assign(Key, Value_View);
      }
    }

    auto GetVar = [&](const std::string_view id) -> std::optional<std::string_view> {
      const auto it = EnvMap.find(id);
      if (it == EnvMap.end()) {
        return std::nullopt;
      }
      return it->second;
    };

    for (auto& it : EnvConfigLookup) {
      if (auto Value = GetVar(it.first); Value) {
        Set(it.second, *Value);
      }
    }
  }

private:
  fextl::vector<std::pair<std::string_view, std::string_view>> Env;
};

class RunnerSyscallHandler final : public FEXCore::HLE::SyscallHandler, public FEXCore::Allocator::FEXAllocOperators {
public:
  // This runner has no Linux system calls (tests that need them declare it and are
  // skipped). Every call fails with ENOSYS, as a Linux kernel answers an unknown number.
  // The core leaves RIP at the `syscall` instruction and expects the handler to step past
  // it, as FEX's Linux handler does; without that the guest runs the same call forever.
  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override {
    constexpr uint64_t LinuxENOSYS = 38;
    Frame->State.gregs[FEXCore::X86State::REG_RAX] = -LinuxENOSYS;
    // `syscall` and `int 0x80` are both two bytes long.
    Frame->State.rip += 2;
  }

  // The core calls this when translated code finds that the guest code it came from has
  // changed. Discarding the stale translations is the embedder's job; without it the
  // core finds the same stale block again and never makes progress.
  void InvalidateGuestCodeRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) override {
    auto Lock = FEXCore::GuardSignalDeferringSectionWithFallback(Thread->CTX->GetCodeInvalidationMutex(), Thread);
    Thread->CTX->InvalidateCodeBuffersCodeRange(Start, Length);
    Thread->CTX->InvalidateThreadCachedCodeRange(Thread, Start, Length);
  }

  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }

  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return {0, UINT64_MAX, true};
  }
};

// The tests end with `hlt`. The core reports that as a synchronous fault and calls this
// function directly from the dispatcher, with guest state already written back to the
// frame. No host signal is involved.
jmp_buf LongJump {};
bool DidFault {};

[[noreturn]]
void SynchronousExceptionHandler(FEXCore::Context::Context*, FEXCore::Core::CpuStateFrame* Frame) {
  const auto& Fault = Frame->SynchronousFaultData;
  // A halt is reported as a segmentation fault at a `hlt` instruction. Anything else
  // means the test went wrong.
  DidFault = !Fault.FaultToTopAndGeneratedException || Fault.Signal != FEXCore::Core::FAULT_SIGSEGV;
  constexpr uint8_t HLT = 0xF4;
  if (reinterpret_cast<const uint8_t*>(Frame->State.rip)[0] != HLT) {
    DidFault = true;
  }
  longjmp(LongJump, 1);
}

// Maps guest memory at the exact address a test asks for. The tests assume 4K pages;
// the host may use 16K pages, so requests are widened to host pages and pages already
// mapped for this test are left alone. An address that is not free is never taken by
// force: the mapping is reported as failed instead.
class GuestMapper {
public:
  GuestMapper()
    : PageSize {static_cast<uint64_t>(sysconf(_SC_PAGESIZE))} {}

  void* Map(uint64_t Address, size_t Size) {
    const uint64_t Begin = Address & ~(PageSize - 1);
    const uint64_t End = (Address + Size + PageSize - 1) & ~(PageSize - 1);
    for (uint64_t Page = Begin; Page < End; Page += PageSize) {
      if (Mapped.contains(Page)) {
        continue;
      }
      int Flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef __APPLE__
      // macOS treats the address as a loose hint, so ask the kernel whether the page is
      // free and, only if it is, place the mapping there exactly.
      mach_vm_address_t RegionAddress = Page;
      mach_vm_size_t RegionSize = 0;
      vm_region_basic_info_data_64_t Info {};
      mach_msg_type_number_t Count = VM_REGION_BASIC_INFO_COUNT_64;
      mach_port_t Object = MACH_PORT_NULL;
      const kern_return_t Status = mach_vm_region(mach_task_self(), &RegionAddress, &RegionSize, VM_REGION_BASIC_INFO_64,
                                                  reinterpret_cast<vm_region_info_t>(&Info), &Count, &Object);
      if (Status == KERN_SUCCESS && RegionAddress < Page + PageSize) {
        fextl::fmt::print("Could not map guest memory at 0x{:x}: in use by a region at 0x{:x} of size 0x{:x}\n", Page,
                          static_cast<uint64_t>(RegionAddress), static_cast<uint64_t>(RegionSize));
        Failed = true;
        return MAP_FAILED;
      }
      Flags |= MAP_FIXED;
#endif
      void* Result = ::mmap(reinterpret_cast<void*>(Page), PageSize, PROT_READ | PROT_WRITE, Flags, -1, 0);
      if (Result != reinterpret_cast<void*>(Page)) {
        fextl::fmt::print("Could not map guest memory at 0x{:x}: got {} (errno {})\n", Page, fmt::ptr(Result), errno);
        if (Result != MAP_FAILED) {
          ::munmap(Result, PageSize);
        }
        Failed = true;
        return MAP_FAILED;
      }
      Mapped.insert(Page);
    }
    return reinterpret_cast<void*>(Address);
  }

  bool Failed {};

private:
  uint64_t PageSize;
  std::set<uint64_t> Mapped;
};
} // namespace

int main(int argc, char** argv, char** const envp) {
  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);

  FEX::Config::InitializeConfigs(FEX::Config::PortableInformation {});
  FEXCore::Config::Initialize();
  FEXCore::Config::AddLayer(FEX::Config::CreateEnvironmentLayer(envp));
  FEXCore::Config::Load();

  if (argc < 3) {
    fextl::fmt::print("Usage: {} <test.bin> <test.config.bin>\n", argv[0]);
    return ExitFail;
  }
  const char* Filename = argv[1];
  const char* ConfigFile = argv[2];
  if (!FHU::Filesystem::Exists(Filename) || !FHU::Filesystem::Exists(ConfigFile)) {
    fextl::fmt::print("Test file or config file does not exist\n");
    return ExitFail;
  }

  FEX::HarnessHelper::HarnessCodeLoader Loader {Filename, ConfigFile};

  FEXCore::Config::AddLayer(fextl::make_unique<TestEnvLoader>(Loader.GetEnvironmentOptions()));
  FEXCore::Config::ReloadMetaLayer();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, Loader.Is64BitMode() ? "1" : "0");

  if (!Loader.Is64BitMode()) {
    fextl::fmt::print("Skipped: 32-bit tests are not supported by this runner yet\n");
    return ExitSkip;
  }
  if (Loader.RequiresLinux()) {
    fextl::fmt::print("Skipped: test needs Linux system calls\n");
    return ExitSkip;
  }

  auto HostFeatures = FEX::FetchHostFeatures(Loader.Is64BitMode());
  auto CTX = FEXCore::Context::Context::CreateNewContext(HostFeatures);

  const bool SupportsAVX = HostFeatures.SupportsAVX();

#if !defined(VIXL_SIMULATOR) && defined(ARCHITECTURE_x86_64)
  FEX::X86::Features Feature {};
#else
  FEX::X86::Features Feature {CTX.get()};
#endif
  // FEX disables this by default; the tests exercise it anyway.
  const bool SupportsSSE4A = true;

  // Each feature a test may ask for, and whether this host and translator provide it. The
  // first group is read from the host; the rest from the CPUID the translator shows the
  // guest, which is what a guest program would check. A skipped test is reported with
  // the names of the features it lacks, so nobody has to work out why it was skipped.
  struct Requirement {
    const char* Name;
    bool Required;
    bool Available;
  };
  const Requirement Requirements[] = {
    {"AVX", Loader.RequiresAVX(), SupportsAVX},
    // RDRAND and RDSEED: either the host's FEAT_RNG or FEX's software generator
    // (FEX_SOFTWARERNG), which the guest CPUID then advertises.
    {"RAND", Loader.RequiresRAND(), HostFeatures.SupportsRAND || Feature.Feat_rand},
    {"SHA", Loader.RequiresSHA(), HostFeatures.SupportsSHA},
    {"CLZERO", Loader.RequiresCLZERO(), HostFeatures.SupportsCLZERO},
    {"AES256", Loader.RequiresAES256(), HostFeatures.SupportsAES256},
    {"AFP", Loader.RequiresAFP(), HostFeatures.SupportsAFP},
    {"AVX_NE_CONVERT (host BF16)", Loader.RequiresAVXNEConvert(), HostFeatures.SupportsBF16},
    {"3DNOW", Loader.Requires3DNow(), Feature.Feat_3dnow},
    {"SSE4A", Loader.RequiresSSE4A(), SupportsSSE4A},
    {"BMI1", Loader.RequiresBMI1(), Feature.Feat_bmi1},
    {"BMI2", Loader.RequiresBMI2(), Feature.Feat_bmi2},
    {"CLWB", Loader.RequiresCLWB(), Feature.Feat_clwb},
    {"SSSE3", Loader.RequiresSSSE3(), Feature.Feat_ssse3},
    {"SSE4.1", Loader.RequiresSSE4_1(), Feature.Feat_sse4_1},
    {"SSE4.2", Loader.RequiresSSE4_2(), Feature.Feat_sse4_2},
    {"AES", Loader.RequiresAES(), Feature.Feat_aes},
    {"PCLMUL", Loader.RequiresPCLMUL(), Feature.Feat_pclmulqdq},
    {"MOVBE", Loader.RequiresMOVBE(), Feature.Feat_movbe},
    {"ADX", Loader.RequiresADX(), Feature.Feat_adx},
    {"XSAVE", Loader.RequiresXSAVE(), Feature.Feat_xsave},
    {"RDPID", Loader.RequiresRDPID(), Feature.Feat_rdpid},
    {"CLFLOPT", Loader.RequiresCLFLOPT(), Feature.Feat_clflopt},
    {"FSGSBASE", Loader.RequiresFSGSBase(), Feature.Feat_fsgsbase},
    {"AVX_VNNI", Loader.RequiresAVXVNNI(), Feature.Feat_avx_vnni},
    {"SERIALIZE", Loader.RequiresSerialize(), Feature.Feat_serialize},
    {"AVX_NE_CONVERT", Loader.RequiresAVXNEConvert(), Feature.Feat_avx_ne_convert},
    {"SHA512", Loader.RequiresSHA512(), Feature.Feat_sha512},
    {"AVX_IFMA", Loader.RequiresAVXIFMA(), Feature.Feat_avx_ifma},
    {"AVX_VNNI_INT8", Loader.RequiresAVXVNNIINT8(), Feature.Feat_avx_vnni_int8},
    {"AVX_VNNI_INT16", Loader.RequiresAVXVNNIINT16(), Feature.Feat_avx_vnni_int16},
  };

  fextl::string Missing;
  for (const auto& Item : Requirements) {
    if (Item.Required && !Item.Available) {
      Missing += Missing.empty() ? "" : ", ";
      Missing += Item.Name;
    }
  }

  if (!Missing.empty()) {
    fextl::fmt::print("Skipped: the host or the translator lacks a CPU feature the test needs: {}\n", Missing);
    return ExitSkip;
  }

  auto SignalDelegation = FEX::DummyHandlers::CreateSignalDelegator();
  auto SyscallHandler = fextl::make_unique<RunnerSyscallHandler>();
  CTX->SetSignalDelegator(SignalDelegation.get());
  CTX->SetSyscallHandler(SyscallHandler.get());
  if (!CTX->InitCore()) {
    fextl::fmt::print("InitCore failed\n");
    return ExitFail;
  }

  GuestMapper Mapper;
  auto DoMmap = [&](uint64_t Address, size_t Size) -> void* {
    return Mapper.Map(Address, Size);
  };
  if (!Loader.MapMemory(DoMmap) || Mapper.Failed) {
    fextl::fmt::print("Failed to map the test's memory\n");
    return ExitFail;
  }

  auto Thread = CTX->CreateThread();
  if (!Thread) {
    return ExitFail;
  }

  if (!FEX::CoreThreadSetup::AllocateCallRetStack(Thread)) {
    fextl::fmt::print("Could not allocate the call and return stack\n");
    return ExitFail;
  }

  auto* GDT = FEX::CoreThreadSetup::FullDescriptorTable();
  {
    auto Frame = Thread->CurrentFrame;
    Frame->State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = &GDT[0];
    Frame->State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = &GDT[0];
    Frame->State.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
    auto CodeSegment = FEXCore::Core::CPUState::GetSegmentFromIndex(Frame->State, Frame->State.cs_idx);
    FEXCore::Core::CPUState::SetGDTBase(CodeSegment, 0);
    FEXCore::Core::CPUState::SetGDTLimit(CodeSegment, 0xF'FFFFU);
    CodeSegment->L = 1;
    CodeSegment->D = 0;
    Frame->State.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*CodeSegment);

    Frame->Pointers.SynchronousExceptionHandler = reinterpret_cast<uintptr_t>(&SynchronousExceptionHandler);
    Frame->State.rip = Loader.DefaultRIP();
  }

#if defined(__APPLE__) && defined(__aarch64__)
  FEX::MacOSHostFaults::Install();
  FEX::MacOSHostFaults::CurrentThread = Thread;
#endif

  if (!setjmp(LongJump)) {
    CTX->ExecuteThread(Thread);
  }

  FEXCore::Core::CPUState State;
  memcpy(&State, &Thread->CurrentFrame->State, sizeof(State));

  if (SupportsAVX) {
    __uint128_t XMM_Low[FEXCore::Core::CPUState::NUM_XMMS];
    __uint128_t YMM_High[FEXCore::Core::CPUState::NUM_XMMS];
    CTX->ReconstructXMMRegisters(Thread, XMM_Low, YMM_High);
    for (size_t i = 0; i < FEXCore::Core::CPUState::NUM_XMMS; ++i) {
      memcpy(&State.xmm.avx.data[i][0], &XMM_Low[i], sizeof(__uint128_t));
      memcpy(&State.xmm.avx.data[i][2], &YMM_High[i], sizeof(__uint128_t));
    }
  } else {
    CTX->ReconstructXMMRegisters(Thread, reinterpret_cast<__uint128_t*>(State.xmm.sse.data), nullptr);
  }

  const bool Passed = !DidFault && Loader.CompareStates(&State, nullptr, SupportsAVX);
  fextl::fmt::print("Faulted? {}\nPassed? {}\n", DidFault ? "Yes" : "No", Passed ? "Yes" : "No");
  fflush(nullptr);
  // Skip teardown: the result is known and the process is about to exit.
  _exit(Passed ? ExitPass : ExitFail);
}
