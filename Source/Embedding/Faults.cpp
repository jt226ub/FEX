// SPDX-License-Identifier: MIT
//
// Host fault handling for translated code. See "Host fault handling" in fexmac.h for the
// contract with the embedder and docs/macOS-embedding-interface.md for the reasoning.
//
// Telling faults apart. macOS reports both a misaligned atomic access and an access to a
// protected page as SIGBUS with si_code 1 (BUS_ADRALN), so si_code says nothing here.
// The kernel passes the data abort's syndrome register in the signal context
// (uc_mcontext->__es.__esr); its exception class says whether this was a data abort and
// its fault status code (DFSC, bits 5:0) says which kind:
//   0x21              alignment fault
//   0x04 to 0x07      translation fault, level 0 to 3 (page not mapped, or never touched)
//   0x08 to 0x0B      access flag fault
//   0x0C to 0x0F      permission fault
// Measured on an Apple M4 (JITWriteProbe's fault-status cases): a load from a PROT_NONE
// page is a translation fault (SIGBUS), a store to a read-only page that was never read
// is a translation fault too, an atomic on a read-only page that was read is a permission
// fault, an unmapped address is a translation fault (SIGSEGV), and a misaligned atomic
// across 16 bytes is an alignment fault, also on a PROT_NONE page (alignment is checked
// first). So translation and permission faults both mean "the guest touched memory it
// may not"; which page state caused it must come from the embedder's own records, not
// from the fault kind. Bit 6 (WnR) says whether the access was a write, but an atomic
// read-modify-write can report a read.
//
// fexmac_handle_fault runs a fixed sequence of checks. Each claims a fault only when it
// is certain the fault is the core's; anything left over is the embedder's. Work that
// later packages add goes into this sequence, at the point marked below:
//  - Package D: the interrupt fault page (a write to Thread->InterruptFaultPage after it
//    is made read-only, to stop a thread from another thread); write faults on pages
//    protected for self-modifying-code detection; guest memory faults (ClassifyFault
//    returns Access with the PC in translated code), which need the guest state
//    reconstructed from the host context before they are reported as a stop.
//  - Package F: nothing expected; cached code runs from the same code buffers.
#include "Internal.h"
#ifdef FEXMAC_TESTING
#include "fexmac_testing.h"
#endif

#include <FEXCore/Core/Context.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/LongJump.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

namespace fexmac {
namespace {
  // Per host thread, kept in pthread keys rather than C++ thread_locals: on macOS the
  // first access to a thread_local on a thread may allocate memory, which must not
  // happen in a signal handler that may have interrupted malloc.
  //  - HandleDepthKey: nesting of fexmac_handle_fault (diverts logging while non-zero).
  //  - HandlerStateKey: where the library's own signal handler is (see HandlerState).
  pthread_key_t HandleDepthKey;
  pthread_key_t HandlerStateKey;
  pthread_key_t HandlerFrameKey;
  pthread_key_t HandlerCanaryKey;
  pthread_key_t HandlerMarkerKey;
  pthread_key_t EmulatingKey;
  pthread_once_t KeysOnce = PTHREAD_ONCE_INIT;
  void CreateKeys() {
    pthread_key_create(&HandleDepthKey, nullptr);
    pthread_key_create(&HandlerStateKey, nullptr);
    pthread_key_create(&HandlerFrameKey, nullptr);
    pthread_key_create(&HandlerCanaryKey, nullptr);
    pthread_key_create(&HandlerMarkerKey, nullptr);
    pthread_key_create(&EmulatingKey, nullptr);
  }

  enum HandlerState : uintptr_t {
    NotInHandler = 0,
    InLibraryHandler = 1,
    InPreviousHandler = 2,
  };

  uintptr_t GetKey(pthread_key_t Key) {
    return reinterpret_cast<uintptr_t>(pthread_getspecific(Key));
  }
  void SetKey(pthread_key_t Key, uintptr_t Value) {
    pthread_setspecific(Key, reinterpret_cast<void*>(Value));
  }

  std::atomic<bool> Installed {};
  // Written only by fexmac_install_fault_handlers (before the handlers can run) and for
  // SA_RESETHAND, as the kernel itself would reset the action.
  struct sigaction PreviousBusAction {};
  struct sigaction PreviousSegvAction {};

  // The synchronous fault signals. They stay unblocked inside the library's handler and
  // while waiting for the invalidation lock; every other signal is blocked in the
  // handler, so the only thing that can interrupt it on its thread is a fault it causes.
  constexpr int SynchronousSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGSYS};

  void WriteHex(uint64_t Value) {
    char Buffer[19] = "0x";
    int Digits = 1;
    for (uint64_t V = Value >> 4; V; V >>= 4) {
      ++Digits;
    }
    for (int i = 0; i < Digits; ++i) {
      const unsigned Nibble = (Value >> (4 * (Digits - 1 - i))) & 0xF;
      Buffer[2 + i] = static_cast<char>(Nibble < 10 ? '0' + Nibble : 'a' + Nibble - 10);
    }
    Buffer[2 + Digits] = '\0';
    WriteRaw(Buffer);
  }

  // ---- Classification ----

  enum class FaultKind { NotDataAbort, Alignment, Access, Other };

  struct Classification {
    FaultKind Kind;
    bool Write;
    uint32_t StatusCode;
  };

  Classification ClassifyFault(const ucontext_t* Context) {
    const uint32_t ESR = Context->uc_mcontext->__es.__esr;
    const uint32_t ExceptionClass = ESR >> 26;
    // 0x24: data abort from a lower exception level (user space); 0x25: same level.
    constexpr uint32_t DataAbortLower = 0x24;
    constexpr uint32_t DataAbortSame = 0x25;
    if (ExceptionClass != DataAbortLower && ExceptionClass != DataAbortSame) {
      return {FaultKind::NotDataAbort, false, 0};
    }
    const uint32_t DFSC = ESR & 0x3F;
    const bool Write = (ESR >> 6) & 1;
    if (DFSC == 0x21) {
      return {FaultKind::Alignment, Write, DFSC};
    }
    if (DFSC >= 0x04 && DFSC <= 0x0F) {
      return {FaultKind::Access, Write, DFSC};
    }
    return {FaultKind::Other, Write, DFSC};
  }

  // ---- The core's faults ----

  // Whether PC is in translated code or the dispatcher of this thread's context.
  bool IsTranslatedOrDispatcher(fexmac_thread* Current, uint64_t PC) {
    const auto& Config = Current->Context->Delegator->GetConfig();
    return Current->Thread->CTX->IsAddressInCodeBuffer(Current->Thread, PC) || (PC >= Config.DispatcherBegin && PC < Config.DispatcherEnd);
  }

  // The translator writes each block into a per-thread scratch buffer followed by an
  // inaccessible page. Running into it means the buffer was too small: resume the thread
  // at the translator's restart point, which retries with a larger buffer. On macOS the
  // inaccessible page is a whole host page placed at the next host page boundary after
  // JITGuardPage, so the fault can land anywhere from JITGuardPage to the end of it.
  //
  // Only the translator itself may claim this: guest addresses are host addresses, so a
  // guest load from that page, or a bug in the embedder's system call callback reading
  // it, lands in the same range and must not be "restarted" (that would jump to a stale
  // restart point). The core does not mark when it is translating, so the translator is
  // recognised by what only it does there: a write (the emitter appending code), from
  // host code (not translated code or the dispatcher), while no system call callback is
  // running on the thread. The restart point is disarmed after use; the core arms it
  // again when it retries the block.
  bool HandleScratchBufferOverflow(fexmac_thread* Current, const Classification& Kind, siginfo_t* Info, ucontext_t* Context) {
    auto* Thread = Current->Thread;
    if (!Thread->JITGuardPage || Kind.Kind != FaultKind::Access || !Kind.Write || Current->InCallback) {
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
    if (IsTranslatedOrDispatcher(Current, State.__pc)) {
      return false;
    }
    Thread->JITGuardPage = 0;
    FEXCore::UncheckedLongJump::ManuallyLoadJumpBuf(Thread->RestartJump, Thread->JITGuardOverflowArgument, &State.__x[0],
                                                    &Context->uc_mcontext->__ns.__v[0], &State.__pc);
    return true;
  }

  // The call/return stack overflowed (many calls without returns) or underflowed (more
  // returns than calls) into one of its guard pages. The stack only predicts returns,
  // so losing its contents is harmless: reset its pointer to the starting position and
  // retry the instruction, as FEX's Linux front end does. The pointer lives in x25
  // (REG_CALLRET_SP in FEXCore's Arm64Emitter.h) while translated code runs.
  //
  // Claimed only for the call/return stack's own push or pop: the faulting instruction
  // is in translated code or the dispatcher, is a load/store pair whose base register is
  // x25, and the fault address is within the 16 bytes it moves. A guest access to the
  // guard page (a wild pointer, a memory scan) or a host bug reading it is not this, and
  // resetting x25 then would corrupt a host register and retry the access forever.
  bool HandleCallRetStackGuard(fexmac_thread* Current, const Classification& Kind, siginfo_t* Info, ucontext_t* Context) {
    if (Kind.Kind != FaultKind::Access || !Current->CallRetAllocation) {
      return false;
    }
    const auto Address = reinterpret_cast<uintptr_t>(Info->si_addr);
    const auto Begin = reinterpret_cast<uintptr_t>(Current->CallRetAllocation);
    if (Address < Begin || Address >= Begin + Current->CallRetAllocationSize) {
      return false;
    }
    auto& State = Context->uc_mcontext->__ss;
    if (!IsTranslatedOrDispatcher(Current, State.__pc)) {
      return false;
    }
    constexpr unsigned CallRetStackRegister = 25;
    // Load/store pair, any indexing mode: bits 29:27 are 101 (bit 26 selects SIMD).
    const uint32_t Instruction = *reinterpret_cast<const uint32_t*>(State.__pc);
    const bool IsPair = (Instruction & 0x3A000000) == 0x28000000;
    const unsigned BaseRegister = (Instruction >> 5) & 0x1F;
    const uint64_t Pointer = State.__x[CallRetStackRegister];
    if (!IsPair || BaseRegister != CallRetStackRegister || Address + 16 < Pointer || Address >= Pointer + 16) {
      return false;
    }
    State.__x[CallRetStackRegister] = Current->CallRetDefault;
    return true;
  }

  // The address a misaligned atomic is being rewritten for, per thread, so that a fault
  // on it inside the rewrite (the target page is also protected) is reported as the
  // guest memory fault it is rather than as a bug in the handler (EmulatingKey).

  // ARM64 atomic instructions raise an alignment fault on an address that is not a
  // multiple of the operand size where the access crosses 16 bytes; x86 allows it. The
  // core rewrites the instruction sequence and execution resumes after it.
  bool HandleMisalignedAtomic(FEXCore::Core::InternalThreadState* Thread, const Classification& Kind, siginfo_t* Info, ucontext_t* Context) {
    auto& State = Context->uc_mcontext->__ss;
    const uint64_t PC = State.__pc;
    if (Kind.Kind != FaultKind::Alignment || !Thread->CTX->IsAddressInCodeBuffer(Thread, PC)) {
      return false;
    }
    SetKey(EmulatingKey, reinterpret_cast<uintptr_t>(Info->si_addr));
    const auto Result = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
      Thread, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, PC, &State.__x[0]);
    SetKey(EmulatingKey, 0);
    if (!Result.has_value()) {
      return false;
    }
    State.__pc = PC + *Result;
    return true;
  }

  // ---- The library's own signal handler ----

  // Whether the signal was sent (kill, pthread_kill, raise) rather than raised by a fault.
  // macOS reports a sent SIGSEGV with si_code 2 and a sent SIGBUS with si_code 1, the same
  // codes as real faults, so si_code cannot tell (measured on the M4). The syndrome can:
  // a real fault was a synchronous abort, while a signal sent by this thread arrives
  // after its kill system call (exception class 0x15, SVC). A signal sent by another
  // thread or process interrupts this one at any point and the syndrome is whatever this
  // thread's last exception left, so the fault address is compared as well: a sent signal
  // has si_addr 0 while a stale abort's address is usually not 0. The one case this
  // misreads is a signal sent while the last exception was a fault at address 0; it is
  // then treated as a fault.
  bool IsSentSignal(const siginfo_t* Info, const ucontext_t* Context) {
    if (Info->si_code == SI_USER || Info->si_code == SI_QUEUE || Info->si_code == SI_TIMER || Info->si_code == SI_ASYNCIO ||
        Info->si_code == SI_MESGQ) {
      return true;
    }
    const uint32_t ExceptionClass = Context->uc_mcontext->__es.__esr >> 26;
    // Instruction abort (0x20, 0x21), PC alignment (0x22), data abort (0x24, 0x25),
    // SP alignment (0x26).
    const bool Abort = ExceptionClass == 0x20 || ExceptionClass == 0x21 || ExceptionClass == 0x22 || ExceptionClass == 0x24 ||
                       ExceptionClass == 0x25 || ExceptionClass == 0x26;
    if (!Abort) {
      return true;
    }
    return reinterpret_cast<uint64_t>(Info->si_addr) != Context->uc_mcontext->__es.__far;
  }

  void SetDefaultAction(int Signal) {
    struct sigaction Default {};
    Default.sa_handler = SIG_DFL;
    sigemptyset(&Default.sa_mask);
    sigaction(Signal, &Default, nullptr);
  }

  // Says why a fault in translated code is passed on, so the crash that follows names it.
  void ReportUnhandledGuestFault(int Signal, siginfo_t* Info, ucontext_t* Context) {
    auto* Current = GetCurrentThread();
    if (!Current || IsSentSignal(Info, Context)) {
      return;
    }
    const auto Kind = ClassifyFault(Context);
    const uint64_t PC = Context->uc_mcontext->__ss.__pc;
    if (Kind.Kind != FaultKind::Access || !Current->Thread->CTX->IsAddressInCodeBuffer(Current->Thread, PC)) {
      return;
    }
    WriteRaw(Signal == SIGBUS ? "fexmac: SIGBUS" : "fexmac: SIGSEGV");
    WriteRaw(": guest memory access fault in translated code at address ");
    WriteHex(reinterpret_cast<uint64_t>(Info->si_addr));
    WriteRaw(Kind.Write ? " (write" : " (read");
    WriteRaw(", fault status ");
    WriteHex(Kind.StatusCode);
    WriteRaw("); guest fault delivery is not implemented, passing it on\n");
  }

  // Calls the action that was installed before the library's, as the kernel would have:
  // with its mask added, the signal itself blocked unless it had SA_NODEFER, and reset to
  // the default first if it had SA_RESETHAND.
  void CallPrevious(int Signal, siginfo_t* Info, void* RawContext, struct sigaction* Stored) {
    const struct sigaction Previous = *Stored;
    if (Previous.sa_flags & SA_RESETHAND) {
      memset(Stored, 0, sizeof(*Stored));
      Stored->sa_handler = SIG_DFL;
    }
    sigset_t Mask = Previous.sa_mask;
    if (!(Previous.sa_flags & SA_NODEFER)) {
      sigaddset(&Mask, Signal);
    }
    sigset_t Old;
    pthread_sigmask(SIG_BLOCK, &Mask, &Old);
    // If the previous handler leaves with siglongjmp, the record it leaves behind is
    // recognised as dead (see RecordedHandlerIsLive) and the mask is whatever the jump
    // restores, as without the library.
    SetKey(HandlerStateKey, InPreviousHandler);
    if (Previous.sa_flags & SA_SIGINFO) {
      Previous.sa_sigaction(Signal, Info, RawContext);
    } else {
      Previous.sa_handler(Signal);
    }
    pthread_sigmask(SIG_SETMASK, &Old, nullptr);
  }

#ifdef FEXMAC_TESTING
  std::atomic<volatile uint32_t*> TestFaultPage {};
  std::atomic<volatile uint32_t*> TestFaultFired {};
  std::atomic<bool> TestLogFromHandler {};
#endif

  // Nesting. On entry the handler records its own frame (its frame pointer) and a fresh
  // marker value in a local. A later entry on the same thread is a nested fault only if
  // that handler is still running: walking the frame-pointer chain of the interrupted
  // code (Apple's arm64 ABI keeps frame records) reaches the recorded frame, and the
  // marker there is intact. A handler that was left without returning (a previous handler
  // that siglongjmp'd out, the usual memory-probe idiom) leaves its record behind, but its
  // frame is no longer on anyone's chain, so the record is ignored and replaced. A flag
  // that only a return clears would instead treat every later fault as nested, and stack
  // depth alone cannot tell: code that runs deeper than the dead frame faults below it.
  std::atomic<uint64_t> MarkerCounter {0x6665786d6163ULL};

  bool RecordedHandlerIsLive(uintptr_t StartFrame, uintptr_t Here) {
    if (GetKey(HandlerStateKey) == NotInHandler) {
      return false;
    }
    const uintptr_t Frame = GetKey(HandlerFrameKey);
    constexpr uintptr_t MaximumDepth = 8 * 1024 * 1024;
    uintptr_t FP = StartFrame;
    for (int i = 0; i < 1024; ++i) {
      if (FP == Frame) {
        return *reinterpret_cast<volatile uint64_t*>(GetKey(HandlerMarkerKey)) == GetKey(HandlerCanaryKey);
      }
      // Only frames above this one on the same stack, each above the last.
      if (FP <= Here || FP - Here > MaximumDepth || (FP & 7) != 0 || FP > Frame) {
        return false;
      }
      const uintptr_t Next = *reinterpret_cast<const uintptr_t*>(FP);
      if (Next <= FP) {
        return false;
      }
      FP = Next;
    }
    return false;
  }

  void LeaveHandler(volatile uint64_t* Marker) {
    *Marker = 0;
    SetKey(HandlerStateKey, NotInHandler);
  }

  void LibraryHandler(int Signal, siginfo_t* Info, void* RawContext) {
    pthread_once(&KeysOnce, CreateKeys);
    volatile uint64_t Marker = 0;
    const auto Here = reinterpret_cast<uintptr_t>(&Marker);
    const auto* Interrupted = static_cast<const ucontext_t*>(RawContext);
    if (RecordedHandlerIsLive(Interrupted->uc_mcontext->__ss.__fp, Here)) {
      // A fault inside this handler, or inside the previous handler it called. Every
      // asynchronous signal is blocked in the handler, so nothing else can have
      // interrupted it. The handler is installed with SA_NODEFER, so the nested fault
      // reaches here instead of hanging the thread with the signal blocked. Put the
      // default action back and return: the instruction faults again and the process dies
      // with the usual signal, where a crash report shows it.
      const auto State = GetKey(HandlerStateKey);
      const auto Emulating = GetKey(EmulatingKey);
      const auto Address = reinterpret_cast<uintptr_t>(Info->si_addr);
      if (State == InLibraryHandler && Emulating && Address + 16 >= Emulating && Address < Emulating + 32) {
        // The core was rewriting a misaligned atomic whose memory is not accessible:
        // the guest's own fault, not a bug in the handler.
        WriteRaw(Signal == SIGBUS ? "fexmac: SIGBUS" : "fexmac: SIGSEGV");
        WriteRaw(": guest memory access fault in translated code at address ");
        WriteHex(Address);
        WriteRaw(" (misaligned atomic on an inaccessible page); guest fault delivery is not implemented, passing it on\n");
      } else {
        WriteRaw(State == InLibraryHandler ? "fexmac: fault inside the fault handler; restoring the default action\n" :
                                             "fexmac: fault inside the previously installed handler the library chained to; restoring "
                                             "the default action\n");
      }
      SetDefaultAction(Signal);
      return;
    }
    // Any record left here belongs to a handler that is gone; this entry replaces it.
    Marker = MarkerCounter.fetch_add(1) ^ Here;
    SetKey(HandlerFrameKey, reinterpret_cast<uintptr_t>(__builtin_frame_address(0)));
    SetKey(HandlerMarkerKey, Here);
    SetKey(HandlerCanaryKey, Marker);
    SetKey(HandleDepthKey, 0);
    SetKey(EmulatingKey, 0);
    SetKey(HandlerStateKey, InLibraryHandler);

#ifdef FEXMAC_TESTING
    if (auto* Fired = TestFaultFired.load()) {
      *Fired += 1;
    }
    if (auto* Page = TestFaultPage.load()) {
      *Page = 1;
    }
    if (TestLogFromHandler.load()) {
      SetKey(HandleDepthKey, GetKey(HandleDepthKey) + 1);
      LogMan::Msg::IFmt("test message from the fault handler");
      SetKey(HandleDepthKey, GetKey(HandleDepthKey) - 1);
    }
#endif

    const bool Sent = IsSentSignal(Info, static_cast<ucontext_t*>(RawContext));
    const bool Handled = !Sent && fexmac_handle_fault(Signal, Info, RawContext) != 0;
    if (Handled) {
      LeaveHandler(&Marker);
      return;
    }

    // Not the library's: pass it to whatever was installed before.
    struct sigaction* Stored = Signal == SIGBUS ? &PreviousBusAction : &PreviousSegvAction;
    const bool HasHandler = (Stored->sa_flags & SA_SIGINFO) ? Stored->sa_sigaction != nullptr :
                                                              (Stored->sa_handler != SIG_DFL && Stored->sa_handler != SIG_IGN);
    if (HasHandler) {
      CallPrevious(Signal, Info, RawContext, Stored);
      LeaveHandler(&Marker);
      return;
    }

    const bool Ignored = !(Stored->sa_flags & SA_SIGINFO) && Stored->sa_handler == SIG_IGN;
    LeaveHandler(&Marker);
    if (Sent) {
      // Sent with kill() or similar. Ignored before the library: ignore it now. Default
      // before: re-send it with the default action, which ends the process as it would
      // have; the library stays installed for as long as the process lives.
      if (!Ignored) {
        SetDefaultAction(Signal);
        pthread_kill(pthread_self(), Signal);
      }
      return;
    }
    // A real fault cannot be ignored: let it happen again with the default action, so the
    // process dies with the usual signal.
    ReportUnhandledGuestFault(Signal, Info, static_cast<ucontext_t*>(RawContext));
    SetDefaultAction(Signal);
  }
} // namespace

bool InFaultHandling() {
  pthread_once(&KeysOnce, CreateKeys);
  volatile uint64_t Probe = 0;
  return GetKey(HandleDepthKey) != 0 ||
         RecordedHandlerIsLive(reinterpret_cast<uintptr_t>(__builtin_frame_address(0)), reinterpret_cast<uintptr_t>(&Probe) - 1);
}
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
  // A signal sent to the thread is never one of the core's faults, whatever the stale
  // syndrome in its context says.
  if (fexmac::IsSentSignal(info, Context)) {
    return 0;
  }

  // macOS keeps x0 to x28 in an array followed directly by the frame pointer (x29),
  // link register (x30) and stack pointer, so the array serves as x0 to x31 for the
  // core's helpers.
  static_assert(offsetof(__darwin_arm_thread_state64, __fp) == 29 * sizeof(uint64_t));
  static_assert(offsetof(__darwin_arm_thread_state64, __lr) == 30 * sizeof(uint64_t));
  static_assert(offsetof(__darwin_arm_thread_state64, __sp) == 31 * sizeof(uint64_t));

  pthread_once(&fexmac::KeysOnce, fexmac::CreateKeys);
  fexmac::SetKey(fexmac::HandleDepthKey, fexmac::GetKey(fexmac::HandleDepthKey) + 1);

  const auto Kind = fexmac::ClassifyFault(Context);
  int Handled = 0;
  if (fexmac::HandleScratchBufferOverflow(Current, Kind, info, Context) || fexmac::HandleCallRetStackGuard(Current, Kind, info, Context) ||
      fexmac::HandleMisalignedAtomic(Thread, Kind, info, Context)) {
    Handled = 1;
  }
  // Package D adds its checks here (see the top of this file).

  fexmac::SetKey(fexmac::HandleDepthKey, fexmac::GetKey(fexmac::HandleDepthKey) - 1);
  return Handled;
}

fexmac_result fexmac_install_fault_handlers(void) {
  if (fexmac::Installed.exchange(true)) {
    return FEXMAC_OK;
  }
  // Create the per-thread keys now rather than inside the first signal.
  (void)fexmac::GetCurrentThread();
  pthread_once(&fexmac::KeysOnce, fexmac::CreateKeys);

  struct sigaction Action {};
  Action.sa_sigaction = fexmac::LibraryHandler;
  // SA_NODEFER: see LibraryHandler. SA_ONSTACK: use the host's alternate signal stack if
  // it has one, so a fault on an exhausted stack can still be reported.
  Action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
  sigfillset(&Action.sa_mask);
  for (int Signal : fexmac::SynchronousSignals) {
    sigdelset(&Action.sa_mask, Signal);
  }
  if (sigaction(SIGBUS, &Action, &fexmac::PreviousBusAction) != 0 || sigaction(SIGSEGV, &Action, &fexmac::PreviousSegvAction) != 0) {
    fexmac::Installed.store(false);
    return FEXMAC_ERROR_UNSUPPORTED;
  }
  return FEXMAC_OK;
}

#ifdef FEXMAC_TESTING
void fexmac_testing_fault_inside_handler(int enable, volatile uint32_t* fired) {
  fexmac::TestFaultFired.store(enable ? fired : nullptr);
  if (!enable) {
    fexmac::TestFaultPage.store(nullptr);
    return;
  }
  void* Page = ::mmap(nullptr, FEXCore::Utils::FEX_HOST_PAGE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Page != MAP_FAILED) {
    fexmac::TestFaultPage.store(static_cast<volatile uint32_t*>(Page));
  }
}

void fexmac_testing_log_from_fault_handler(int enable) {
  fexmac::TestLogFromHandler.store(enable != 0);
}

void fexmac_testing_log(const char* message) {
  LogMan::Msg::IFmt("{}", message);
}
#endif

} // extern "C"
