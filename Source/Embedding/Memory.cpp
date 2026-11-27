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
#include <cstring>
#include <signal.h>
#include <sched.h>
#include <sys/mman.h>

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
  __atomic_store_n(&Chunk[GuestBytesPerChunk + (HostPageAddress & ((1ULL << ChunkShift) - 1)) / HostPage],
                   Value ? HostCodeProtected : uint8_t {0}, __ATOMIC_RELEASE);
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
  template<typename F>
  void WithCodeLocked(fexmac_context* Context, bool Resync, F&& Fn) {
    std::lock_guard ThreadsLock {Context->ThreadsMutex};
    auto Lock = FEXCore::GuardSignalDeferringSectionWithFallback(Context->CTX->GetCodeInvalidationMutex(), CallingGuestThread(Context),
                                                                 InvalidationSignalMask());
    uint64_t Start = 0, End = 0;
    {
      std::lock_guard MemoryLock {Context->Memory.Mutex};
      Fn(Start, End);
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
fexmac_result SetGuestMemory(fexmac_context* Context, uint64_t Address, uint64_t Size, uint8_t Value, bool RequireMapped) {
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
      // A host page none of whose guest pages is mapped is left alone: the embedder
      // owns whatever is there (or nothing is).
      // Translations of the page are discarded below, so it needs no write protection.
      // The host protection changes first and the mark after: a fault handler that finds
      // the page not marked must find the host's protection already the guest's.
      if (Mapped && Memory.ReduceProtection(HostPage, GuestMemory::HostPage, Memory.HostProtection(HostPage, true)) != 0) {
        Result = FEXMAC_ERROR_UNSUPPORTED;
      }
      Memory.SetCodeProtected(HostPage, false);
    }
  });
  return Result;
}

fexmac_result WriteBegin(fexmac_context* Context, uint64_t Address, uint64_t Size, bool Begin) {
  if (Size == 0 || Address >= GuestMemory::AddressLimit || Size > GuestMemory::AddressLimit - Address) {
    return Size == 0 ? FEXMAC_OK : FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  if (!Context->WriteProtectCode || !Context->Memory.Tracking()) {
    return FEXMAC_OK;
  }
  fexmac_result Result = FEXMAC_OK;
  WithCodeLocked(Context, !Begin, [&](uint64_t& Start, uint64_t& End) {
    auto& Memory = Context->Memory;
    Start = Address & ~HostPageMask;
    End = (Address + Size + HostPageMask) & ~HostPageMask;
    for (uint64_t HostPage = Start; HostPage < End; HostPage += GuestMemory::HostPage) {
      if (Begin) {
        ++Memory.Writers[HostPage];
        if (Memory.CodeProtected(HostPage)) {
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
    return {0, UINT64_MAX, true};
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
  return {Begin, End - Begin, Writable};
}

void SyscallHandler::MarkGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Start, uint64_t Length) {
#ifdef FEXMAC_TESTING
  CallTestMarkHook(Start, Length);
#endif
  auto& Memory = Context->Memory;
  if (!Context->WriteProtectCode || !Memory.Tracking() || Length == 0) {
    return;
  }
  // Called by the core while translating, holding its invalidation lock shared, so no
  // invalidation (and no ResolveCodeWrite) runs meanwhile.
  std::lock_guard MemoryLock {Memory.Mutex};
  const uint64_t First = Start & ~HostPageMask;
  const uint64_t Last = (Start + Length - 1) & ~HostPageMask;
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
// cannot fault again however often other threads translate from it. FinishCodeWrite,
// after the store, discards what they translated meanwhile and makes threads in loops of
// that code look it up again.
void ResolveCodeWrite(fexmac_thread* Thread, uint64_t Address) {
  FinishCodeWrite(Thread);
  auto* Context = Thread->Context;
  const uint64_t HostPage = Address & ~HostPageMask;
  WithCodeLocked(Context, false, [&](uint64_t& Start, uint64_t& End) {
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
