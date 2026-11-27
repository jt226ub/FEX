// SPDX-License-Identifier: MIT
//
// Private to the fexmac library: the objects behind the opaque handles.
#pragma once

#include "fexmac.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/fextl/memory.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>

#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace fexmac {

// The core needs somewhere to publish the dispatcher's entry and exit points. The
// library uses ThreadStopHandlerAddress to leave the dispatcher (stops, guest memory
// faults, stop requests) and AbsoluteLoopTopAddressFillSRA to re-enter it after a write
// to code it had write-protected (Memory.cpp).
class SignalDelegator final : public FEXCore::SignalDelegator, public FEXCore::Allocator::FEXAllocOperators {};

class SyscallHandler final : public FEXCore::HLE::SyscallHandler, public FEXCore::Allocator::FEXAllocOperators {
public:
  explicit SyscallHandler(fexmac_context* Context)
    : Context {Context} {}

  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override;
  void InvalidateGuestCodeRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) override;

  // Until the embedder registers guest memory (fexmac_memory_map), every address is
  // executable and writable code, as before. Afterwards the registered protections are
  // reported, so the translator stops with a fetch fault at an unmapped or
  // non-executable address instead of reading it. Package F (cached-code keying) uses
  // the same answer. Memory.cpp.
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t Address) override;
  // The core reports each guest page it starts translating code from. With SMCCHECKS
  // mtrack and registered memory, the library write-protects the page. Memory.cpp.
  void MarkGuestExecutableRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) override;

  // Package F (cached code loading) answers this from the embedder's file mappings.
  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }

private:
  fexmac_context* Context;
};

// Options checked and converted to the strings the core's configuration expects.
using ParsedOptions = fextl::vector<std::pair<FEXCore::Config::ConfigOption, fextl::string>>;
fexmac_result ParseOptions(const fexmac_config& Config, ParsedOptions* Out);
fextl::unique_ptr<FEXCore::Config::Layer> CreateOptionsLayer(ParsedOptions Options);

// The guest thread currently running on this host thread, or null. Read by the fault
// handler, so it is kept in a pthread key: on macOS the first access to a C++
// thread_local on a thread may allocate, which a signal handler must not do.
fexmac_thread* GetCurrentThread();
void SetCurrentThread(fexmac_thread* Thread);

// True while this host thread is inside the library's fault handling (fexmac_handle_fault
// or the library's own signal handler). Logging is diverted to write(2) then.
bool InFaultHandling();

// Writes a message to standard error with write(2) only. Async-signal-safe.
void WriteRaw(const char* Message);
void WriteHex(uint64_t Value);

// ---- Guest memory as the embedder describes it (Memory.cpp) ----
//
// Guest pages are 4K, host pages 16K. For each 4K guest page the library keeps one byte:
// the guest's protection (FEXMAC_PROT_* bits) and whether the embedder registered it as
// mapped. For each 16K host page one byte says whether the library has removed write
// access from it because code on it was translated (self-modifying-code detection).
// Tables are two-level (1GB chunks), allocated when a chunk is first registered and kept
// until the context is destroyed, so the fault handler reads them without locks.
class GuestMemory {
public:
  static constexpr uint8_t ProtMask = 7;
  static constexpr uint8_t Mapped = 8;
  static constexpr uint8_t HostCodeProtected = 1;

  static constexpr uint64_t GuestPage = 0x1000;
  static constexpr uint64_t HostPage = 0x4000;
  static constexpr unsigned ChunkShift = 30;
  static constexpr uint64_t AddressLimit = 1ULL << 47;

  bool Initialize();
  ~GuestMemory();

  // True once the embedder has registered any guest memory. Async-signal-safe.
  bool Tracking() const {
    return TrackingFlag.load(std::memory_order_acquire);
  }
  // The byte for the guest page holding Address (0 when never registered).
  // Async-signal-safe.
  uint8_t Page(uint64_t Address) const;
  // Whether the library write-protected the host page holding Address. Async-signal-safe.
  bool CodeProtected(uint64_t Address) const;

  // Everything below runs in ordinary context with Mutex held.
  std::mutex Mutex;
  bool SetPages(uint64_t Address, uint64_t Size, uint8_t Value);
  void SetCodeProtected(uint64_t HostPageAddress, bool Value);
  // The protection the host page should have: the union of its guest pages' protections,
  // without write access when the library write-protects it for code.
  int HostProtection(uint64_t HostPageAddress, bool IgnoreCodeProtection = false) const;
  // Host pages that the embedder is writing between fexmac_memory_write_begin and _end;
  // they are not write-protected meanwhile. Count per host page.
  std::map<uint64_t, uint32_t> Writers;
  void StartTracking() {
    TrackingFlag.store(true, std::memory_order_release);
  }

  // Emulations of misaligned atomics in progress in fault handlers, and protection
  // reductions the library is about to apply. Before reducing a protection the library
  // raises ProtectionChanges and waits for Emulations to be zero; an emulation raises
  // Emulations and backs off (the instruction runs again) while ProtectionChanges is
  // raised. So the access check the handler makes before an emulation stays true until
  // the emulation ends. Sequentially consistent, lock-free.
  std::atomic<uint32_t> Emulations {};
  std::atomic<uint32_t> ProtectionChanges {};
  // mprotect that waits as described, for any change that may remove access.
  int ReduceProtection(uint64_t Address, uint64_t Size, int Prot);

  // Number of writes to write-protected code handled (statistics, relaxed).
  std::atomic<uint64_t> CodeWriteFaults {};

private:
  uint8_t* Chunk(uint64_t Address, bool Create);
  static constexpr size_t ChunkCount = AddressLimit >> ChunkShift;
  static constexpr size_t GuestBytesPerChunk = (1ULL << ChunkShift) / GuestPage;
  static constexpr size_t HostBytesPerChunk = (1ULL << ChunkShift) / HostPage;
  static constexpr size_t ChunkBytes = GuestBytesPerChunk + HostBytesPerChunk;
  std::atomic<uint8_t*>* Chunks {};
  std::atomic<bool> TrackingFlag {};
};

} // namespace fexmac

struct fexmac_context {
  fextl::unique_ptr<FEXCore::Context::Context> CTX;
  fextl::unique_ptr<fexmac::SignalDelegator> Delegator;
  fextl::unique_ptr<fexmac::SyscallHandler> Syscalls;

  fexmac_syscall_fn SyscallFn {};
  void* SyscallUserData {};

  // One descriptor table for every thread, used as both GDT and LDT. Every selector a
  // guest can load reads a present-but-null descriptor instead of memory past the end.
  static constexpr size_t DescriptorCount = 8192;
  FEXCore::Core::CPUState::gdt_segment* Descriptors {};

  // Calls in progress that use the context without holding the registry lock (thread
  // creation, CPUID, invalidation). Guarded by the registry lock; the context cannot be
  // destroyed while it is non-zero.
  uint32_t ActiveCalls {};

  // Live threads, in the order the core needs for invalidation. Lock order: the registry
  // lock may be held when taking this one, never the other way round. Then the core's
  // invalidation lock, then GuestMemory::Mutex.
  std::mutex ThreadsMutex;
  fextl::vector<fexmac_thread*> Threads;

  fexmac::GuestMemory Memory;
  // SMCCHECKS is mtrack: translated pages are write-protected once memory is registered.
  bool WriteProtectCode {};
};

struct fexmac_thread {
  fexmac_context* Context {};
  FEXCore::Core::InternalThreadState* Thread {};
  void* CallRetAllocation {};
  size_t CallRetAllocationSize {};
  // Where the call/return stack pointer starts, and is reset to after an overflow or
  // underflow into a guard page.
  uint64_t CallRetDefault {};
  void* UserData {};

  // Both guarded by the registry lock. Running: a host thread is inside
  // fexmac_thread_run for this thread. InCallback: its system call callback runs.
  bool Running {};
  bool InCallback {};

  // Written just before leaving the dispatcher.
  fexmac_stop PendingStop {};

  // fexmac_thread_request_stop sets this and makes the interrupt fault page read-only;
  // it is consumed by the one FEXMAC_STOP_REQUESTED stop that answers it.
  std::atomic<uint32_t> StopRequested {};
  // Set (with the interrupt fault page made read-only) when code this thread may be
  // running was discarded by another thread: at its next block entry or loop back-edge
  // the thread re-enters the dispatcher at the same guest instruction, so a loop that
  // never leaves its block cannot keep running the stale translation.
  std::atomic<uint32_t> ResyncRequested {};

  // A guest store to write-protected code is in progress on this thread (Memory.cpp): the
  // host page it writes, kept writable and unprotected until the store has run, and the
  // interrupt checks to pass before then (the single-instruction block's entry, then the
  // next block's). Only this thread touches these.
  uint64_t PendingCodeWritePage {};
  uint32_t PendingCodeWriteChecks {};
};

namespace fexmac {
// The interrupt fault page of a thread, which translated code writes at every block
// entry and loop back-edge (see fexmac_thread_request_stop).
inline void* InterruptPage(fexmac_thread* Thread) {
  return &Thread->Thread->InterruptFaultPage;
}
void ProtectInterruptPage(fexmac_thread* Thread, bool ReadOnly);

// Discards translations of [Start, Start + Length) for every thread. The caller holds the
// context's ThreadsMutex and the core's invalidation lock exclusively.
void InvalidateLocked(fexmac_context* Context, uint64_t Start, uint64_t Length);
// The signals blocked while a host thread that is not a guest thread waits for the
// invalidation lock.
uint64_t InvalidationSignalMask();

// Called in ordinary context after the fault handler left translated code because a
// guest write hit a page the library write-protected for code (Memory.cpp), and once the
// store has run (or the run ended first).
void ResolveCodeWrite(fexmac_thread* Thread, uint64_t Address);
void FinishCodeWrite(fexmac_thread* Thread);

// The process's one context, for the fault handler's message about host writes to
// write-protected code from any host thread. Set while the context exists.
extern std::atomic<fexmac_context*> ProcessContext;

// Fills a stop record for a guest page fault: signal, si_code, #PF error code and access
// from what the embedder registered (or, without registration, from the host fault).
void DescribePageFault(fexmac_context* Context, fexmac_stop* Stop, uint64_t Address, uint32_t Access, int HostSignal, bool HostPermissionFault);
} // namespace fexmac
