// SPDX-License-Identifier: MIT
//
// Host fault handling for translated code. See "Host fault handling" in fexmac.h for the
// contract with the embedder and docs/macOS-embedding-interface.md for the reasoning.
//
// fexmac_handle_fault runs a fixed sequence of checks. Each claims a fault only when it
// is certain the fault is the core's; anything left over is the embedder's. Work that
// later packages add goes into this sequence, at the points marked below:
//  - Package D: the interrupt fault page (a write to Thread->InterruptFaultPage after it
//    is made read-only, to stop a thread from another thread); write faults on pages
//    protected for self-modifying-code detection; guest faults (guest access to
//    unmapped or protected memory), which need the guest state reconstructed from the
//    host context before they are reported as a stop.
//  - Package F: nothing expected; cached code runs from the same code buffers.
#include "Internal.h"
#include "fexmac_testing.h"

#include <FEXCore/Core/Context.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LongJump.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <atomic>
#include <cstddef>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

namespace fexmac {
namespace {
  // The handler's own nesting depth, per host thread. A pthread key rather than a C++
  // thread_local: on macOS the first access to a thread_local on a thread may allocate
  // memory, which must not happen in a signal handler that may have interrupted malloc.
  pthread_key_t DepthKey;
  pthread_once_t DepthKeyOnce = PTHREAD_ONCE_INIT;

  std::atomic<bool> Installed {};
  struct sigaction PreviousBusAction {};
  struct sigaction PreviousSegvAction {};

  // Testing hook: a page that is never accessible, written by the handler on purpose.
  std::atomic<volatile uint32_t*> FaultInsideHandlerPage {};

  void WriteMessage(const char* Message) {
    size_t Length = 0;
    while (Message[Length]) {
      ++Length;
    }
    (void)!::write(STDERR_FILENO, Message, Length);
  }

  // The translator writes each block into a per-thread scratch buffer followed by an
  // inaccessible page. Running into it means the buffer was too small: resume the thread
  // at the translator's restart point, which retries with a larger buffer. On macOS the
  // inaccessible page is a whole host page placed at the next host page boundary after
  // JITGuardPage, so the fault can land anywhere from JITGuardPage to the end of it.
  bool HandleScratchBufferOverflow(FEXCore::Core::InternalThreadState* Thread, siginfo_t* Info, ucontext_t* Context) {
    if (!Thread->JITGuardPage) {
      return false;
    }
    const uintptr_t HostPage = FEXCore::Utils::FEX_HOST_PAGE_SIZE;
    const uintptr_t GuardBegin = Thread->JITGuardPage;
    const uintptr_t GuardEnd = ((Thread->JITGuardPage + FEXCore::Utils::FEX_PAGE_SIZE + HostPage - 1) & ~(HostPage - 1)) + HostPage;
    const auto Address = reinterpret_cast<uintptr_t>(Info->si_addr);
    if (Address < GuardBegin || Address >= GuardEnd) {
      return false;
    }
    auto& State = Context->uc_mcontext->__ss;
    FEXCore::UncheckedLongJump::ManuallyLoadJumpBuf(Thread->RestartJump, Thread->JITGuardOverflowArgument, &State.__x[0],
                                                    &Context->uc_mcontext->__ns.__v[0], &State.__pc);
    return true;
  }

  // ARM64 atomic instructions fault on an address that is not a multiple of the operand
  // size (on Apple silicon: when the access crosses a 16-byte boundary), where x86 allows
  // it. macOS reports that as SIGBUS with BUS_ADRALN. The core rewrites the instruction
  // sequence and execution resumes after it.
  bool HandleMisalignedAtomic(FEXCore::Core::InternalThreadState* Thread, int Signal, siginfo_t* Info, ucontext_t* Context) {
    auto& State = Context->uc_mcontext->__ss;
    const uint64_t PC = State.__pc;
    if (Signal != SIGBUS || Info->si_code != BUS_ADRALN || !Thread->CTX->IsAddressInCodeBuffer(Thread, PC)) {
      return false;
    }
    const auto Result = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
      Thread, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, PC, &State.__x[0]);
    if (!Result.has_value()) {
      return false;
    }
    State.__pc = PC + *Result;
    return true;
  }

  void LibraryHandler(int Signal, siginfo_t* Info, void* RawContext) {
    pthread_once(&DepthKeyOnce, [] { pthread_key_create(&DepthKey, nullptr); });
    const auto Depth = reinterpret_cast<uintptr_t>(pthread_getspecific(DepthKey));
    if (Depth != 0) {
      // A fault inside this handler. The handler is installed with SA_NODEFER, so the
      // nested fault reaches here instead of hanging the thread with the signal blocked.
      // Put the default action back and return: the instruction faults again and the
      // process dies with the usual signal, where a debugger or crash report shows it.
      WriteMessage("fexmac: fault inside the fault handler; restoring the default action\n");
      struct sigaction Default {};
      Default.sa_handler = SIG_DFL;
      sigemptyset(&Default.sa_mask);
      sigaction(Signal, &Default, nullptr);
      return;
    }
    pthread_setspecific(DepthKey, reinterpret_cast<void*>(Depth + 1));

    if (auto* Page = FaultInsideHandlerPage.load()) {
      *Page = 1;
    }

    const bool Handled = fexmac_handle_fault(Signal, Info, RawContext) != 0;
    pthread_setspecific(DepthKey, nullptr);
    if (Handled) {
      return;
    }

    // Not the library's: pass it to whatever was installed before.
    const struct sigaction& Previous = Signal == SIGBUS ? PreviousBusAction : PreviousSegvAction;
    if ((Previous.sa_flags & SA_SIGINFO) && Previous.sa_sigaction) {
      Previous.sa_sigaction(Signal, Info, RawContext);
      return;
    }
    if (!(Previous.sa_flags & SA_SIGINFO) && Previous.sa_handler != SIG_DFL && Previous.sa_handler != SIG_IGN) {
      Previous.sa_handler(Signal);
      return;
    }
    // Default or ignored: restore the default action and let the instruction fault
    // again. (Ignoring a synchronous SIGSEGV or SIGBUS would only repeat the fault.)
    struct sigaction Default {};
    Default.sa_handler = SIG_DFL;
    sigemptyset(&Default.sa_mask);
    sigaction(Signal, &Default, nullptr);
  }
} // namespace
} // namespace fexmac

extern "C" {

int fexmac_handle_fault(int signal, siginfo_t* info, void* ucontext) {
  if ((signal != SIGBUS && signal != SIGSEGV) || !info || !ucontext) {
    return 0;
  }
  // Only faults on a host thread that is running guest code can be the core's.
  auto* Current = fexmac::GetCurrentThread();
  if (!Current) {
    return 0;
  }
  auto* Thread = Current->Thread;
  auto* Context = static_cast<ucontext_t*>(ucontext);

  // macOS keeps x0 to x28 in an array followed directly by the frame pointer (x29),
  // link register (x30) and stack pointer, so the array serves as x0 to x31 for the
  // core's helpers.
  static_assert(offsetof(__darwin_arm_thread_state64, __fp) == 29 * sizeof(uint64_t));
  static_assert(offsetof(__darwin_arm_thread_state64, __lr) == 30 * sizeof(uint64_t));
  static_assert(offsetof(__darwin_arm_thread_state64, __sp) == 31 * sizeof(uint64_t));

  if (fexmac::HandleScratchBufferOverflow(Thread, info, Context)) {
    return 1;
  }
  if (fexmac::HandleMisalignedAtomic(Thread, signal, info, Context)) {
    return 1;
  }
  // Package D adds its checks here (see the top of this file).
  return 0;
}

fexmac_result fexmac_install_fault_handlers(void) {
  if (fexmac::Installed.exchange(true)) {
    return FEXMAC_OK;
  }
  // Create the per-thread keys now rather than inside the first signal.
  (void)fexmac::GetCurrentThread();
  pthread_once(&fexmac::DepthKeyOnce, [] { pthread_key_create(&fexmac::DepthKey, nullptr); });
  struct sigaction Action {};
  Action.sa_sigaction = fexmac::LibraryHandler;
  // SA_NODEFER: see LibraryHandler. SA_ONSTACK: use the host's alternate signal stack if
  // it has one, so a fault on an exhausted stack can still be reported.
  Action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
  sigemptyset(&Action.sa_mask);
  if (sigaction(SIGBUS, &Action, &fexmac::PreviousBusAction) != 0 || sigaction(SIGSEGV, &Action, &fexmac::PreviousSegvAction) != 0) {
    fexmac::Installed.store(false);
    return FEXMAC_ERROR_UNSUPPORTED;
  }
  return FEXMAC_OK;
}

void fexmac_testing_fault_inside_handler(int enable) {
  if (!enable) {
    fexmac::FaultInsideHandlerPage.store(nullptr);
    return;
  }
  void* Page = ::mmap(nullptr, FEXCore::Utils::FEX_HOST_PAGE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Page != MAP_FAILED) {
    fexmac::FaultInsideHandlerPage.store(static_cast<volatile uint32_t*>(Page));
  }
}

} // extern "C"
