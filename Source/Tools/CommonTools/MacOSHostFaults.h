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
#pragma once

#if defined(__APPLE__) && defined(__aarch64__)

#include <FEXCore/Core/Context.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>

#include <signal.h>
#include <sys/ucontext.h>

namespace FEX::MacOSHostFaults {

inline thread_local FEXCore::Core::InternalThreadState* CurrentThread {};
inline struct sigaction PreviousBusAction {};

inline void BusHandler(int Signal, siginfo_t* Info, void* RawContext) {
  auto* Context = static_cast<ucontext_t*>(RawContext);
  // macOS keeps x0 to x28 in an array followed directly by the frame pointer (x29) and
  // link register (x30), so the array address serves as a pointer to x0 through x30.
  auto& State = Context->uc_mcontext->__ss;
  static_assert(offsetof(__darwin_arm_thread_state64, __fp) == 29 * sizeof(uint64_t));
  static_assert(offsetof(__darwin_arm_thread_state64, __lr) == 30 * sizeof(uint64_t));

  auto* Thread = CurrentThread;
  const uint64_t PC = State.__pc;
  if (Thread && Info->si_code == BUS_ADRALN && Thread->CTX->IsAddressInCodeBuffer(Thread, PC)) {
    const auto Result = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
      Thread, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, PC, &State.__x[0]);
    if (Result.has_value()) {
      State.__pc = PC + *Result;
      return;
    }
  }

  // Not ours: put the previous behaviour back and return, so the fault happens again
  // and is reported normally.
  sigaction(Signal, &PreviousBusAction, nullptr);
}

// Call once per process, and set CurrentThread on each thread that runs guest code.
inline void Install() {
  struct sigaction Action {};
  Action.sa_sigaction = BusHandler;
  Action.sa_flags = SA_SIGINFO;
  sigemptyset(&Action.sa_mask);
  sigaction(SIGBUS, &Action, &PreviousBusAction);
}

} // namespace FEX::MacOSHostFaults

#endif
