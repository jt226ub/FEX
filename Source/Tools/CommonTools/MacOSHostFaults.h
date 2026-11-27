// SPDX-License-Identifier: MIT
//
// SUPERSEDED by the fexmac library (Source/Embedding, fexmac.h), which owns this duty now.
// No tool on this branch uses this header. It is kept only so that tools on branches
// written before the library existed still build when merged; port them to fexmac.h
// and delete this file.
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
#pragma once

#if defined(__APPLE__) && defined(__aarch64__)

#include <FEXCore/Core/Context.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LongJump.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <signal.h>
#include <sys/ucontext.h>

namespace FEX::MacOSHostFaults {

inline thread_local FEXCore::Core::InternalThreadState* CurrentThread {};
inline struct sigaction PreviousBusAction {};
inline struct sigaction PreviousSegvAction {};

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

  // macOS reports a protection fault as SIGBUS with si_code BUS_ADRALN too, so only the
  // data abort's fault status code (ESR DFSC 0x21) identifies an alignment fault. See
  // Source/Embedding/Faults.cpp.
  const uint32_t ESR = Context->uc_mcontext->__es.__esr;
  const bool AlignmentFault = ((ESR >> 26) == 0x24 || (ESR >> 26) == 0x25) && (ESR & 0x3F) == 0x21;
  if (Thread && Signal == SIGBUS && AlignmentFault && Thread->CTX->IsAddressInCodeBuffer(Thread, PC)) {
    const auto Result = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
      Thread, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, PC, &State.__x[0]);
    if (Result.has_value()) {
      State.__pc = PC + *Result;
      return;
    }
  }

  // Not ours: put the previous behaviour back and return, so the fault happens again
  // and is reported normally.
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
