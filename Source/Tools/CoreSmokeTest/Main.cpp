// SPDX-License-Identifier: MIT
//
// Smallest end-to-end check of the translator core: translate a short x86-64
// program, run it, and confirm the result it hands back.
//
// The guest program sums 1 to 100 in a loop and then makes a system call with the
// sum as its first argument. Reaching the system call handler with 5050 proves that
// the dispatcher was emitted and runs, that blocks are compiled and copied into
// executable memory, and that guest state reaches the host intact. Multiblock is
// turned off so the loop's backward branch goes through the block linker, which
// patches code that has already been made executable.
//
// The program never faults and needs no signal handling, so it only depends on the
// core. It exits from inside the system call handler.

#include "Common/HostFeatures.h"
#include "DummyHandlers.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/fextl/memory.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <sys/mman.h>
#include <unistd.h>

namespace {
constexpr uint64_t ExpectedSum = 5050;

// clang-format off
constexpr uint8_t GuestProgram[] = {
  0x31, 0xC0,                   // xor eax, eax
  0xB9, 0x64, 0x00, 0x00, 0x00, // mov ecx, 100
  // loop:
  0x01, 0xC8,                   // add eax, ecx
  0xFF, 0xC9,                   // dec ecx
  0x75, 0xFA,                   // jnz loop
  0x89, 0xC7,                   // mov edi, eax
  0xB8, 0x3C, 0x00, 0x00, 0x00, // mov eax, 60
  0x0F, 0x05,                   // syscall
  0xF4,                         // hlt (never reached)
};
// clang-format on

void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  fprintf(stderr, "[%s] %s\n", LogMan::DebugLevelStr(Level), Message);
  fflush(stderr);
}

void AssertHandler(const char* Message) {
  fprintf(stderr, "[ASSERT] %s\n", Message);
  fflush(stderr);
}

class SmokeSyscallHandler final : public FEXCore::HLE::SyscallHandler, public FEXCore::Allocator::FEXAllocOperators {
public:
  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override {
    const uint64_t Number = Frame->State.gregs[FEXCore::X86State::REG_RAX];
    const uint64_t Argument = Frame->State.gregs[FEXCore::X86State::REG_RDI];
    const uint64_t Counter = Frame->State.gregs[FEXCore::X86State::REG_RCX];

    printf("Guest made system call %llu with argument %llu (loop counter %llu)\n", static_cast<unsigned long long>(Number),
           static_cast<unsigned long long>(Argument), static_cast<unsigned long long>(Counter));

    const bool Passed = Number == 60 && Argument == ExpectedSum && Counter == 0;
    printf("%s: expected system call 60 with argument %llu and loop counter 0\n", Passed ? "PASS" : "FAIL",
           static_cast<unsigned long long>(ExpectedSum));
    fflush(stdout);
    _exit(Passed ? 0 : 1);
  }

  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }

  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return {0, UINT64_MAX, true};
  }
};
} // namespace

int main() {
  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);

  FEXCore::Config::Initialize();
  FEXCore::Config::Load();
  FEXCore::Config::ReloadMetaLayer();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_MULTIBLOCK, "0");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");

  // Guest memory is ordinary host memory: the guest's addresses are the host's.
  constexpr size_t CodeSize = 0x10000;
  constexpr size_t StackSize = 0x10000;
  auto* Code = static_cast<uint8_t*>(::mmap(nullptr, CodeSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  auto* Stack = static_cast<uint8_t*>(::mmap(nullptr, StackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (Code == MAP_FAILED || Stack == MAP_FAILED) {
    fprintf(stderr, "FAIL: could not allocate guest memory\n");
    return 1;
  }
  memcpy(Code, GuestProgram, sizeof(GuestProgram));

  printf("Host page size: %ld\n", sysconf(_SC_PAGESIZE));
  printf("Creating translator context\n");
  fflush(stdout);

  fextl::unique_ptr<FEXCore::Context::Context> CTX;
  {
    auto HostFeatures = FEX::FetchHostFeatures(true);
    CTX = FEXCore::Context::Context::CreateNewContext(HostFeatures);
  }

  auto SignalDelegation = FEX::DummyHandlers::CreateSignalDelegator();
  auto SyscallHandler = fextl::make_unique<SmokeSyscallHandler>();
  CTX->SetSignalDelegator(SignalDelegation.get());
  CTX->SetSyscallHandler(SyscallHandler.get());

  if (!CTX->InitCore()) {
    fprintf(stderr, "FAIL: InitCore returned false\n");
    return 1;
  }
  printf("Core initialised, dispatcher emitted\n");
  fflush(stdout);

  auto Thread = CTX->CreateThread();

  FEXCore::Core::CPUState::gdt_segment GDT[32] {};
  {
    auto Frame = Thread->CurrentFrame;
    Frame->State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = &GDT[0];
    Frame->State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = &GDT[0];

    // A 64-bit code segment, as a 64-bit Linux process would have.
    Frame->State.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
    auto CodeSegment = FEXCore::Core::CPUState::GetSegmentFromIndex(Frame->State, Frame->State.cs_idx);
    FEXCore::Core::CPUState::SetGDTBase(CodeSegment, 0);
    FEXCore::Core::CPUState::SetGDTLimit(CodeSegment, 0xF'FFFFU);
    CodeSegment->L = 1;
    CodeSegment->D = 0;
    Frame->State.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*CodeSegment);

    Frame->State.rip = reinterpret_cast<uint64_t>(Code);
    Frame->State.gregs[FEXCore::X86State::REG_RSP] = reinterpret_cast<uint64_t>(Stack) + StackSize - 64;
  }

  printf("Running guest code at %p\n", static_cast<void*>(Code));
  fflush(stdout);

  CTX->ExecuteThread(Thread);

  // The system call handler exits the process, so getting here means the guest ran
  // off the end of its program without making the call.
  fprintf(stderr, "FAIL: guest execution returned without reaching the system call\n");
  return 1;
}
