// SPDX-License-Identifier: MIT
//
// fexmac: the guest memory map the embedder describes, instruction-fetch ranges for the
// translator, and self-modifying-code detection by write-protecting translated pages.
// See "Guest memory" in fexmac.h and docs/macOS-embedding-interface.md.
//
// How a guest write to translated code is handled (SMCCHECKS mtrack):
//  1. The core translates a block and reports each 4K guest page it read code from
//     (MarkGuestExecutableRange, under the core's invalidation lock, shared). If the guest
//     page is writable, the library removes host write access from its 16K host page.
//  2. A guest store to that host page faults. The fault handler (Faults.cpp) writes the
//     guest state back from the host context, with rip at the storing instruction, and
//     returns from the signal into a trampoline instead of the faulting code.
//  3. The trampoline runs in ordinary context, on the dispatcher's stack, and calls
//     ResolveCodeWrite: it takes the context's thread lock and the core's invalidation
//     lock exclusively, gives the host page its write access back and discards the
//     translations of the whole host page for every thread. No lock is taken in the
//     signal handler, so the interrupted code cannot be holding one it needs: it was
//     translated guest code, which holds none.
//  4. The trampoline re-enters the dispatcher at its fill-state entry with the
//     single-instruction flag set, so the storing instruction is translated and run on
//     its own: a block that writes into itself never runs the stale rest of itself, and
//     because single-instruction translations do not report code pages, the page is not
//     protected again before the store is done (otherwise the store would fault forever).
//     The next block translated from the page protects it again.
#include "Internal.h"

#include <FEXCore/Utils/SignalScopeGuards.h>

#include <algorithm>
#include <optional>
#include <cstring>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

namespace fexmac {

std::atomic<fexmac_context*> ProcessContext {};

bool GuestMemory::Initialize() {
  void* Table = ::mmap(nullptr, ChunkCount * sizeof(std::atomic<uint8_t*>), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Table == MAP_FAILED) {
    return false;
  }
  Chunks = static_cast<std::atomic<uint8_t*>*>(Table);
  return true;
}

GuestMemory::~GuestMemory() {
  if (!Chunks) {
    return;
  }
  for (size_t i = 0; i < ChunkCount; ++i) {
    if (auto* Chunk = Chunks[i].load(std::memory_order_relaxed)) {
      ::munmap(Chunk, ChunkBytes);
    }
  }
  ::munmap(Chunks, ChunkCount * sizeof(std::atomic<uint8_t*>));
}

uint8_t* GuestMemory::Chunk(uint64_t Address, bool Create) {
  if (!Chunks || Address >= AddressLimit) {
    return nullptr;
  }
  auto& Slot = Chunks[Address >> ChunkShift];
  uint8_t* Chunk = Slot.load(std::memory_order_acquire);
  if (!Chunk && Create) {
    // Only ever created with Mutex held, so no two threads race to create one.
    void* Memory = ::mmap(nullptr, ChunkBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (Memory == MAP_FAILED) {
      return nullptr;
    }
    Chunk = static_cast<uint8_t*>(Memory);
    Slot.store(Chunk, std::memory_order_release);
  }
  return Chunk;
}

uint8_t GuestMemory::Page(uint64_t Address) const {
  if (!Chunks || Address >= AddressLimit) {
    return 0;
  }
  const uint8_t* Chunk = Chunks[Address >> ChunkShift].load(std::memory_order_acquire);
  if (!Chunk) {
    return 0;
  }
  const size_t Index = (Address & ((1ULL << ChunkShift) - 1)) / GuestPage;
  return __atomic_load_n(&Chunk[Index], __ATOMIC_ACQUIRE);
}

bool GuestMemory::CodeProtected(uint64_t Address) const {
  if (!Chunks || Address >= AddressLimit) {
    return false;
  }
  const uint8_t* Chunk = Chunks[Address >> ChunkShift].load(std::memory_order_acquire);
  if (!Chunk) {
    return false;
  }
  const size_t Index = (Address & ((1ULL << ChunkShift) - 1)) / HostPage;
  return __atomic_load_n(&Chunk[GuestBytesPerChunk + Index], __ATOMIC_ACQUIRE) & HostCodeProtected;
}

bool GuestMemory::SetPages(uint64_t Address, uint64_t Size, uint8_t Value) {
  for (uint64_t Page = Address; Page < Address + Size; Page += GuestPage) {
    uint8_t* Chunk = this->Chunk(Page, true);
    if (!Chunk) {
      return false;
    }
    __atomic_store_n(&Chunk[(Page & ((1ULL << ChunkShift) - 1)) / GuestPage], Value, __ATOMIC_RELEASE);
  }
  return true;
}

void GuestMemory::SetCodeProtected(uint64_t HostPageAddress, bool Value) {
  uint8_t* Chunk = this->Chunk(HostPageAddress, true);
  if (!Chunk) {
    return;
  }
  uint8_t* Byte = &Chunk[GuestBytesPerChunk + (HostPageAddress & ((1ULL << ChunkShift) - 1)) / HostPage];
  if (Value) {
    __atomic_fetch_or(Byte, HostCodeProtected, __ATOMIC_SEQ_CST);
  } else {
    __atomic_fetch_and(Byte, static_cast<uint8_t>(~HostCodeProtected), __ATOMIC_SEQ_CST);
  }
}

bool GuestMemory::HasCode(uint64_t HostPageAddress) const {
  if (!Chunks || HostPageAddress >= AddressLimit) {
    return false;
  }
  const uint8_t* Chunk = Chunks[HostPageAddress >> ChunkShift].load(std::memory_order_acquire);
  return Chunk && (__atomic_load_n(&Chunk[GuestBytesPerChunk + (HostPageAddress & ((1ULL << ChunkShift) - 1)) / HostPage], __ATOMIC_ACQUIRE) &
                   HostHasCode);
}

void GuestMemory::SetHasCode(uint64_t HostPageAddress, bool Value) {
  uint8_t* Chunk = this->Chunk(HostPageAddress, true);
  if (!Chunk) {
    return;
  }
  uint8_t* Byte = &Chunk[GuestBytesPerChunk + (HostPageAddress & ((1ULL << ChunkShift) - 1)) / HostPage];
  if (Value) {
    __atomic_fetch_or(Byte, HostHasCode, __ATOMIC_SEQ_CST);
  } else {
    __atomic_fetch_and(Byte, static_cast<uint8_t>(~HostHasCode), __ATOMIC_SEQ_CST);
  }
}

int GuestMemory::HostProtection(uint64_t HostPageAddress, bool IgnoreCodeProtection) const {
  uint32_t Prot = 0;
  for (uint64_t Page = HostPageAddress; Page < HostPageAddress + HostPage; Page += GuestPage) {
    const uint8_t Info = this->Page(Page);
    if (Info & Mapped) {
      Prot |= Info & ProtMask;
    }
  }
  if (!IgnoreCodeProtection && CodeProtected(HostPageAddress)) {
    Prot &= ~FEXMAC_PROT_WRITE;
  }
  // x86 has no write-only or execute-only pages: writable or executable implies readable.
  int Host = Prot ? PROT_READ : PROT_NONE;
  if (Prot & FEXMAC_PROT_WRITE) {
    Host |= PROT_WRITE;
  }
  // Guest memory is never executed by the host; the translator only reads it.
  return Host;
}

int GuestMemory::ReduceProtection(uint64_t Address, uint64_t Size, int Prot) {
  ProtectionChanges.fetch_add(1, std::memory_order_seq_cst);
  while (Emulations.load(std::memory_order_seq_cst) != 0) {
    sched_yield();
  }
  const int Result = ::mprotect(reinterpret_cast<void*>(Address), Size, Prot);
  ProtectionChanges.fetch_sub(1, std::memory_order_seq_cst);
  return Result;
}

namespace {
  FEXCore::Core::InternalThreadState* CallingGuestThread(fexmac_context* Context) {
    auto* Current = GetCurrentThread();
    return Current && Current->Context == Context ? Current->Thread : nullptr;
  }

  constexpr uint64_t HostPageMask = GuestMemory::HostPage - 1;

  // Holds the context's thread list, the core's invalidation lock (exclusively) and the
  // memory lock, in that order, around Fn; then discards translations of
  // [InvalidateStart, InvalidateEnd) if Fn asks for it, still holding the first two.
  // Asynchronous signals stay blocked while the locks below are held, also on a guest
  // thread (the self-modifying-code trampolines run on one, where the core's
  // signal-deferring guard does not mask them): an embedder's asynchronous handler that
  // suspended the thread or left with siglongjmp would otherwise leave every thread's
  // translation waiting for these locks.
  class AsyncSignalsBlocked {
  public:
    AsyncSignalsBlocked() {
      sigset_t Block;
      sigfillset(&Block);
      for (int Signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGSYS}) {
        sigdelset(&Block, Signal);
      }
      pthread_sigmask(SIG_BLOCK, &Block, &Old);
    }
    ~AsyncSignalsBlocked() {
      pthread_sigmask(SIG_SETMASK, &Old, nullptr);
    }

  private:
    sigset_t Old;
  };

  template<typename F>
  void WithCodeLocked(fexmac_context* Context, bool Resync, F&& Fn) {
    AsyncSignalsBlocked Blocked;
    std::lock_guard ThreadsLock {Context->ThreadsMutex};
    auto Lock = FEXCore::GuardSignalDeferringSectionWithFallback(Context->CTX->GetCodeInvalidationMutex(), CallingGuestThread(Context),
                                                                 InvalidationSignalMask());
    uint64_t Start = 0, End = 0;
    {
      std::lock_guard MemoryLock {Context->Memory.Mutex};
      Fn(Start, End);
    }
    // Only host pages the core translated code from need their code discarded (and the
    // threads told): a write_end, map or protect of plain data costs no invalidation.
    {
      std::lock_guard MemoryLock {Context->Memory.Mutex};
      bool AnyCode = false;
      for (uint64_t Page = Start & ~(GuestMemory::HostPage - 1); Page < End; Page += GuestMemory::HostPage) {
        if (Context->Memory.HasCode(Page)) {
          AnyCode = true;
          Context->Memory.SetHasCode(Page, false);
        }
      }
      if (Context->Memory.UntrackedCode) {
        // Code translated before memory was tracked may be anywhere: discard all of it once.
        Context->Memory.UntrackedCode = false;
        Start = 0;
        End = GuestMemory::AddressLimit;
      } else if (!AnyCode) {
        End = Start;
      }
    }
    if (End > Start) {
      InvalidateLocked(Context, Start, End - Start);
      if (Resync) {
        // Threads inside a loop of the discarded code would not leave it on their own.
        auto* Current = GetCurrentThread();
        for (auto* Thread : Context->Threads) {
          if (Thread != Current) {
            Thread->ResyncRequested.store(1);
            ProtectInterruptPage(Thread, true);
          }
        }
      }
    }
  }

  bool ValidRange(uint64_t Address, uint64_t Size) {
    return Size != 0 && (Address % GuestMemory::GuestPage) == 0 && (Size % GuestMemory::GuestPage) == 0 &&
           Address < GuestMemory::AddressLimit && Size <= GuestMemory::AddressLimit - Address;
  }
} // namespace

// Sets the guest pages of [Address, Address + Size) to Value and every host page they
// touch to its new protection, discarding those host pages' translations.
// What a guest page needs from the host's single protection for its 16K host page: no
// access (unmapped or PROT_NONE), read (R, X or RX: x86 has no execute-only pages, and
// execute permission is checked by the library per 4K page, not by the host), or read and
// write (any W). With [Address, Address + Size) set to Value, every host page the range
// touches is enforceable when its four guest pages need the same: then the host's
// protection is each one's, and the rest (mapped or not, executable or not) comes from
// the per-4K table.
static uint8_t HostNeed(uint8_t Page) {
  if (!(Page & GuestMemory::Mapped) || !(Page & GuestMemory::ProtMask)) {
    return 0;
  }
  return (Page & FEXMAC_PROT_WRITE) ? 2 : 1;
}

static bool Enforceable(const GuestMemory& Memory, uint64_t Address, uint64_t Size, uint8_t Value) {
  for (uint64_t HostPage = Address & ~HostPageMask; HostPage < Address + Size; HostPage += GuestMemory::HostPage) {
    std::optional<uint8_t> First;
    for (uint64_t Page = HostPage; Page < HostPage + GuestMemory::HostPage; Page += GuestMemory::GuestPage) {
      const uint8_t Need = HostNeed((Page >= Address && Page < Address + Size) ? Value : Memory.Page(Page));
      if (First && *First != Need) {
        return false;
      }
      First = Need;
    }
  }
  return true;
}

fexmac_result SetGuestMemory(fexmac_context* Context, uint64_t Address, uint64_t Size, uint8_t Value, bool RequireMapped, bool AllowUnenforced,
                             bool* Unenforced) {
  if (!ValidRange(Address, Size) || (Value & GuestMemory::ProtMask) != (Value & ~GuestMemory::Mapped)) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  fexmac_result Result = FEXMAC_OK;
  WithCodeLocked(Context, true, [&](uint64_t& Start, uint64_t& End) {
    auto& Memory = Context->Memory;
    if (RequireMapped) {
      for (uint64_t Page = Address; Page < Address + Size; Page += GuestMemory::GuestPage) {
        if (!(Memory.Page(Page) & GuestMemory::Mapped)) {
          Result = FEXMAC_ERROR_INVALID_ARGUMENT;
          return;
        }
      }
    }
    if (!Enforceable(Memory, Address, Size, Value)) {
      if (!AllowUnenforced) {
        // Refused, nothing changed: the embedder can choose a 16K-aligned layout, or say
        // that it accepts the host page's union protection.
        Result = FEXMAC_ERROR_UNSUPPORTED;
        return;
      }
      Memory.UnenforcedChanges.fetch_add(1, std::memory_order_relaxed);
      if (Unenforced) {
        *Unenforced = true;
      }
    }
    if (!Memory.Tracking()) {
      // Code translated before memory was tracked has no record of its pages.
      Memory.UntrackedCode = true;
    }
    if (!Memory.SetPages(Address, Size, Value)) {
      Result = FEXMAC_ERROR_OUT_OF_MEMORY;
      return;
    }
    Memory.StartTracking();
    Start = Address & ~HostPageMask;
    End = (Address + Size + HostPageMask) & ~HostPageMask;
    for (uint64_t HostPage = Start; HostPage < End; HostPage += GuestMemory::HostPage) {
      bool Mapped = false;
      for (uint64_t Page = HostPage; Page < HostPage + GuestMemory::HostPage; Page += GuestMemory::GuestPage) {
        Mapped |= (Memory.Page(Page) & GuestMemory::Mapped) != 0;
      }
      // A host page none of whose guest pages is mapped any more becomes inaccessible
      // here, through the same wait for emulations in progress as any reduction, so an
      // embedder's munmap that follows fexmac_memory_unmap cannot land between a
      // misaligned atomic's access check and its emulation.
      // Translations of the page are discarded below, so it needs no write protection.
      // The host protection changes first and the mark after: a fault handler that finds
      // the page not marked must find the host's protection already the guest's.
      if (Memory.ReduceProtection(HostPage, GuestMemory::HostPage, Mapped ? Memory.HostProtection(HostPage, true) : PROT_NONE) != 0 && Mapped) {
        Result = FEXMAC_ERROR_UNSUPPORTED;
      }
      Memory.SetCodeProtected(HostPage, false);
    }
  });
  return Result;
}

fexmac_result WriteBegin(fexmac_context* Context, uint64_t Address, uint64_t Size, bool Begin, uint32_t Flags, uint64_t* FirstUnwritable) {
  if (Size == 0 || Address >= GuestMemory::AddressLimit || Size > GuestMemory::AddressLimit - Address ||
      (Flags & ~uint32_t {FEXMAC_WRITE_FORCE}) != 0) {
    return Size == 0 && Flags == 0 ? FEXMAC_OK : FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  if (!Context->Memory.Tracking()) {
    // Nothing registered: the memory is the embedder's own.
    return FEXMAC_OK;
  }
  const bool Force = (Flags & FEXMAC_WRITE_FORCE) != 0;
  fexmac_result Result = FEXMAC_OK;
  WithCodeLocked(Context, !Begin, [&](uint64_t& Start, uint64_t& End) {
    auto& Memory = Context->Memory;
    Start = Address & ~HostPageMask;
    End = (Address + Size + HostPageMask) & ~HostPageMask;
    if (Begin && !Force) {
      // A write on the guest's behalf (a kernel call it makes) may only reach memory the
      // guest may write: refused, nothing changed, naming the first page it may not.
      for (uint64_t Page = Address & ~(GuestMemory::GuestPage - 1); Page < Address + Size; Page += GuestMemory::GuestPage) {
        const uint8_t Info = Memory.Page(Page);
        if (!(Info & GuestMemory::Mapped) || !(Info & FEXMAC_PROT_WRITE)) {
          if (FirstUnwritable) {
            *FirstUnwritable = Page < Address ? Address : Page;
          }
          Result = FEXMAC_ERROR_INVALID_ARGUMENT;
          Start = End = 0;
          return;
        }
      }
    }
    for (uint64_t HostPage = Start; HostPage < End; HostPage += GuestMemory::HostPage) {
      if (Begin) {
        ++Memory.Writers[HostPage];
        if (Force) {
          if (Memory.Forced[HostPage]++ == 0) {
            ::mprotect(reinterpret_cast<void*>(HostPage), GuestMemory::HostPage, PROT_READ | PROT_WRITE);
          }
          Memory.SetCodeProtected(HostPage, false);
        } else if (Memory.CodeProtected(HostPage)) {
          ::mprotect(reinterpret_cast<void*>(HostPage), GuestMemory::HostPage, Memory.HostProtection(HostPage, true));
          Memory.SetCodeProtected(HostPage, false);
        }
      } else {
        auto It = Memory.Writers.find(HostPage);
        if (It == Memory.Writers.end()) {
          Result = FEXMAC_ERROR_INVALID_ARGUMENT;
          continue;
        }
        if (--It->second == 0) {
          Memory.Writers.erase(It);
        }
        auto ForcedIt = Memory.Forced.find(HostPage);
        if (ForcedIt != Memory.Forced.end() && --ForcedIt->second == 0) {
          Memory.Forced.erase(ForcedIt);
          // Back to the guest's protection (translations of the page are discarded below,
          // so it needs no write protection now).
          Memory.ReduceProtection(HostPage, GuestMemory::HostPage, Memory.HostProtection(HostPage, true));
        }
      }
    }
    // Begin: translations of these pages are no longer protected. End: code translated
    // from them during the write may have read bytes the write then changed.
  });
  return Result;
}

FEXCore::HLE::ExecutableRangeInfo SyscallHandler::QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Address) {
  const auto& Memory = Context->Memory;
  if (!Memory.Tracking()) {
    // Nothing registered: any host memory the process can read is executable guest code,
    // as before. But memory it cannot read is not: the translator would read it and the
    // process would die of the host fault (a guest jump to 0 or to any unmapped address
    // did, round 2 review). The host's own map says which: the region holding Address,
    // or none. The frontend caches the answer until it leaves the range or code is
    // invalidated. Writable stays true, as before, because the code cache key uses it.
    mach_vm_address_t RegionStart = Address;
    mach_vm_size_t RegionSize = 0;
    vm_region_basic_info_data_64_t Info {};
    mach_msg_type_number_t Count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t Object = MACH_PORT_NULL;
    const kern_return_t Result = mach_vm_region(mach_task_self(), &RegionStart, &RegionSize, VM_REGION_BASIC_INFO_64,
                                                reinterpret_cast<vm_region_info_t>(&Info), &Count, &Object);
    if (Result != KERN_SUCCESS || RegionStart > Address || !(Info.protection & VM_PROT_READ)) {
      return {0, 0, false};
    }
    return {RegionStart, RegionSize, true};
  }
  const auto Executable = [&](uint64_t Page) {
    const uint8_t Info = Memory.Page(Page);
    return (Info & GuestMemory::Mapped) && (Info & FEXMAC_PROT_EXEC);
  };
  const uint64_t Page = Address & ~(GuestMemory::GuestPage - 1);
  if (!Executable(Page)) {
    return {0, 0, false};
  }
  const bool Writable = Memory.Page(Page) & FEXMAC_PROT_WRITE;
  const auto Same = [&](uint64_t Other) {
    return Executable(Other) && ((Memory.Page(Other) & FEXMAC_PROT_WRITE) != 0) == Writable;
  };
  // The frontend caches the range and asks again when it leaves it; a few pages either
  // way are enough to avoid asking for every page of a large block.
  constexpr uint64_t Reach = 64 * GuestMemory::GuestPage;
  uint64_t Begin = Page;
  while (Begin >= GuestMemory::GuestPage && Page - Begin < Reach && Same(Begin - GuestMemory::GuestPage)) {
    Begin -= GuestMemory::GuestPage;
  }
  uint64_t End = Page + GuestMemory::GuestPage;
  while (End - Page < Reach && Same(End)) {
    End += GuestMemory::GuestPage;
  }
  FEXCore::HLE::ExecutableRangeInfo Info {Begin, End - Begin, Writable};
  return Info;
}

void SyscallHandler::MarkGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Start, uint64_t Length) {
#ifdef FEXMAC_TESTING
  CallTestMarkHook(Start, Length);
#endif
  auto& Memory = Context->Memory;
  if (!Memory.Tracking() || Length == 0) {
    return;
  }
  // Called by the core while translating, holding its invalidation lock shared, so no
  // invalidation (and no ResolveCodeWrite) runs meanwhile.
  std::lock_guard MemoryLock {Memory.Mutex};
  const uint64_t First = Start & ~HostPageMask;
  const uint64_t Last = (Start + Length - 1) & ~HostPageMask;
  for (uint64_t HostPage = First; HostPage <= Last; HostPage += GuestMemory::HostPage) {
    Memory.SetHasCode(HostPage, true);
  }
  if (!Context->WriteProtectCode) {
    return;
  }
  for (uint64_t HostPage = First; HostPage <= Last; HostPage += GuestMemory::HostPage) {
    if (Memory.CodeProtected(HostPage) || Memory.Writers.contains(HostPage)) {
      continue;
    }
    bool Writable = false;
    for (uint64_t Page = std::max(HostPage, Start & ~(GuestMemory::GuestPage - 1));
         Page < HostPage + GuestMemory::HostPage && Page < Start + Length; Page += GuestMemory::GuestPage) {
      const uint8_t Info = Memory.Page(Page);
      Writable |= (Info & GuestMemory::Mapped) && (Info & FEXMAC_PROT_WRITE);
    }
    if (!Writable) {
      continue;
    }
    Memory.SetCodeProtected(HostPage, true);
    if (Memory.ReduceProtection(HostPage, GuestMemory::HostPage, Memory.HostProtection(HostPage)) != 0) {
      Memory.SetCodeProtected(HostPage, false);
    }
  }
}

// The store has not run yet when this is called, and other threads may translate the
// page's old bytes meanwhile. So the page is treated as written by the embedder
// (Writers, as fexmac_memory_write_begin): it is not write-protected again, and the store
// cannot fault again however often other threads translate from it. Threads in a loop of
// the discarded code are told to look it up again now (they may never leave the loop of
// their own) and once more by FinishCodeWrite, after the store, which also discards what
// they translated meanwhile from the old bytes.
void ResolveCodeWrite(fexmac_thread* Thread, uint64_t Address) {
  FinishCodeWrite(Thread);
  auto* Context = Thread->Context;
  const uint64_t HostPage = Address & ~HostPageMask;
  WithCodeLocked(Context, true, [&](uint64_t& Start, uint64_t& End) {
    auto& Memory = Context->Memory;
    ++Memory.Writers[HostPage];
    // Write access first, then the mark: a fault handler that finds the page not marked
    // (HandleMisalignedAtomic, HandleCodeWrite) must find it writable.
    ::mprotect(reinterpret_cast<void*>(HostPage), GuestMemory::HostPage, Memory.HostProtection(HostPage, true));
    Memory.SetCodeProtected(HostPage, false);
    Memory.CodeWriteFaults.fetch_add(1, std::memory_order_relaxed);
    Start = HostPage;
    End = HostPage + GuestMemory::HostPage;
  });
  Thread->PendingCodeWritePage = HostPage;
  Thread->PendingCodeWriteChecks = 2;
  ProtectInterruptPage(Thread, true);
}

void FinishCodeWrite(fexmac_thread* Thread) {
  const uint64_t HostPage = Thread->PendingCodeWritePage;
  if (!HostPage) {
    return;
  }
  Thread->PendingCodeWritePage = 0;
  Thread->PendingCodeWriteChecks = 0;
  auto* Context = Thread->Context;
  WithCodeLocked(Context, true, [&](uint64_t& Start, uint64_t& End) {
    auto& Memory = Context->Memory;
    auto It = Memory.Writers.find(HostPage);
    if (It != Memory.Writers.end() && --It->second == 0) {
      Memory.Writers.erase(It);
    }
    Start = HostPage;
    End = HostPage + GuestMemory::HostPage;
  });
}

void DescribePageFault(fexmac_context* Context, fexmac_stop* Stop, uint64_t Address, uint32_t Access, int HostSignal, bool HostPermissionFault) {
  const bool Write = Access & FEXMAC_ACCESS_WRITE;
  const bool Execute = Access & FEXMAC_ACCESS_EXECUTE;
  bool Present {};
  int Signal = SIGSEGV;
  int Code {};
  const auto& Memory = Context->Memory;
  if (Memory.Tracking()) {
    const uint8_t Info = Memory.Page(Address);
    if (!(Info & GuestMemory::Mapped)) {
      Code = SEGV_MAPERR;
    } else {
      const uint32_t Prot = Info & GuestMemory::ProtMask;
      const uint32_t Needed = Execute ? FEXMAC_PROT_EXEC : Write ? FEXMAC_PROT_WRITE : FEXMAC_PROT_READ;
      if (Prot & Needed) {
        // The guest may make this access, but the host refused it.
        Signal = SIGBUS;
        Code = BUS_ADRERR;
        Present = true;
      } else {
        Code = SEGV_ACCERR;
        // Linux reports a PROT_NONE page as not present, with SEGV_ACCERR.
        Present = Prot != 0;
      }
    }
  } else if (HostSignal == SIGSEGV) {
    // macOS raises SIGSEGV for an unmapped address and SIGBUS for a protected page.
    Code = SEGV_MAPERR;
  } else {
    Code = SEGV_ACCERR;
    Present = HostPermissionFault;
  }
  Stop->reason = FEXMAC_STOP_GUEST_FAULT;
  Stop->signal = Signal;
  Stop->si_code = Code;
  Stop->trap_number = 14;
  Stop->error_code = (Present ? FEXMAC_PF_PRESENT : 0) | (Write ? FEXMAC_PF_WRITE : 0) | FEXMAC_PF_USER | (Execute ? FEXMAC_PF_INSTRUCTION : 0);
  Stop->fault_address = Address;
  Stop->access = Access;
}

} // namespace fexmac
