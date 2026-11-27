// SPDX-License-Identifier: MIT
//
// Runs one of FEX's assembly instruction tests against the translator core alone.
//
// Upstream's TestHarnessRunner does the same job but is built on the Linux process
// emulation layer, which the macOS port does not have. This runner needs only the core:
// it maps the test's memory, runs the guest code until it halts, and compares the final
// register state with the values the test file declares.
//
// Thread setup, running guest code, fault handling and invalidation go through the
// fexmac interface (fexmac.h). What still uses the core's C++ headers is harness work
// only: reading the test files and comparing registers (HarnessHelpers.h, which is laid
// out on the core's CPUState), and the host-feature checks that decide whether a test
// can run here (Common/HostFeatures.h, Common/X86Features.h).
//
// Usage: FEXCoreASMTestRunner <test.bin> <test.config.bin>
// Exit code: 0 pass, 1 fail, 125 skipped (the test needs something this host lacks).

#include "Common/HostFeatures.h"
#include "Common/X86Features.h"
#include "HarnessHelpers.h"

#include <fexmac.h>

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>
#include <FEXHeaderUtils/Filesystem.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <ctime>
#include <signal.h>
#include <set>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <sys/ucontext.h>
#endif

namespace {
constexpr int ExitPass = 0;
constexpr int ExitFail = 1;
constexpr int ExitSkip = 125;

void LogHandler(fexmac_log_level Level, const char* Message, void*) {
  if (Level == FEXMAC_LOG_ASSERT) {
    fextl::fmt::print("A {}\n", Message);
    fflush(nullptr);
    return;
  }
  fextl::fmt::print("{} {}\n", LogMan::DebugLevelStr(Level), Message);
}

// Options for the core, as fexmac takes them: the test file's own options first, then
// FEX_* environment variables, which override them (the order of the core's
// configuration layers that upstream's runner relies on). Names the core does not know
// are ignored, as the core's own loaders ignore them.
class OptionList {
public:
  void Add(std::string_view Key, std::string_view Value) {
    if (!Key.starts_with("FEX_")) {
      return;
    }
    std::string Name {Key.substr(4)};
    if (!fexmac_option_known(Name.c_str())) {
      return;
    }
    Names.emplace_back(std::move(Name));
    Values.emplace_back(Value);
  }

  std::vector<fexmac_option> Get() const {
    std::vector<fexmac_option> Options;
    for (size_t i = 0; i < Names.size(); ++i) {
      Options.push_back({Names[i].c_str(), Values[i].c_str()});
    }
    return Options;
  }

private:
  std::vector<std::string> Names;
  std::vector<std::string> Values;
};

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

  // The mapped pages as [begin, end) ranges, for fexmac_add_code_range.
  std::vector<std::pair<uint64_t, uint64_t>> Ranges() const {
    std::vector<std::pair<uint64_t, uint64_t>> Out;
    for (uint64_t Page : Mapped) {
      if (!Out.empty() && Out.back().second == Page) {
        Out.back().second += PageSize;
      } else {
        Out.emplace_back(Page, Page + PageSize);
      }
    }
    return Out;
  }

private:
  uint64_t PageSize;
  std::set<uint64_t> Mapped;
};
// Whether [Address, Address + Size) is mapped writable in this process (guest addresses
// are host addresses). Used before writing a system call's result through a guest pointer.
bool IsWritable(uint64_t Address, uint64_t Size) {
#ifdef __APPLE__
  // The range must not wrap past the top of the address space (rdi near 2^64).
  if (Size == 0 || Address > UINT64_MAX - Size) {
    return false;
  }
  const uint64_t End = Address + Size;
  uint64_t Cursor = Address;
  while (Cursor < End) {
    mach_vm_address_t RegionAddress = Cursor;
    mach_vm_size_t RegionSize = 0;
    vm_region_basic_info_data_64_t Info {};
    mach_msg_type_number_t Count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t Object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &RegionAddress, &RegionSize, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&Info),
                       &Count, &Object) != KERN_SUCCESS ||
        RegionAddress > Cursor || !(Info.protection & VM_PROT_WRITE) || RegionAddress + RegionSize <= Cursor) {
      return false;
    }
    Cursor = RegionAddress + RegionSize;
  }
  return true;
#else
  return Address != 0;
#endif
}

// This runner has no Linux system calls in general (tests that need them declare it and
// are skipped). It answers `time` (201) honestly, as the one call a test makes
// (TwoByte/0F_02, macOS/syscall_time), and every other call fails with ENOSYS, as a Linux
// kernel answers an unknown number; either way the guest continues after the instruction.
// Without a callback the run would stop at the first `syscall` and the test would fail
// although the instruction worked.
fexmac_syscall_action SyscallHandler(fexmac_thread* Thread, void*) {
  constexpr uint64_t LinuxSysTime = 201;
  constexpr uint64_t LinuxENOSYS = 38;
  constexpr uint64_t LinuxEFAULT = 14;
  fexmac_x86_state State;
  fexmac_x86_state_init(&State);
  if (fexmac_thread_get_state(Thread, &State) != FEXMAC_OK) {
    // Nothing sensible to answer with; end the run, and the test fails.
    return FEXMAC_SYSCALL_STOP;
  }
  uint64_t& RAX = State.gpr[0];
  const uint64_t RDI = State.gpr[7];
  if (RAX == LinuxSysTime) {
    // time(time_t* tloc): seconds since the epoch, also stored through tloc if non-null.
    const uint64_t Now = static_cast<uint64_t>(::time(nullptr));
    if (RDI && !IsWritable(RDI, sizeof(uint64_t))) {
      RAX = -LinuxEFAULT;
    } else {
      if (RDI) {
        memcpy(reinterpret_cast<void*>(RDI), &Now, sizeof(Now));
      }
      RAX = Now;
    }
  } else {
    RAX = -LinuxENOSYS;
  }
  if (fexmac_thread_set_state(Thread, &State) != FEXMAC_OK) {
    return FEXMAC_SYSCALL_STOP;
  }
  return FEXMAC_SYSCALL_RESUME;
}

#ifdef __APPLE__
// The __TEXT ranges of every image loaded at startup (the runner with the core and the
// library linked in, and the system libraries), so the fault handler can say whether a
// faulting pc is in host code or, by elimination, in translated code. Collected before
// any guest code runs; the handler only reads the array.
struct CodeRange {
  uint64_t Begin;
  uint64_t End;
};
CodeRange HostCodeRanges[2048];
size_t HostCodeRangeCount {};

void CollectHostCodeRanges() {
  const uint32_t Count = _dyld_image_count();
  for (uint32_t i = 0; i < Count && HostCodeRangeCount < std::size(HostCodeRanges); ++i) {
    const auto* Header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(i));
    if (!Header) {
      continue;
    }
    unsigned long Size = 0;
    const auto* Text = getsegmentdata(Header, "__TEXT", &Size);
    if (Text && Size) {
      HostCodeRanges[HostCodeRangeCount++] = {reinterpret_cast<uint64_t>(Text), reinterpret_cast<uint64_t>(Text) + Size};
    }
  }
}

bool IsHostCode(uint64_t PC) {
  for (size_t i = 0; i < HostCodeRangeCount; ++i) {
    if (PC >= HostCodeRanges[i].Begin && PC < HostCodeRanges[i].End) {
      return true;
    }
  }
  return false;
}

thread_local int FaultHandlerDepth {};

// The runner owns SIGBUS and SIGSEGV ("way 2" in fexmac.h): each fault goes to
// fexmac_handle_fault first, and one the library does not claim is reported here and then
// kills the process with the original signal. The runner does not use
// fexmac_install_fault_handlers and chain to this, because the library's handler, when a
// previous handler returns without resolving the fault, returns too, and the instruction
// would fault again forever; and a chained handler must not change the process-wide
// action under the library. As the owner, this handler may: it restores the default
// action only when the process is about to die of this fault.
//
// The line names the fault so results.tsv records it: "runner: unclaimed SIGSEGV at
// address <a>, pc <p> in host|translated code (ESR class <c>, fault status <s>)". Only
// write(2) is used; this runs in a signal handler.
void FaultHandler(int Signal, siginfo_t* Info, void* RawContext) {
  auto Die = [Signal] {
    struct sigaction Default {};
    Default.sa_handler = SIG_DFL;
    sigemptyset(&Default.sa_mask);
    sigaction(Signal, &Default, nullptr);
    // Returning re-executes the faulting instruction, which now kills the process.
  };
  if (FaultHandlerDepth) {
    // Installed with SA_NODEFER, so a fault in here re-enters instead of hanging.
    constexpr char Message[] = "runner: fault inside the fault handler\n";
    (void)!write(STDOUT_FILENO, Message, sizeof(Message) - 1);
    Die();
    return;
  }
  ++FaultHandlerDepth;
  if (fexmac_handle_fault(Signal, Info, RawContext)) {
    --FaultHandlerDepth;
    return;
  }

  char Buffer[192];
  size_t Length = 0;
  auto Append = [&](const char* Text) {
    while (*Text && Length < sizeof(Buffer)) {
      Buffer[Length++] = *Text++;
    }
  };
  auto AppendHex = [&](uint64_t Value) {
    Append("0x");
    bool Started = false;
    for (int Shift = 60; Shift >= 0; Shift -= 4) {
      const auto Digit = (Value >> Shift) & 0xF;
      if (Digit || Started || Shift == 0) {
        Started = true;
        if (Length < sizeof(Buffer)) {
          Buffer[Length++] = "0123456789abcdef"[Digit];
        }
      }
    }
  };
  const auto* Context = static_cast<ucontext_t*>(RawContext);
  const uint64_t ESR = Context->uc_mcontext->__es.__esr;
  const uint64_t PC = Context->uc_mcontext->__ss.__pc;
  Append(Signal == SIGBUS ? "runner: unclaimed SIGBUS at address " : "runner: unclaimed SIGSEGV at address ");
  AppendHex(reinterpret_cast<uint64_t>(Info->si_addr));
  Append(", pc ");
  AppendHex(PC);
  Append(IsHostCode(PC) ? " in host code" : " in translated code");
  Append(" (ESR class ");
  AppendHex((ESR >> 26) & 0x3F);
  Append(", fault status ");
  AppendHex(ESR & 0x3F);
  Append(")\n");
  (void)!write(STDOUT_FILENO, Buffer, Length);
  --FaultHandlerDepth;
  Die();
}
#endif
} // namespace

int main(int argc, char** argv, char** const envp) {
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

  if (!Loader.Is64BitMode()) {
    fextl::fmt::print("Skipped: 32-bit tests are not supported by this runner yet\n");
    return ExitSkip;
  }
  if (Loader.RequiresLinux()) {
    fextl::fmt::print("Skipped: test needs Linux system calls\n");
    return ExitSkip;
  }

  OptionList Options;
  for (const auto& [Key, Value] : Loader.GetEnvironmentOptions()) {
    Options.Add(Key, Value);
  }
  for (char* const* Var = envp; Var && *Var; ++Var) {
    const std::string_view Entry {*Var};
    const auto Equals = Entry.find('=');
    if (Equals != std::string_view::npos) {
      Options.Add(Entry.substr(0, Equals), Entry.substr(Equals + 1));
    }
  }
  const auto OptionArray = Options.Get();

  // The code cache, switched on by the environment the way the core's own option names
  // would (Scripts/macos_asm_tests.py --cache-dir): FEX_DISKCACHE=1 and
  // FEX_DISKCACHEPATH=<dir>, turned into fexmac's config fields.
  const char* CacheEnv = getenv("FEX_DISKCACHE");
  const char* CachePath = getenv("FEX_DISKCACHEPATH");
  const bool UseCache = CacheEnv && std::string_view {CacheEnv} == "1";

  fexmac_config Config;
  fexmac_config_init(&Config);
  if (UseCache) {
    Config.code_cache_enabled = 1;
    Config.code_cache_directory = CachePath && *CachePath ? CachePath : nullptr;
  }
  Config.options = OptionArray.data();
  Config.option_count = OptionArray.size();
  Config.log = LogHandler;
  Config.syscall = SyscallHandler;

  fexmac_context* Context {};
  if (const auto Result = fexmac_context_create(&Config, &Context); Result != FEXMAC_OK) {
    fextl::fmt::print("Could not create the context: {}\n", fexmac_result_string(Result));
    return ExitFail;
  }

  // Whether the test can run here. The core's configuration is set up by now, so host
  // feature detection sees the same options the core does.
  auto HostFeatures = FEX::FetchHostFeatures(true);
  const bool SupportsAVX = HostFeatures.SupportsAVX();

  FEX::X86::Features Feature {[Context](uint32_t Function, uint32_t Leaf) {
    uint32_t Regs[4] {};
    fexmac_cpuid(Context, Function, Leaf, Regs);
    return FEXCore::CPUID::FunctionResults {.eax = Regs[0], .ebx = Regs[1], .ecx = Regs[2], .edx = Regs[3]};
  }};
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

  GuestMapper Mapper;
  auto DoMmap = [&](uint64_t Address, size_t Size) -> void* {
    return Mapper.Map(Address, Size);
  };
  if (!Loader.MapMemory(DoMmap) || Mapper.Failed) {
    fextl::fmt::print("Failed to map the test's memory\n");
    return ExitFail;
  }
  // Everything this runner mapped may hold code, and tests write to it. Registering it
  // makes the core treat only that memory as code (a jump elsewhere stops with a guest
  // fault), and the code cache hashes and patches guest bytes only inside it.
  for (const auto& [Begin, End] : Mapper.Ranges()) {
    fexmac_add_code_range(Context, Begin, End - Begin, 1);
  }

  fexmac_thread* Thread {};
  if (const auto Result = fexmac_thread_create(Context, &Thread); Result != FEXMAC_OK) {
    fextl::fmt::print("Could not create the thread: {}\n", fexmac_result_string(Result));
    return ExitFail;
  }
  fexmac_x86_state GuestState;
  fexmac_x86_state_init(&GuestState);
  if (const auto Result = fexmac_thread_get_state(Thread, &GuestState); Result != FEXMAC_OK) {
    fextl::fmt::print("Could not read the thread's state: {}\n", fexmac_result_string(Result));
    return ExitFail;
  }
  GuestState.rip = Loader.DefaultRIP();
  if (const auto Result = fexmac_thread_set_state(Thread, &GuestState); Result != FEXMAC_OK) {
    fextl::fmt::print("Could not set the thread's state: {}\n", fexmac_result_string(Result));
    return ExitFail;
  }

#ifdef __APPLE__
  {
    // Misaligned atomics and the translator's scratch-buffer restart need the library's
    // fault handling; FaultHandler passes every fault to it first.
    CollectHostCodeRanges();
    struct sigaction Action {};
    Action.sa_sigaction = FaultHandler;
    // As fexmac.h asks of a host that owns the signals: SA_NODEFER, so a fault while
    // handling one re-enters rather than hanging the thread.
    Action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigfillset(&Action.sa_mask);
    sigdelset(&Action.sa_mask, SIGSEGV);
    sigdelset(&Action.sa_mask, SIGBUS);
    sigdelset(&Action.sa_mask, SIGILL);
    sigdelset(&Action.sa_mask, SIGFPE);
    sigdelset(&Action.sa_mask, SIGTRAP);
    sigdelset(&Action.sa_mask, SIGSYS);
    if (sigaction(SIGSEGV, &Action, nullptr) != 0 || sigaction(SIGBUS, &Action, nullptr) != 0) {
      fextl::fmt::print("Could not install the fault handlers\n");
      return ExitFail;
    }
  }
#else
  if (const auto Result = fexmac_install_fault_handlers(); Result != FEXMAC_OK) {
    fextl::fmt::print("Could not install the fault handlers: {}\n", fexmac_result_string(Result));
    return ExitFail;
  }
#endif

  // The tests end with `hlt`. Anything else (a guest fault) means the test went wrong;
  // system calls are answered by SyscallHandler and the run continues.
  fexmac_stop Stop;
  fexmac_stop_init(&Stop);
  const auto RunResult = fexmac_thread_run(Thread, &Stop);
  const bool DidFault = RunResult != FEXMAC_OK || Stop.reason != FEXMAC_STOP_HALT;
  if (DidFault) {
    // Machine-readable, for Scripts/macos_asm_tests.py's expected-outcome check.
    fextl::fmt::print("Stopped: result {} reason {} signal {} trap {} si_code {} at 0x{:x} fault address 0x{:x}\n",
                      fexmac_result_string(RunResult), static_cast<int>(Stop.reason), Stop.signal, Stop.trap_number, Stop.si_code,
                      Stop.instruction_address, Stop.fault_address);
  }
  if (const auto Result = fexmac_thread_get_state(Thread, &GuestState); Result != FEXMAC_OK) {
    fextl::fmt::print("Could not read the thread's state: {}\n", fexmac_result_string(Result));
    return ExitFail;
  }

  // The harness compares registers laid out as the core's CPUState, with the vector
  // registers in the layout it expects for this host.
  FEXCore::Core::CPUState State {};
  State.rip = GuestState.rip;
  memcpy(State.gregs, GuestState.gpr, sizeof(State.gregs));
  State.fs_cached = GuestState.fs_base;
  State.gs_cached = GuestState.gs_base;
  memcpy(State.mm, GuestState.mm, sizeof(State.mm));
  for (size_t i = 0; i < FEXCore::Core::CPUState::NUM_XMMS; ++i) {
    if (SupportsAVX) {
      memcpy(&State.xmm.avx.data[i][0], GuestState.xmm[i], sizeof(GuestState.xmm[i]));
      memcpy(&State.xmm.avx.data[i][2], GuestState.ymm_high[i], sizeof(GuestState.ymm_high[i]));
    } else {
      memcpy(&State.xmm.sse.data[i][0], GuestState.xmm[i], sizeof(GuestState.xmm[i]));
    }
  }

  const bool Passed = !DidFault && Loader.CompareStates(&State, nullptr, SupportsAVX);

  if (UseCache) {
    // Wait for this run's translations to be written; the process skips teardown below.
    // The counters line is what Scripts/macos_asm_tests.py --cache-dir checks.
    fexmac_context_flush_cache(Context);
    fexmac_translation_counters Counters;
    fexmac_translation_counters_init(&Counters);
    fexmac_context_get_counters(Context, &Counters);
    fextl::fmt::print("Code cache: compiled {} loaded {} rejected {} queued {}\n", Counters.blocks_compiled, Counters.blocks_loaded,
                      Counters.cache_entries_rejected, Counters.cache_entries_queued);
    fextl::fmt::print("Code cache state: {} dropped {} replaced {} reset {}\n", static_cast<int>(Counters.cache_state),
                      Counters.cache_entries_dropped, Counters.untrusted_files_replaced, Counters.damaged_databases_reset);
  }
  fextl::fmt::print("Faulted? {}\nPassed? {}\n", DidFault ? "Yes" : "No", Passed ? "Yes" : "No");
  fflush(nullptr);
  // Skip teardown: the result is known and the process is about to exit.
  _exit(Passed ? ExitPass : ExitFail);
}
