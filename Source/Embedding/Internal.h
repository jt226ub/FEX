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

#include <mutex>
#include <optional>
#include <utility>

namespace fexmac {

// The core needs somewhere to publish the dispatcher's entry and exit points. The
// library uses ThreadStopHandlerAddress to leave the dispatcher; package D will use the
// rest to stop a thread from another thread.
class SignalDelegator final : public FEXCore::SignalDelegator, public FEXCore::Allocator::FEXAllocOperators {};

class SyscallHandler final : public FEXCore::HLE::SyscallHandler, public FEXCore::Allocator::FEXAllocOperators {
public:
  explicit SyscallHandler(fexmac_context* Context)
    : Context {Context} {}

  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override;
  void InvalidateGuestCodeRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) override;

  // EXTENSION POINT (packages D and F): every guest address is treated as executable
  // and writable code. Once the embedder can register its mapped guest ranges (see the
  // planned functions in fexmac.h), this reports the real range and its writability;
  // page-protection self-modifying-code detection (D) and cached-code keying (F) need it.
  // MarkGuestExecutableRange is where the core reports ranges it starts executing.
  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return {0, UINT64_MAX, true};
  }

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
  // lock may be held when taking this one, never the other way round.
  std::mutex ThreadsMutex;
  fextl::vector<fexmac_thread*> Threads;
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
};
