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
// is certain the fault is the core's or the guest's; anything left over is the
// embedder's:
//  1. the translator's scratch-buffer guard (restart the translation);
//  2. the call/return stack's own push or pop into its guard pages (reset x25);
//  3. a write to the thread's interrupt fault page, read-only because another thread
//     asked this one to stop (leave the dispatcher at a block entry, or step over the
//     write anywhere else);
//  4. a misaligned atomic in translated code (rewritten by the core; first checked
//     against write-protected code and inaccessible memory, which go to 5 and 6);
//  5. a guest write to a page the library write-protected for self-modifying-code
//     detection (Memory.cpp; the work is done after the signal returns);
//  6. any other memory access fault with the PC in translated code: the guest's page
//     fault, delivered as FEXMAC_STOP_GUEST_FAULT with the guest state written back.
// Package F adds nothing here; cached code runs from the same code buffers.
//
// Leaving translated code from a signal handler. At a fault in translated code the guest
// registers that the translator keeps in host registers (the static register
// allocation: GPRs, the low halves of the vector registers, NZCV, the PF/AF registers,
// the call/return stack pointer) are in the signal context. SpillGuestState writes them
// to the thread's frame, as the core's own spill would, with rip taken from the block's
// table of guest instruction starts (RestoreRIPFromHostPC). The handler then changes the
// context so that the signal returns to the dispatcher's stop handler with the stack
// pointer reset to where the dispatcher saved it on entry, which leaves the host stack
// and the call/return stack consistent (the reset DEF_OP(Break) does, and the route from
// ExitFunctionLink does not, is done by this). The advanced floating-point modes the
// translator sets in FPCR are cleared first, since host code runs next. Nothing here
// allocates, locks or logs.
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
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

namespace fexmac {
namespace {
  struct ResumePoint {
    uint64_t Frame;
    uint64_t Target;
  };
} // namespace
} // namespace fexmac

// Called by CodeWriteTrampoline in ordinary context (the signal has returned): resolve
// the write, then re-enter the dispatcher at its fill-state entry with the frame in x28
// and the single-instruction flag (x1) set, so the storing instruction runs on its own.
extern "C" __attribute__((visibility("hidden"))) fexmac::ResumePoint FexmacCodeWriteResume(fexmac_thread* Thread, uint64_t Address) {
  fexmac::ResolveCodeWrite(Thread, Address);
  return {reinterpret_cast<uint64_t>(Thread->Thread->CurrentFrame), Thread->Context->Delegator->GetConfig().AbsoluteLoopTopAddressFillSRA};
}

extern "C" __attribute__((visibility("hidden"))) fexmac::ResumePoint FexmacFinishCodeWriteResume(fexmac_thread* Thread) {
  fexmac::FinishCodeWrite(Thread);
  // Requests that arrived meanwhile keep the page read-only.
  fexmac::ProtectInterruptPage(Thread, Thread->StopRequested.load() || Thread->ResyncRequested.load());
  return {reinterpret_cast<uint64_t>(Thread->Thread->CurrentFrame), Thread->Context->Delegator->GetConfig().AbsoluteLoopTopAddressFillSRA};
}

namespace fexmac {
namespace {
  // Entered with sp at the dispatcher's saved stack location, x0 the fexmac_thread and
  // x1 the address written. STATE is x28 and ENTRY_FILL_SRA_SINGLE_INST_REG is x1 in
  // the core (Arm64Emitter.h).
  __attribute__((naked)) void CodeWriteTrampoline() {
    asm volatile("bl _FexmacCodeWriteResume\n\t"
                 "mov x28, x0\n\t"
                 "mov x2, x1\n\t"
                 "mov x1, #1\n\t"
                 "br x2\n\t");
  }

  // As CodeWriteTrampoline, after the store: x0 the fexmac_thread. Re-enters without the
  // single-instruction flag.
  __attribute__((naked)) void FinishCodeWriteTrampoline() {
    asm volatile("bl _FexmacFinishCodeWriteResume\n\t"
                 "mov x28, x0\n\t"
                 "mov x2, x1\n\t"
                 "mov x1, #0\n\t"
                 "br x2\n\t");
  }

  // Per host thread, kept in pthread keys rather than C++ thread_locals: on macOS the
  // first access to a thread_local on a thread may allocate memory, which must not
  // happen in a signal handler that may have interrupted malloc.
  //  - OwnDepthKey: how deep this thread is inside the library's own fault-handling code
  //    (the library's signal handler up to the point where it hands a fault on, and
  //    fexmac_handle_fault). That code never leaves by a jump, so the count cannot go
  //    stale. A fault while it is non-zero is a fault inside the library's handler.
  //    Non-zero also diverts logging away from the log callback.
  //  - EmulatingKey: see HandleMisalignedAtomic.
  pthread_key_t OwnDepthKey;
  pthread_key_t EmulatingKey;
  pthread_once_t KeysOnce = PTHREAD_ONCE_INIT;
  void CreateKeys() {
    pthread_key_create(&OwnDepthKey, nullptr);
    pthread_key_create(&EmulatingKey, nullptr);
  }

#ifdef FEXMAC_TESTING
  std::atomic<void*> TestProtectBeforeEmulation {};
#endif

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

  // Whether an ARM64 instruction writes memory: stores, store-pairs, store-exclusives
  // and store-releases, and the read-modify-write atomics (LSE and CAS). The syndrome's
  // WnR bit is not enough: an atomic on a PROT_NONE page reports the read.
  bool IsWritingInstruction(uint32_t Inst) {
    // Load/store exclusive and ordered, CAS and CASP: L (bit 22) clear is a store;
    // o1 (bit 21) set is CAS/CASP.
    if ((Inst & 0x3F000000) == 0x08000000) {
      return !(Inst & (1U << 22)) || (Inst & (1U << 21));
    }
    // LDAPR/STLUR (unscaled, release-consistent): opc 00 is the store.
    if ((Inst & 0x3F200C00) == 0x19000000) {
      return ((Inst >> 22) & 3) == 0;
    }
    // LSE atomic memory operations (LDADD, SWP, ...); LDAPR (o3=1, opc=100) only loads.
    if ((Inst & 0x3B200C00) == 0x38200000) {
      return ((Inst >> 10) & 0x3F) != 0x30;
    }
    // Load/store pair: L (bit 22).
    if ((Inst & 0x3A000000) == 0x28000000) {
      return !(Inst & (1U << 22));
    }
    // Load/store register (immediate, unscaled, register offset): opc bits 23:22.
    if ((Inst & 0x3A000000) == 0x38000000) {
      const uint32_t Opc = (Inst >> 22) & 3;
      const bool Vector = Inst & (1U << 26);
      return Opc == 0 || (Vector && Opc == 2);
    }
    return false;
  }

  bool IsWriteFault(const Classification& Kind, uint64_t PC) {
    return Kind.Write || IsWritingInstruction(*reinterpret_cast<const uint32_t*>(PC));
  }

  // Whether the host currently allows [Address, Address + Size) to be read, or written.
  // Asked in the fault handler before the core rewrites a misaligned atomic, because the
  // rewrite would fault inside the handler on memory the guest cannot access. User space
  // has no non-faulting probe instruction; the kernel's region query is the native way.
  bool HostAllows(uint64_t Address, uint64_t Size, bool Write) {
    uint64_t Cursor = Address;
    while (Cursor < Address + Size) {
      mach_vm_address_t RegionAddress = Cursor;
      mach_vm_size_t RegionSize = 0;
      vm_region_basic_info_data_64_t Info {};
      mach_msg_type_number_t Count = VM_REGION_BASIC_INFO_COUNT_64;
      mach_port_t Object = MACH_PORT_NULL;
      if (mach_vm_region(mach_task_self(), &RegionAddress, &RegionSize, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&Info),
                         &Count, &Object) != KERN_SUCCESS ||
          RegionAddress > Cursor) {
        return false;
      }
      if (!(Info.protection & VM_PROT_READ) || (Write && !(Info.protection & VM_PROT_WRITE))) {
        return false;
      }
      Cursor = RegionAddress + RegionSize;
    }
    return true;
  }

  // ---- Leaving translated code (see the top of this file) ----

  // The static registers holding the raw PF and AF values (REG_PF, REG_AF in the core's
  // Arm64Emitter.h).
  constexpr unsigned PFRegister = 26;
  constexpr unsigned AFRegister = 27;

  void SpillGuestState(fexmac_thread* Current, ucontext_t* Context) {
    auto* Thread = Current->Thread;
    auto* Frame = Thread->CurrentFrame;
    auto* CTX = Thread->CTX;
    const auto& Config = Current->Context->Delegator->GetConfig();
    auto& State = Context->uc_mcontext->__ss;
    auto& Vector = Context->uc_mcontext->__ns;
    // x0 to x31, see the static_asserts in fexmac_handle_fault.
    const uint64_t* X = &State.__x[0];

    Frame->State.rip = CTX->RestoreRIPFromHostPC(Thread, State.__pc);
    // Registers the system call path has already written back are not in host registers.
    const uint32_t AlreadySpilled = Frame->InSyscallInfo & 0xFFFF;
    for (size_t i = 0; i < Config.SRAGPRCount; ++i) {
      const uint8_t Register = Config.SRAGPRMapping[i];
      if (!(AlreadySpilled & (1U << Register))) {
        Frame->State.gregs[i] = X[Register];
      }
    }
    // The static vector registers hold the low 128 bits (Apple silicon has no SVE, so
    // the translator keeps the upper AVX halves in the frame already).
    for (size_t i = 0; i < Config.SRAFPRCount; ++i) {
      memcpy(&Frame->State.xmm.sse.data[i][0], &Vector.__v[Config.SRAFPRMapping[i]], sizeof(__uint128_t));
    }
    constexpr unsigned CallRetStackRegister = 25;
    Frame->State.callret_sp = X[CallRetStackRegister];
    // NZCV from PSTATE and PF/AF from their registers, written back in the frame's form.
    // An instruction that changes them (or rsp) before its last access that can fault
    // saved them first (the undo record, CPUState::UndoMask): use the saved values, the
    // state before the instruction.
    const uint32_t Undo = Frame->State.UndoMask;
    uint64_t Registers[32];
    memcpy(Registers, X, sizeof(Registers));
    uint64_t PSTATE = State.__cpsr;
    if (Undo & FEXCore::Core::CPUState::UNDO_FLAGS) {
      PSTATE = Frame->State.UndoNZCV;
      Registers[PFRegister] = Frame->State.UndoPF;
      Registers[AFRegister] = Frame->State.UndoAF;
    }
    const uint32_t EFLAGS = CTX->ReconstructCompactedEFLAGS(Thread, true, Registers, PSTATE);
    CTX->SetFlagsFromCompactedEFLAGS(Thread, EFLAGS);
    if (Undo & FEXCore::Core::CPUState::UNDO_RSP) {
      Frame->State.gregs[FEXCore::X86State::REG_RSP] = Frame->State.UndoRSP;
    }
    Frame->State.UndoMask = 0;
    Frame->InSyscallInfo = 0;
  }

  void ClearTranslatorFPCR(ucontext_t* Context) {
    // FPCR.NEP, AH and FIZ: set by the translator, wrong for host code.
    Context->uc_mcontext->__ns.__fpcr &= ~0x7U;
  }

  // Makes the signal return into the dispatcher's stop handler, which returns from
  // ExecuteThread to fexmac_thread_run. The guest state must be in the frame.
  void LeaveDispatcherFromHandler(fexmac_thread* Current, ucontext_t* Context, const fexmac_stop& Stop) {
    auto* Frame = Current->Thread->CurrentFrame;
    Current->PendingStop = Stop;
    Current->PendingStop.struct_size = sizeof(fexmac_stop);
    auto& State = Context->uc_mcontext->__ss;
    State.__sp = Frame->ReturningStackLocation;
    State.__pc = Current->Context->Delegator->GetConfig().ThreadStopHandlerAddress;
    ClearTranslatorFPCR(Context);
  }

  // Makes the signal return into CodeWriteTrampoline (below), on the dispatcher's stack.
  void ResolveCodeWriteAfterHandler(fexmac_thread* Current, ucontext_t* Context, uint64_t Address) {
    auto& State = Context->uc_mcontext->__ss;
    State.__sp = Current->Thread->CurrentFrame->ReturningStackLocation;
    State.__x[0] = reinterpret_cast<uint64_t>(Current);
    State.__x[1] = Address;
    State.__lr = 0;
    State.__pc = reinterpret_cast<uint64_t>(&CodeWriteTrampoline);
    ClearTranslatorFPCR(Context);
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
  // inaccessible page is one whole host page and JITGuardPage is exactly where it starts.
  //
  // Only a translation that is running may be restarted: guest addresses are host
  // addresses, so a guest load from that page, or a bug in the embedder's system call
  // callback touching it, lands in the same range and must not jump to a restart point.
  // On macOS the core arms JITGuardPage when it starts emitting a block and disarms it
  // when the block is emitted (Arm64JITCore::EmitJITCode), so a non-zero value means the
  // translator is writing on this thread right now; neither guest code nor a callback runs
  // on the thread then. The library disarms it too before restarting; the core arms it
  // again for the retry.
  bool HandleScratchBufferOverflow(fexmac_thread* Current, const Classification& Kind, siginfo_t* Info, ucontext_t* Context) {
    auto* Thread = Current->Thread;
    if (!Thread->JITGuardPage || Kind.Kind != FaultKind::Access) {
      return false;
    }
    const uintptr_t HostPage = FEXCore::Utils::FEX_HOST_PAGE_SIZE;
    const uintptr_t GuardBegin = Thread->JITGuardPage;
    const uintptr_t GuardEnd = GuardBegin + HostPage;
    const auto Address = reinterpret_cast<uintptr_t>(Info->si_addr);
    if (Address < GuardBegin || Address >= GuardEnd) {
      return false;
    }
    auto& State = Context->uc_mcontext->__ss;
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
  bool HandleMisalignedAtomic(fexmac_thread* Current, const Classification& Kind, siginfo_t* Info, ucontext_t* Context) {
    auto* Thread = Current->Thread;
    auto& State = Context->uc_mcontext->__ss;
    const uint64_t PC = State.__pc;
    if (Kind.Kind != FaultKind::Alignment || !Thread->CTX->IsAddressInCodeBuffer(Thread, PC)) {
      return false;
    }
    // ARM64 checks alignment before permissions, so the memory may not allow the access
    // at all. The core's rewrite (for a locked operation, an emulation under the core's
    // process-wide split-lock lock) must never fault on it: a fault there is inside this
    // handler, with the lock held and one half possibly written, and cannot be left. So
    // the access is checked first and a guest fault is reported from this, the outer,
    // context, with nothing written. The check is only valid if no protection is reduced
    // between it and the emulation: the library's own changes (write protection of
    // translated code, fexmac_memory_*) wait for emulations in progress and make new ones
    // retry (GuestMemory::Emulations); a raw mprotect or munmap of the page by the
    // embedder at that moment is outside the contract, and the nested fault then ends the
    // process (it never hangs; see fexmac_handle_fault). The access is at most 16 bytes
    // from the fault address.
    const auto Address = reinterpret_cast<uint64_t>(Info->si_addr);
    const bool Write = IsWriteFault(Kind, PC);
    auto* Embedding = Current->Context;
    auto& Memory = Embedding->Memory;
    Memory.Emulations.fetch_add(1, std::memory_order_seq_cst);
    struct EmulationGuard {
      GuestMemory& Memory;
      ~EmulationGuard() {
        Memory.Emulations.fetch_sub(1, std::memory_order_seq_cst);
      }
    } Guard {Memory};
    if (Memory.ProtectionChanges.load(std::memory_order_seq_cst) != 0) {
      // A protection change is about to be applied: run the instruction again, which
      // faults again once it is done.
      return true;
    }
    if (Write && Embedding->WriteProtectCode && Memory.Tracking()) {
      for (uint64_t Byte : {Address, Address + 15}) {
        const uint8_t Page = Memory.Page(Byte);
        if ((Page & GuestMemory::Mapped) && (Page & FEXMAC_PROT_WRITE) && Memory.CodeProtected(Byte)) {
          // Translated code on the page: resolve that first; the atomic then runs again
          // as a single instruction and is rewritten on writable memory.
          SpillGuestState(Current, Context);
          ResolveCodeWriteAfterHandler(Current, Context, Byte);
          return true;
        }
      }
    }
    // The bytes the access touches: the size is in bits 31:30 of every load/store
    // encoding; a pair (CASP, LDXP/STXP, LDP/STP) touches two.
    const uint32_t Instruction = *reinterpret_cast<const uint32_t*>(PC);
    const bool Pair = (Instruction & 0x3F200000) == 0x08200000 || (Instruction & 0x3A000000) == 0x28000000;
    const uint64_t Size = (1ULL << (Instruction >> 30)) * (Pair ? 2 : 1);
    if (!HostAllows(Address, Size, Write)) {
      for (uint64_t Byte : {Address, (Address + Size - 1) & ~(GuestMemory::GuestPage - 1)}) {
        if (Byte < Address) {
          continue;
        }
        if (!HostAllows(Byte, 1, Write)) {
          SpillGuestState(Current, Context);
          fexmac_stop Stop {};
          Stop.instruction_address = Thread->CurrentFrame->State.rip;
          // Without registered memory: present (a protection fault) if the page is readable.
          DescribePageFault(Embedding, &Stop, Byte, Write ? FEXMAC_ACCESS_WRITE : FEXMAC_ACCESS_READ, SIGBUS, Write && HostAllows(Byte, 1, false));
          LeaveDispatcherFromHandler(Current, Context, Stop);
          return true;
        }
      }
    }
    // Asynchronous signals stay blocked while the emulation holds the core's locks, also
    // when the host owns the signals and installed its handler with a wider mask (an
    // asynchronous handler that left with siglongjmp or suspended the thread would leave
    // the lock and the thread's JIT write mode held). pthread_sigmask is
    // async-signal-safe.
    sigset_t Block, Old;
    sigfillset(&Block);
    for (int Synchronous : SynchronousSignals) {
      sigdelset(&Block, Synchronous);
    }
    pthread_sigmask(SIG_BLOCK, &Block, &Old);
#ifdef FEXMAC_TESTING
    if (void* Page = TestProtectBeforeEmulation.load()) {
      ::mprotect(Page, FEXCore::Utils::FEX_HOST_PAGE_SIZE, PROT_NONE);
    }
#endif
    SetKey(EmulatingKey, reinterpret_cast<uintptr_t>(Info->si_addr));
    const auto Result = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
      Thread, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, PC, &State.__x[0]);
    SetKey(EmulatingKey, 0);
    pthread_sigmask(SIG_SETMASK, &Old, nullptr);
    if (!Result.has_value()) {
      return false;
    }
    State.__pc = PC + *Result;
    return true;
  }

  // Makes the interrupt fault page writable again, then checks for a request that arrived
  // in between (its store and mprotect may both have come before the unprotect) and makes
  // the page read-only again if so, so that no request is left with a writable page.
  void UnprotectInterruptPageUnlessRequested(fexmac_thread* Current) {
    ProtectInterruptPage(Current, false);
    if (Current->StopRequested.load() || Current->ResyncRequested.load()) {
      ProtectInterruptPage(Current, true);
    }
  }

  // Another thread asked this one to stop (fexmac_thread_request_stop) and made its
  // interrupt fault page read-only; this thread has just written to it. Translated code
  // writes it only at block entries and loop back-edges, where the guest state is at an
  // instruction boundary (or part-way through a rep loop, resumable), so the thread
  // leaves the dispatcher there. The dispatcher writes it after its own calls, and the
  // core's C++ code at the end of every signal-deferring section (a lock guard); there
  // the store is stepped over (it has no other effect: nothing reads the page) and the
  // page stays read-only, so the next block entry stops the thread.
  bool HandleInterruptPage(fexmac_thread* Current, const Classification& Kind, siginfo_t* Info, ucontext_t* Context) {
    const auto Address = reinterpret_cast<uint64_t>(Info->si_addr);
    const auto Page = reinterpret_cast<uint64_t>(InterruptPage(Current));
    if (Kind.Kind != FaultKind::Access || Address < Page || Address >= Page + FEXCore::Utils::FEX_HOST_PAGE_SIZE) {
      return false;
    }
    auto& State = Context->uc_mcontext->__ss;
    const bool StopWanted = Current->StopRequested.load() != 0;
    if (!StopWanted && Current->ResyncRequested.load() == 0 && Current->PendingCodeWriteChecks == 0) {
      // Stale: the request was answered between its store and its mprotect.
      UnprotectInterruptPageUnlessRequested(Current);
      return true;
    }
    if (!Current->Thread->CTX->IsAddressInCodeBuffer(Current->Thread, State.__pc)) {
      State.__pc += 4;
      return true;
    }
    if (Current->PendingCodeWriteChecks != 0) {
      // A store to write-protected code is being run as a single instruction (Memory.cpp).
      // The first check is that block's entry: the store is still to come. The second is
      // after it: finish the write in ordinary context, then carry on from here.
      if (--Current->PendingCodeWriteChecks != 0) {
        State.__pc += 4;
        return true;
      }
      SpillGuestState(Current, Context);
      State.__sp = Current->Thread->CurrentFrame->ReturningStackLocation;
      State.__x[0] = reinterpret_cast<uint64_t>(Current);
      State.__lr = 0;
      State.__pc = reinterpret_cast<uint64_t>(&FinishCodeWriteTrampoline);
      ClearTranslatorFPCR(Context);
      return true;
    }
    if (!StopWanted) {
      // Code was discarded by another thread: re-enter the dispatcher at this guest
      // instruction (the fill-state entry, not single-stepping), which looks it up anew.
      Current->ResyncRequested.store(0);
      UnprotectInterruptPageUnlessRequested(Current);
      SpillGuestState(Current, Context);
      State.__sp = Current->Thread->CurrentFrame->ReturningStackLocation;
      State.__x[1] = 0;
      State.__x[28] = reinterpret_cast<uint64_t>(Current->Thread->CurrentFrame);
      State.__pc = Current->Context->Delegator->GetConfig().AbsoluteLoopTopAddressFillSRA;
      return true;
    }
    Current->ResyncRequested.store(0);
    // Consume first, then make the page writable: a request made in between is not lost
    // (it leaves the flag set, so the next run answers it at once).
    Current->StopRequested.store(0);
    UnprotectInterruptPageUnlessRequested(Current);
    SpillGuestState(Current, Context);
    fexmac_stop Stop {};
    Stop.reason = FEXMAC_STOP_REQUESTED;
    Stop.instruction_address = Current->Thread->CurrentFrame->State.rip;
    LeaveDispatcherFromHandler(Current, Context, Stop);
    return true;
  }

  // A guest store to a page the library write-protected because code on it was
  // translated (Memory.cpp). Only from translated code: a host write (the embedder) is
  // not the guest's and is passed on; fexmac_memory_write_begin is the way for that.
  bool HandleCodeWrite(fexmac_thread* Current, const Classification& Kind, siginfo_t* Info, ucontext_t* Context) {
    auto* Embedding = Current->Context;
    if (Kind.Kind != FaultKind::Access || !Embedding->WriteProtectCode || !Embedding->Memory.Tracking()) {
      return false;
    }
    const auto Address = reinterpret_cast<uint64_t>(Info->si_addr);
    const uint8_t Page = Embedding->Memory.Page(Address);
    const uint64_t PC = Context->uc_mcontext->__ss.__pc;
    if (!(Page & GuestMemory::Mapped) || !(Page & FEXMAC_PROT_WRITE) || !Current->Thread->CTX->IsAddressInCodeBuffer(Current->Thread, PC) ||
        !IsWriteFault(Kind, PC)) {
      return false;
    }
    if (!Embedding->Memory.CodeProtected(Address)) {
      // Another thread gave the page its write access back after this store faulted:
      // run it again. If the host still refuses the write, it is not ours.
      return HostAllows(Address, 1, true);
    }
    SpillGuestState(Current, Context);
    ResolveCodeWriteAfterHandler(Current, Context, Address);
    return true;
  }

  // Any other memory access fault in translated code is the guest's own page fault.
  bool HandleGuestMemoryFault(fexmac_thread* Current, int Signal, const Classification& Kind, siginfo_t* Info, ucontext_t* Context) {
    const uint64_t PC = Context->uc_mcontext->__ss.__pc;
    if (Kind.Kind != FaultKind::Access || !Current->Thread->CTX->IsAddressInCodeBuffer(Current->Thread, PC)) {
      return false;
    }
    const bool Write = IsWriteFault(Kind, PC);
    SpillGuestState(Current, Context);
    fexmac_stop Stop {};
    Stop.instruction_address = Current->Thread->CurrentFrame->State.rip;
    const bool Permission = Kind.StatusCode >= 0x0C && Kind.StatusCode <= 0x0F;
    DescribePageFault(Current->Context, &Stop, reinterpret_cast<uint64_t>(Info->si_addr), Write ? FEXMAC_ACCESS_WRITE : FEXMAC_ACCESS_READ,
                      Signal, Permission);
    LeaveDispatcherFromHandler(Current, Context, Stop);
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

  // Says why a fault is passed on when the cause is a known misuse, so the crash that
  // follows names it: host code writing a page the library write-protected for code.
  void ReportUnhandledFault(int Signal, siginfo_t* Info, ucontext_t* Context) {
    if (IsSentSignal(Info, Context)) {
      return;
    }
    const auto Address = reinterpret_cast<uint64_t>(Info->si_addr);
    auto* Context_ = ProcessContext.load();
    if (!Context_ || !Context_->Memory.CodeProtected(Address)) {
      return;
    }
    WriteRaw(Signal == SIGBUS ? "fexmac: SIGBUS" : "fexmac: SIGSEGV");
    WriteRaw(": host code wrote guest address ");
    WriteHex(Address);
    WriteRaw(", which holds translated code and is write-protected for self-modifying-code detection; "
             "bracket such writes with fexmac_memory_write_begin and fexmac_memory_write_end\n");
  }

  // The core keeps an inaccessible host page after each code buffer, so an access that
  // runs past a buffer's end faults instead of landing in whatever is mapped next. Every
  // write into a code buffer is bounds-checked first, so this never happens in normal
  // operation: it is a bug in the core, never the guest's doing, and nothing can be
  // repaired. Reported whatever handles the fault next, then passed on like any other.
  void ReportCodeBufferOverrun(int Signal, siginfo_t* Info, ucontext_t* Context) {
    auto* Current = GetCurrentThread();
    if (!Current) {
      return;
    }
    const auto Kind = ClassifyFault(Context);
    const auto Address = reinterpret_cast<uintptr_t>(Info->si_addr);
    if (Kind.Kind != FaultKind::Access || !Current->Thread->CTX->IsAddressInCodeBufferGuard(Current->Thread, Address)) {
      return;
    }
    WriteRaw(Signal == SIGBUS ? "fexmac: SIGBUS" : "fexmac: SIGSEGV");
    WriteRaw(Kind.Write ? ": write" : ": read");
    WriteRaw(" past the end of a code buffer, at address ");
    WriteHex(Address);
    WriteRaw(" (its guard page) from host pc ");
    WriteHex(Context->uc_mcontext->__ss.__pc);
    WriteRaw("; a bug in the translator core, passing it on\n");
  }

  // Calls the action that was installed before the library's: with its mask added and
  // reset to the default first if it had SA_RESETHAND, as the kernel would. One
  // difference, on purpose: SIGSEGV and SIGBUS (and the other synchronous fault signals)
  // are never blocked around it, even without SA_NODEFER, because on macOS a fault while
  // its signal is blocked hangs the thread. The library does not police handlers it
  // chains to: while the previous handler runs, the library is not handling anything, so
  // a fault inside it re-enters, is not the library's, and is passed to the previous
  // handler again, as if the library were not installed. If that recurses, it ends in a
  // stack overflow and the process dies; it does not hang.
  void CallPrevious(int Signal, siginfo_t* Info, void* RawContext, struct sigaction* Stored) {
    const struct sigaction Previous = *Stored;
    if (Previous.sa_flags & SA_RESETHAND) {
      memset(Stored, 0, sizeof(*Stored));
      Stored->sa_handler = SIG_DFL;
    }
    sigset_t Mask = Previous.sa_mask;
    for (int Synchronous : SynchronousSignals) {
      sigdelset(&Mask, Synchronous);
    }
    sigset_t Old;
    pthread_sigmask(SIG_BLOCK, &Mask, &Old);
    // If the previous handler leaves with siglongjmp, the mask is whatever the jump
    // restores, as without the library; the library holds no state that the jump skips.
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

  void EnterOwnCode() {
    SetKey(OwnDepthKey, GetKey(OwnDepthKey) + 1);
  }
  void LeaveOwnCode() {
    SetKey(OwnDepthKey, GetKey(OwnDepthKey) - 1);
  }

  void LibraryHandler(int Signal, siginfo_t* Info, void* RawContext) {
    pthread_once(&KeysOnce, CreateKeys);
    if (GetKey(OwnDepthKey) != 0) {
      // A fault inside the library's own handling code. Every asynchronous signal is
      // blocked there, and the handler is installed with SA_NODEFER, so the nested fault
      // reaches here instead of hanging the thread with the signal blocked. Put the
      // default action back and return: the instruction faults again and the process dies
      // with the usual signal, where a crash report shows it.
      const auto Emulating = GetKey(EmulatingKey);
      const auto Address = reinterpret_cast<uintptr_t>(Info->si_addr);
      if (Emulating && Address + 16 >= Emulating && Address < Emulating + 32) {
        // The core was emulating a misaligned atomic and its memory became inaccessible
        // under it (see HandleMisalignedAtomic), with the split-lock lock held: end the
        // process rather than leave the emulation.
        WriteRaw(Signal == SIGBUS ? "fexmac: SIGBUS" : "fexmac: SIGSEGV");
        WriteRaw(": fault while emulating a misaligned atomic at address ");
        WriteHex(Address);
        WriteRaw(" (the page's protection changed under it); ending the process\n");
      } else {
        WriteRaw("fexmac: fault inside the fault handler; restoring the default action\n");
      }
      SetDefaultAction(Signal);
      return;
    }
    EnterOwnCode();

#ifdef FEXMAC_TESTING
    if (auto* Fired = TestFaultFired.load()) {
      *Fired += 1;
    }
    if (auto* Page = TestFaultPage.load()) {
      *Page = 1;
    }
    if (TestLogFromHandler.load()) {
      LogMan::Msg::IFmt("test message from the fault handler");
    }
#endif

    const bool Sent = IsSentSignal(Info, static_cast<ucontext_t*>(RawContext));
    const bool Handled = !Sent && fexmac_handle_fault(Signal, Info, RawContext) != 0;
    if (Handled) {
      LeaveOwnCode();
      return;
    }

    // Not the library's: pass it to whatever was installed before. The library's own
    // handling ends here, before the previous handler runs.
    struct sigaction* Stored = Signal == SIGBUS ? &PreviousBusAction : &PreviousSegvAction;
    const bool HasHandler = (Stored->sa_flags & SA_SIGINFO) ? Stored->sa_sigaction != nullptr :
                                                              (Stored->sa_handler != SIG_DFL && Stored->sa_handler != SIG_IGN);
    const bool Ignored = !(Stored->sa_flags & SA_SIGINFO) && Stored->sa_handler == SIG_IGN;
    if (!Sent) {
      ReportCodeBufferOverrun(Signal, Info, static_cast<ucontext_t*>(RawContext));
    }
    if (!HasHandler && !Sent) {
      // Written while still in the library's own code.
      ReportUnhandledFault(Signal, Info, static_cast<ucontext_t*>(RawContext));
    }
    LeaveOwnCode();
    if (HasHandler) {
      CallPrevious(Signal, Info, RawContext, Stored);
      return;
    }
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
    SetDefaultAction(Signal);
  }
} // namespace

bool InFaultHandling() {
  pthread_once(&KeysOnce, CreateKeys);
  return GetKey(OwnDepthKey) != 0;
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
  if (fexmac::GetKey(fexmac::EmulatingKey) != 0) {
    // A fault inside the core's emulation of a misaligned atomic, with its split-lock
    // lock held (only possible if the embedder changed the page's protection directly at
    // that moment; see HandleMisalignedAtomic). It must not reach the host's handler,
    // which might leave it with siglongjmp and the lock held: every later split locked
    // operation would then spin forever. End the process instead: claim it and let it
    // fault again with the default action.
    fexmac::WriteRaw("fexmac: fault while emulating a misaligned atomic (the page's protection changed under it); "
                     "ending the process\n");
    fexmac::SetDefaultAction(signal);
    return 1;
  }
  fexmac::EnterOwnCode();

  const auto Kind = fexmac::ClassifyFault(Context);
  int Handled = 0;
  if (fexmac::HandleScratchBufferOverflow(Current, Kind, info, Context) || fexmac::HandleCallRetStackGuard(Current, Kind, info, Context) ||
      fexmac::HandleInterruptPage(Current, Kind, info, Context) || fexmac::HandleMisalignedAtomic(Current, Kind, info, Context) ||
      fexmac::HandleCodeWrite(Current, Kind, info, Context) || fexmac::HandleGuestMemoryFault(Current, signal, Kind, info, Context)) {
    Handled = 1;
  }
  (void)Thread;

  fexmac::LeaveOwnCode();
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

void fexmac_testing_protect_before_emulation(void* page) {
  fexmac::TestProtectBeforeEmulation.store(page);
}

void fexmac_testing_log_from_fault_handler(int enable) {
  fexmac::TestLogFromHandler.store(enable != 0);
}

void fexmac_testing_log(const char* message) {
  LogMan::Msg::IFmt("{}", message);
}
#endif

} // extern "C"
