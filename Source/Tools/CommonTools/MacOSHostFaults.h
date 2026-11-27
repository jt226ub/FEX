// SPDX-License-Identifier: MIT
//
// Host fault handling for programs that embed the translator core on macOS.
//
// The core does not install signal handlers itself; whatever embeds it must catch the
// faults that translated code is designed to take and pass them back to the core. On
// Linux that is FEX's process emulation layer. This is the macOS counterpart for the
// test tools, and the starting point for a real embedder.
//
// Handled so far:
//  - Misaligned atomic memory operations. ARM64 atomic instructions fault on an
//    address that is not a multiple of the operand size, where x86 allows it. macOS
//    reports that as SIGBUS with code BUS_ADRALN. The core rewrites the instruction
//    sequence and execution resumes.
//  - The translator's scratch buffer filling up. The translator writes each block into a
//    per-thread scratch buffer that ends in an inaccessible page. Running into that page
//    is how it learns the buffer was too small: the embedder must resume the thread at
//    the translator's restart point, which then retries with a larger buffer. macOS
//    reports the write as SIGBUS (or SIGSEGV for an unmapped address).
// Any other fault is reported on stdout with its address and program counter, then
// allowed to crash the process as usual.
#pragma once

#if defined(__APPLE__) && defined(__aarch64__)

#include <FEXCore/Core/Context.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LongJump.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <signal.h>
#include <sys/ucontext.h>
#include <unistd.h>

namespace FEX::MacOSHostFaults {

inline thread_local FEXCore::Core::InternalThreadState* CurrentThread {};
inline struct sigaction PreviousBusAction {};
inline struct sigaction PreviousSegvAction {};

// Writes "Unhandled host fault: signal N at 0x... (pc 0x...)" to stdout. Only write(2)
// is used: this runs inside a signal handler, where formatted printing is unsafe.
inline void ReportUnhandled(int Signal, uint64_t Address, uint64_t PC) {
  char Buffer[96];
  size_t Length = 0;
  auto Append = [&](const char* Text) {
    while (*Text && Length < sizeof(Buffer)) {
      Buffer[Length++] = *Text++;
    }
  };
  auto AppendHex = [&](uint64_t Value) {
    Append("0x");
    for (int Shift = 60; Shift >= 0; Shift -= 4) {
      if (Length < sizeof(Buffer)) {
        Buffer[Length++] = "0123456789abcdef"[(Value >> Shift) & 0xF];
      }
    }
  };
  Append("Unhandled host fault: signal ");
  if (Signal >= 10 && Length < sizeof(Buffer)) {
    Buffer[Length++] = static_cast<char>('0' + Signal / 10);
  }
  if (Length < sizeof(Buffer)) {
    Buffer[Length++] = static_cast<char>('0' + Signal % 10);
  }
  Append(" at ");
  AppendHex(Address);
  Append(" (pc ");
  AppendHex(PC);
  Append(")\n");
  (void)!write(STDOUT_FILENO, Buffer, Length);
}

inline void FaultHandler(int Signal, siginfo_t* Info, void* RawContext) {
  auto* Context = static_cast<ucontext_t*>(RawContext);
  // macOS keeps x0 to x28 in an array followed directly by the frame pointer (x29) and
  // link register (x30), so the array address serves as a pointer to x0 through x30.
  auto& State = Context->uc_mcontext->__ss;
  static_assert(offsetof(__darwin_arm_thread_state64, __fp) == 29 * sizeof(uint64_t));
  static_assert(offsetof(__darwin_arm_thread_state64, __lr) == 30 * sizeof(uint64_t));
  // The stack pointer follows as entry 31, which the restart below relies on.
  static_assert(offsetof(__darwin_arm_thread_state64, __sp) == 31 * sizeof(uint64_t));

  auto* Thread = CurrentThread;
  const uint64_t PC = State.__pc;

  if (Thread && Thread->JITGuardPage) {
    // JITGuardPage is where the translator stops using the buffer. On macOS the
    // inaccessible page is a whole host page placed after the buffer, at the next host
    // page boundary, so the fault can land anywhere from JITGuardPage to the end of it.
    const uintptr_t HostPage = FEXCore::Utils::FEX_HOST_PAGE_SIZE;
    const uintptr_t GuardBegin = Thread->JITGuardPage;
    const uintptr_t GuardEnd = ((Thread->JITGuardPage + FEXCore::Utils::FEX_PAGE_SIZE + HostPage - 1) & ~(HostPage - 1)) + HostPage;
    const auto Address = reinterpret_cast<uintptr_t>(Info->si_addr);
    if (Address >= GuardBegin && Address < GuardEnd) {
      FEXCore::UncheckedLongJump::ManuallyLoadJumpBuf(Thread->RestartJump, Thread->JITGuardOverflowArgument, &State.__x[0],
                                                      &Context->uc_mcontext->__ns.__v[0], &State.__pc);
      return;
    }
  }

  if (Thread && Signal == SIGBUS && Info->si_code == BUS_ADRALN && Thread->CTX->IsAddressInCodeBuffer(Thread, PC)) {
    const auto Result = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
      Thread, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, PC, &State.__x[0]);
    if (Result.has_value()) {
      State.__pc = PC + *Result;
      return;
    }
  }

  // Not ours: say where it happened, so a crashing test names the address it reached
  // (for example a guest address cut to 32 bits), then put the previous behaviour back
  // and return, so the fault happens again and is reported normally.
  ReportUnhandled(Signal, reinterpret_cast<uint64_t>(Info->si_addr), PC);
  sigaction(Signal, Signal == SIGBUS ? &PreviousBusAction : &PreviousSegvAction, nullptr);
}

// Call once per process, and set CurrentThread on each thread that runs guest code.
inline void Install() {
  struct sigaction Action {};
  Action.sa_sigaction = FaultHandler;
  Action.sa_flags = SA_SIGINFO;
  sigemptyset(&Action.sa_mask);
  sigaction(SIGBUS, &Action, &PreviousBusAction);
  sigaction(SIGSEGV, &Action, &PreviousSegvAction);
}

} // namespace FEX::MacOSHostFaults

#endif
