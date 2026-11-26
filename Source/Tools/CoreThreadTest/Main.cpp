// SPDX-License-Identifier: MIT
//
// Runs the same guest loop on several host threads at once, each with its own guest
// registers, for long enough that the system switches between them many times.
//
// It checks two things the single-thread smoke test cannot:
//  - Threads sharing one translator context do not disturb each other: each thread sums
//    a different count and must get its own answer.
//  - Guest state survives being switched out. macOS may clear register x18 when it
//    switches threads, so translated code must never keep anything there.
//
// Each thread stops at a `hlt`, which the core reports through the synchronous exception
// handler. No host signal is involved.

#include "Common/HostFeatures.h"
#include "CoreThreadSetup.h"
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

#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <sys/mman.h>
#include <thread>
#include <vector>

namespace {
// clang-format off
constexpr uint8_t GuestProgram[] = {
  // loop:
  0x48, 0x01, 0xC8, // add rax, rcx
  0x48, 0xFF, 0xC9, // dec rcx
  0x75, 0xF8,       // jnz loop
  0xF4,             // hlt
};
// clang-format on

void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  fprintf(stderr, "[%s] %s\n", LogMan::DebugLevelStr(Level), Message);
}
void AssertHandler(const char* Message) {
  fprintf(stderr, "[ASSERT] %s\n", Message);
}

class NullSyscallHandler final : public FEXCore::HLE::SyscallHandler, public FEXCore::Allocator::FEXAllocOperators {
public:
  void HandleSyscall(FEXCore::Core::CpuStateFrame*) override {}
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return {0, UINT64_MAX, true};
  }
};

thread_local jmp_buf LongJump {};

[[noreturn]]
void SynchronousExceptionHandler(FEXCore::Context::Context*, FEXCore::Core::CpuStateFrame*) {
  longjmp(LongJump, 1);
}

struct Worker {
  uint64_t Count {};
  uint64_t Result {};
  uint64_t FinalCounter {};
  bool Started {};
};

std::mutex CreateMutex;

void Run(FEXCore::Context::Context* CTX, uint64_t Code, Worker* Self) {
  FEXCore::Allocator::InitializeThread();

  FEXCore::Core::InternalThreadState* Thread {};
  {
    std::lock_guard Lock {CreateMutex};
    Thread = CTX->CreateThread();
  }
  if (!Thread || !FEX::CoreThreadSetup::AllocateCallRetStack(Thread)) {
    return;
  }

  auto Frame = Thread->CurrentFrame;
  auto* GDT = FEX::CoreThreadSetup::FullDescriptorTable();
  Frame->State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = GDT;
  Frame->State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = GDT;
  Frame->State.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
  auto CodeSegment = FEXCore::Core::CPUState::GetSegmentFromIndex(Frame->State, Frame->State.cs_idx);
  {
    // The descriptor table is shared, so set the code segment up under the lock.
    std::lock_guard Lock {CreateMutex};
    FEXCore::Core::CPUState::SetGDTBase(CodeSegment, 0);
    FEXCore::Core::CPUState::SetGDTLimit(CodeSegment, 0xF'FFFFU);
    CodeSegment->L = 1;
    CodeSegment->D = 0;
  }
  Frame->State.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*CodeSegment);
  Frame->Pointers.SynchronousExceptionHandler = reinterpret_cast<uintptr_t>(&SynchronousExceptionHandler);

  Frame->State.rip = Code;
  Frame->State.gregs[FEXCore::X86State::REG_RAX] = 0;
  Frame->State.gregs[FEXCore::X86State::REG_RCX] = Self->Count;
  Self->Started = true;

  if (!setjmp(LongJump)) {
    CTX->ExecuteThread(Thread);
  }

  Self->Result = Thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RAX];
  Self->FinalCounter = Thread->CurrentFrame->State.gregs[FEXCore::X86State::REG_RCX];
}
} // namespace

int main(int argc, char** argv) {
  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);

  FEXCore::Config::Initialize();
  FEXCore::Config::Load();
  FEXCore::Config::ReloadMetaLayer();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
  FEXCore::Config::Set(FEXCore::Config::CONFIG_DISABLETELEMETRY, "1");

  const unsigned ThreadCount = argc > 1 ? static_cast<unsigned>(atoi(argv[1])) : 8;
  const uint64_t BaseCount = argc > 2 ? strtoull(argv[2], nullptr, 0) : 600'000'000ULL;

  auto* Code = static_cast<uint8_t*>(::mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (Code == MAP_FAILED) {
    fprintf(stderr, "FAIL: could not allocate guest memory\n");
    return 1;
  }
  memcpy(Code, GuestProgram, sizeof(GuestProgram));

  fextl::unique_ptr<FEXCore::Context::Context> CTX;
  {
    auto HostFeatures = FEX::FetchHostFeatures(true);
    CTX = FEXCore::Context::Context::CreateNewContext(HostFeatures);
  }
  auto SignalDelegation = FEX::DummyHandlers::CreateSignalDelegator();
  auto SyscallHandler = fextl::make_unique<NullSyscallHandler>();
  CTX->SetSignalDelegator(SignalDelegation.get());
  CTX->SetSyscallHandler(SyscallHandler.get());
  if (!CTX->InitCore()) {
    fprintf(stderr, "FAIL: InitCore returned false\n");
    return 1;
  }

  std::vector<Worker> Workers(ThreadCount);
  std::vector<std::thread> Threads;
  for (unsigned i = 0; i < ThreadCount; ++i) {
    Workers[i].Count = BaseCount + i * 1'000'003ULL;
    Threads.emplace_back(Run, CTX.get(), reinterpret_cast<uint64_t>(Code), &Workers[i]);
  }
  for (auto& Thread : Threads) {
    Thread.join();
  }

  int Failures = 0;
  for (unsigned i = 0; i < ThreadCount; ++i) {
    const uint64_t N = Workers[i].Count;
    const uint64_t Expected = (N % 2 == 0) ? (N / 2) * (N + 1) : N * ((N + 1) / 2);
    const bool Passed = Workers[i].Started && Workers[i].Result == Expected && Workers[i].FinalCounter == 0;
    printf("thread %u: summed 1 to %llu, got %llu, expected %llu: %s\n", i, static_cast<unsigned long long>(N),
           static_cast<unsigned long long>(Workers[i].Result), static_cast<unsigned long long>(Expected), Passed ? "ok" : "WRONG");
    Failures += Passed ? 0 : 1;
  }
  printf("%s: %u threads, %d wrong\n", Failures ? "FAIL" : "PASS", ThreadCount, Failures);
  fflush(stdout);
  _exit(Failures ? 1 : 0);
}
