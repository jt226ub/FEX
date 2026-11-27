// SPDX-License-Identifier: MIT
//
// fexmac: contexts, threads, guest state and running guest code. See fexmac.h.
#include "Internal.h"

#include "Common/HostFeatures.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/SignalScopeGuards.h>
#include <FEXCore/Utils/TypeDefines.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <unordered_set>

namespace fexmac {
namespace {
  pthread_key_t CurrentThreadKey;
  pthread_once_t CurrentThreadKeyOnce = PTHREAD_ONCE_INIT;
  void CreateCurrentThreadKey() {
    pthread_key_create(&CurrentThreadKey, nullptr);
  }
} // namespace

fexmac_thread* GetCurrentThread() {
  pthread_once(&CurrentThreadKeyOnce, CreateCurrentThreadKey);
  return static_cast<fexmac_thread*>(pthread_getspecific(CurrentThreadKey));
}

void SetCurrentThread(fexmac_thread* Thread) {
  pthread_once(&CurrentThreadKeyOnce, CreateCurrentThreadKey);
  pthread_setspecific(CurrentThreadKey, Thread);
}

namespace {
  // Handles are checked against these sets before they are used, so a destroyed handle
  // is reported instead of being dereferenced.
  std::mutex RegistryMutex;
  std::unordered_set<const void*> LiveContexts;
  std::unordered_set<const void*> LiveThreads;
  // The core's configuration and log hooks are process-wide: one context at a time.
  bool ContextExists {};

  bool IsLiveContext(const fexmac_context* Context) {
    std::lock_guard Lock {RegistryMutex};
    return Context && LiveContexts.contains(Context);
  }

  bool IsLiveThread(const fexmac_thread* Thread) {
    std::lock_guard Lock {RegistryMutex};
    return Thread && LiveThreads.contains(Thread);
  }

  // Log routing. The core's hooks are plain function pointers, so the embedder's
  // callback lives here.
  fexmac_log_fn LogFn {};
  void* LogUserData {};

  void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
    if (LogFn) {
      LogFn(static_cast<fexmac_log_level>(Level), Message, LogUserData);
    } else {
      fprintf(stderr, "[%s] %s\n", LogMan::DebugLevelStr(Level), Message);
      fflush(stderr);
    }
  }

  void AssertHandler(const char* Message) {
    MsgHandler(LogMan::ASSERT, Message);
  }

  // Every host thread that runs guest code or creates threads needs the core
  // allocator's per-thread state. Initialising it again is harmless; this just avoids
  // the call on every run.
  void InitializeAllocatorForThisThread() {
    static thread_local bool Initialized {};
    if (!Initialized) {
      FEXCore::Allocator::InitializeThread();
      Initialized = true;
    }
  }

  // Leaves the dispatcher the way the core itself does when a thread is stopped: reset
  // the stack to where the dispatcher saved it on entry and jump to its stop handler,
  // which restores the host's callee-saved registers and returns from ExecuteThread.
  // Only valid when the guest's registers are already written back to the frame, which
  // is the case in the system call and synchronous exception handlers (the core spills
  // them before calling either), so the stop handler's non-spilling entry is used.
  [[noreturn]]
  __attribute__((naked)) void JumpToStopHandler(uint64_t /*StackLocation*/, uint64_t /*Target*/) {
    asm volatile("mov sp, x0\n\t"
                 "br x1\n\t");
  }

  [[noreturn]]
  void LeaveDispatcher(fexmac_thread* Thread, FEXCore::Core::CpuStateFrame* Frame) {
    const uint64_t Target = Thread->Context->Delegator->GetConfig().ThreadStopHandlerAddress;
    JumpToStopHandler(Frame->ReturningStackLocation, Target);
  }

  // Called by the core, from the dispatcher, for guest exceptions it detects without a
  // host fault (hlt, ud2, int3, ...). Guest state is in the frame.
  [[noreturn]]
  void SynchronousExceptionHandler(FEXCore::Context::Context*, FEXCore::Core::CpuStateFrame* Frame) {
    auto* Thread = static_cast<fexmac_thread*>(Frame->Thread->FrontendPtr);
    const auto& Fault = Frame->SynchronousFaultData;
    fexmac_stop Stop {};
    Stop.reason = FEXMAC_STOP_GUEST_FAULT;
    Stop.signal = Fault.Signal;
    Stop.trap_number = Fault.TrapNo;
    Stop.si_code = Fault.si_code;
    Stop.error_code = Fault.err_code;
    Stop.fault_address = Fault.FaultAddress;

    // A user-mode `hlt` is a general protection fault at the instruction. The core
    // leaves rip on it, and the instruction was just decoded, so the byte is readable.
    constexpr uint8_t HLT = 0xF4;
    if (Fault.Signal == FEXCore::Core::FAULT_SIGSEGV && Fault.TrapNo == FEXCore::X86State::X86_TRAPNO_GP &&
        *reinterpret_cast<const uint8_t*>(Frame->State.rip) == HLT) {
      Stop.reason = FEXMAC_STOP_HALT;
    }
    Thread->PendingStop = Stop;
    LeaveDispatcher(Thread, Frame);
  }

  void ReadState(fexmac_thread* Thread, fexmac_x86_state* Out) {
    auto* CTX = Thread->Context->CTX.get();
    const auto& State = Thread->Thread->CurrentFrame->State;
    memset(Out, 0, sizeof(*Out));
    memcpy(Out->gpr, State.gregs, sizeof(Out->gpr));
    Out->rip = State.rip;
    Out->rflags = CTX->ReconstructCompactedEFLAGS(Thread->Thread, false, nullptr, 0);
    Out->fs_base = State.fs_cached;
    Out->gs_base = State.gs_cached;
    Out->mxcsr = State.mxcsr;
    Out->fcw = State.FCW;
    Out->ftw = State.AbridgedFTW;

    using namespace FEXCore::X86State;
    uint16_t FSW = static_cast<uint16_t>((State.flags[X87FLAG_TOP_LOC] & 0b111) << 11);
    for (unsigned Bit : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 14U, 15U}) {
      FSW |= static_cast<uint16_t>((State.flags[X87FLAG_BASE + Bit] & 1) << Bit);
    }
    Out->fsw = FSW;
    memcpy(Out->mm, State.mm, sizeof(Out->mm));

    __uint128_t Low[16] {};
    __uint128_t High[16] {};
    CTX->ReconstructXMMRegisters(Thread->Thread, Low, High);
    memcpy(Out->xmm, Low, sizeof(Out->xmm));
    memcpy(Out->ymm_high, High, sizeof(Out->ymm_high));
  }

  void WriteState(fexmac_thread* Thread, const fexmac_x86_state* In) {
    auto* CTX = Thread->Context->CTX.get();
    auto& State = Thread->Thread->CurrentFrame->State;
    memcpy(State.gregs, In->gpr, sizeof(In->gpr));
    State.rip = In->rip;
    CTX->SetFlagsFromCompactedEFLAGS(Thread->Thread, static_cast<uint32_t>(In->rflags));
    State.fs_cached = In->fs_base;
    State.gs_cached = In->gs_base;
    State.mxcsr = In->mxcsr;
    State.FCW = In->fcw;
    State.AbridgedFTW = In->ftw;

    using namespace FEXCore::X86State;
    State.flags[X87FLAG_TOP_LOC] = (In->fsw >> 11) & 0b111;
    for (unsigned Bit : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 14U, 15U}) {
      State.flags[X87FLAG_BASE + Bit] = (In->fsw >> Bit) & 1;
    }
    memcpy(State.mm, In->mm, sizeof(State.mm));

    __uint128_t Low[16];
    __uint128_t High[16];
    memcpy(Low, In->xmm, sizeof(Low));
    memcpy(High, In->ymm_high, sizeof(High));
    CTX->SetXMMRegistersFromState(Thread->Thread, Low, High);
  }

  // State may be touched when the thread is not running, or from its own system call
  // callback on the host thread running it.
  bool MayAccessState(fexmac_thread* Thread) {
    if (!Thread->Running.load()) {
      return true;
    }
    return Thread->InCallback && GetCurrentThread() == Thread;
  }

  // The core keeps a private stack of return addresses so that guest `call` and `ret`
  // pair up as host calls and returns. It has an inaccessible guard page on each side;
  // guard pages are a whole host page, larger than a guest page on Apple silicon.
  bool AllocateCallRetStack(fexmac_thread* Thread) {
    constexpr size_t Guard = FEXCore::Utils::FEX_HOST_PAGE_SIZE;
    constexpr size_t Size = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
    void* Alloc = ::mmap(nullptr, Size + 2 * Guard, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (Alloc == MAP_FAILED) {
      return false;
    }
    const auto Base = reinterpret_cast<uint64_t>(Alloc) + Guard;
    if (::mprotect(reinterpret_cast<void*>(Base), Size, PROT_READ | PROT_WRITE) != 0) {
      ::munmap(Alloc, Size + 2 * Guard);
      return false;
    }
    Thread->CallRetAllocation = Alloc;
    Thread->CallRetAllocationSize = Size + 2 * Guard;
    Thread->Thread->CallRetStackBase = reinterpret_cast<void*>(Base);
    // Start a quarter of the way in, as FEX's Linux layer does, leaving room both ways.
    Thread->Thread->CurrentFrame->State.callret_sp = Base + Size / 4;
    return true;
  }

  fexmac_result Invalidate(fexmac_context* Context, FEXCore::Core::InternalThreadState* CallingThread, uint64_t Start, uint64_t Length) {
    std::lock_guard ThreadsLock {Context->ThreadsMutex};
    // Holding the core's invalidation lock keeps other threads from compiling or linking
    // blocks for the range meanwhile. With no calling guest thread, signals are masked
    // instead of deferred.
    auto Lock = FEXCore::GuardSignalDeferringSectionWithFallback(Context->CTX->GetCodeInvalidationMutex(), CallingThread);
    Context->CTX->InvalidateCodeBuffersCodeRange(Start, Length);
    for (auto* Thread : Context->Threads) {
      Context->CTX->InvalidateThreadCachedCodeRange(Thread->Thread, Start, Length);
    }
    return FEXMAC_OK;
  }
} // namespace

void SyscallHandler::HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) {
  auto* Thread = static_cast<fexmac_thread*>(Frame->Thread->FrontendPtr);
  // The core leaves rip on the `syscall` instruction; skipping it is the handler's job.
  // Done before the callback so that it sees the same rip as the guest's rcx, and so
  // that resuming and stopping both continue after the instruction.
  constexpr uint64_t SyscallInstructionSize = 2;
  Frame->State.rip += SyscallInstructionSize;
  fexmac_syscall_action Action = FEXMAC_SYSCALL_STOP;
  if (Context->SyscallFn) {
    Thread->InCallback = true;
    Action = Context->SyscallFn(Thread, Context->SyscallUserData);
    Thread->InCallback = false;
  }
  if (Action == FEXMAC_SYSCALL_RESUME) {
    return;
  }
  Thread->PendingStop = fexmac_stop {.reason = FEXMAC_STOP_SYSCALL};
  // The dispatcher marks the thread as inside a system call until the handler returns;
  // this one does not return, so clear the mark here.
  Frame->InSyscallInfo = 0;
  LeaveDispatcher(Thread, Frame);
}

// The core calls this when translated code finds that the guest code it came from has
// changed. Without discarding the stale translations it finds the same block again and
// never makes progress.
void SyscallHandler::InvalidateGuestCodeRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) {
  Invalidate(Context, Thread, Start, Length);
}
} // namespace fexmac

using namespace fexmac;

extern "C" {

const char* fexmac_result_string(fexmac_result result) {
  switch (result) {
  case FEXMAC_OK: return "success";
  case FEXMAC_ERROR_INVALID_ARGUMENT: return "invalid argument";
  case FEXMAC_ERROR_INVALID_HANDLE: return "not a live handle";
  case FEXMAC_ERROR_BAD_CONFIG: return "bad configuration option";
  case FEXMAC_ERROR_BUSY: return "busy";
  case FEXMAC_ERROR_INIT_FAILED: return "core initialisation failed";
  case FEXMAC_ERROR_OUT_OF_MEMORY: return "out of memory";
  case FEXMAC_ERROR_UNSUPPORTED: return "not supported";
  case FEXMAC_ERROR_INTERNAL: return "internal error";
  }
  return "unknown result";
}

uint32_t fexmac_version(void) {
  return FEXMAC_VERSION;
}

void fexmac_config_init(fexmac_config* config) {
  if (!config) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->struct_size = sizeof(*config);
}

fexmac_result fexmac_context_create(const fexmac_config* config, fexmac_context** out_context) {
  if (!config || !out_context || config->struct_size != sizeof(fexmac_config)) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  *out_context = nullptr;

  {
    std::lock_guard Lock {RegistryMutex};
    if (ContextExists) {
      return FEXMAC_ERROR_BUSY;
    }
    ContextExists = true;
  }
  auto Release = [] {
    std::lock_guard Lock {RegistryMutex};
    ContextExists = false;
  };

  LogFn = config->log;
  LogUserData = config->log_user_data;
  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);
  InitializeAllocatorForThisThread();

  // Library defaults first, so the embedder's options override them.
  ParsedOptions Parsed {
    {FEXCore::Config::CONFIG_DISABLETELEMETRY, "1"},
  };
  if (const auto Result = ParseOptions(*config, &Parsed); Result != FEXMAC_OK) {
    Release();
    return Result;
  }

  // Package F: cached-code loading will add its options (cache directory, keying) to the
  // config and set them up here, before the context is created.
  FEXCore::Config::Initialize();
  FEXCore::Config::AddLayer(CreateOptionsLayer(std::move(Parsed)));
  FEXCore::Config::Load();
  FEXCore::Config::ReloadMetaLayer();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");

  auto* Context = new (std::nothrow) fexmac_context {};
  if (!Context) {
    FEXCore::Config::Shutdown();
    Release();
    return FEXMAC_ERROR_OUT_OF_MEMORY;
  }
  Context->SyscallFn = config->syscall;
  Context->SyscallUserData = config->syscall_user_data;

  {
    auto HostFeatures = FEX::FetchHostFeatures(true);
    Context->CTX = FEXCore::Context::Context::CreateNewContext(HostFeatures);
  }
  Context->Delegator = fextl::make_unique<fexmac::SignalDelegator>();
  Context->Syscalls = fextl::make_unique<fexmac::SyscallHandler>(Context);
  Context->CTX->SetSignalDelegator(Context->Delegator.get());
  Context->CTX->SetSyscallHandler(Context->Syscalls.get());
  if (!Context->CTX->InitCore()) {
    delete Context;
    FEXCore::Config::Shutdown();
    Release();
    return FEXMAC_ERROR_INIT_FAILED;
  }

  Context->Descriptors = new (std::nothrow) FEXCore::Core::CPUState::gdt_segment[fexmac_context::DescriptorCount] {};
  if (!Context->Descriptors) {
    delete Context;
    FEXCore::Config::Shutdown();
    Release();
    return FEXMAC_ERROR_OUT_OF_MEMORY;
  }
  // A 64-bit user code segment, as a 64-bit Linux process has.
  auto& CodeSegment = Context->Descriptors[FEXCore::Core::CPUState::DEFAULT_USER_CS];
  FEXCore::Core::CPUState::SetGDTBase(&CodeSegment, 0);
  FEXCore::Core::CPUState::SetGDTLimit(&CodeSegment, 0xF'FFFFU);
  CodeSegment.L = 1;
  CodeSegment.D = 0;

  {
    std::lock_guard Lock {RegistryMutex};
    LiveContexts.insert(Context);
  }
  *out_context = Context;
  return FEXMAC_OK;
}

fexmac_result fexmac_context_destroy(fexmac_context* context) {
  {
    std::lock_guard Lock {RegistryMutex};
    if (!context || !LiveContexts.contains(context)) {
      return FEXMAC_ERROR_INVALID_HANDLE;
    }
    std::lock_guard ThreadsLock {context->ThreadsMutex};
    if (!context->Threads.empty()) {
      return FEXMAC_ERROR_BUSY;
    }
    LiveContexts.erase(context);
  }

  context->CTX.reset();
  context->Syscalls.reset();
  context->Delegator.reset();
  delete[] context->Descriptors;
  delete context;
  FEXCore::Config::Shutdown();
  LogFn = nullptr;
  LogUserData = nullptr;

  std::lock_guard Lock {RegistryMutex};
  ContextExists = false;
  return FEXMAC_OK;
}

fexmac_result fexmac_cpuid(fexmac_context* context, uint32_t function, uint32_t leaf, uint32_t out[4]) {
  if (!out) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  if (!IsLiveContext(context)) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  const auto Result = context->CTX->RunCPUIDFunction(function, leaf);
  out[0] = Result.eax;
  out[1] = Result.ebx;
  out[2] = Result.ecx;
  out[3] = Result.edx;
  return FEXMAC_OK;
}

fexmac_result fexmac_invalidate(fexmac_context* context, uint64_t address, uint64_t size) {
  if (!IsLiveContext(context)) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  if (size == 0) {
    return FEXMAC_OK;
  }
  auto* Current = GetCurrentThread();
  auto* Calling = Current && Current->Context == context ? Current->Thread : nullptr;
  return Invalidate(context, Calling, address, size);
}

fexmac_result fexmac_thread_create(fexmac_context* context, fexmac_thread** out_thread) {
  if (!out_thread) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  *out_thread = nullptr;
  if (!IsLiveContext(context)) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  if (auto* Current = GetCurrentThread(); Current && Current->InCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  InitializeAllocatorForThisThread();

  auto* Thread = new (std::nothrow) fexmac_thread {};
  if (!Thread) {
    return FEXMAC_ERROR_OUT_OF_MEMORY;
  }
  Thread->Context = context;

  std::lock_guard Lock {context->ThreadsMutex};
  Thread->Thread = context->CTX->CreateThread();
  if (!Thread->Thread) {
    delete Thread;
    return FEXMAC_ERROR_OUT_OF_MEMORY;
  }
  if (!AllocateCallRetStack(Thread)) {
    context->CTX->DestroyThread(Thread->Thread);
    delete Thread;
    return FEXMAC_ERROR_OUT_OF_MEMORY;
  }

  auto* Frame = Thread->Thread->CurrentFrame;
  Thread->Thread->FrontendPtr = Thread;
  Frame->State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = context->Descriptors;
  Frame->State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = context->Descriptors;
  Frame->State.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
  Frame->State.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(context->Descriptors[FEXCore::Core::CPUState::DEFAULT_USER_CS]);
  Frame->Pointers.SynchronousExceptionHandler = reinterpret_cast<uintptr_t>(&SynchronousExceptionHandler);

  context->Threads.push_back(Thread);
  {
    std::lock_guard RegistryLock {RegistryMutex};
    LiveThreads.insert(Thread);
  }
  *out_thread = Thread;
  return FEXMAC_OK;
}

fexmac_result fexmac_thread_destroy(fexmac_thread* thread) {
  {
    std::lock_guard Lock {RegistryMutex};
    if (!thread || !LiveThreads.contains(thread)) {
      return FEXMAC_ERROR_INVALID_HANDLE;
    }
    if (thread->Running.load()) {
      return FEXMAC_ERROR_BUSY;
    }
    LiveThreads.erase(thread);
  }

  auto* Context = thread->Context;
  {
    std::lock_guard Lock {Context->ThreadsMutex};
    auto& Threads = Context->Threads;
    Threads.erase(std::remove(Threads.begin(), Threads.end(), thread), Threads.end());
    Context->CTX->DestroyThread(thread->Thread);
  }
  if (thread->CallRetAllocation) {
    ::munmap(thread->CallRetAllocation, thread->CallRetAllocationSize);
  }
  delete thread;
  return FEXMAC_OK;
}

fexmac_context* fexmac_thread_context(fexmac_thread* thread) {
  return IsLiveThread(thread) ? thread->Context : nullptr;
}

void fexmac_thread_set_user_data(fexmac_thread* thread, void* user_data) {
  if (IsLiveThread(thread)) {
    thread->UserData = user_data;
  }
}

void* fexmac_thread_get_user_data(fexmac_thread* thread) {
  return IsLiveThread(thread) ? thread->UserData : nullptr;
}

fexmac_result fexmac_thread_get_state(fexmac_thread* thread, fexmac_x86_state* state) {
  if (!state) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  if (!IsLiveThread(thread)) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  if (!MayAccessState(thread)) {
    return FEXMAC_ERROR_BUSY;
  }
  ReadState(thread, state);
  return FEXMAC_OK;
}

fexmac_result fexmac_thread_set_state(fexmac_thread* thread, const fexmac_x86_state* state) {
  if (!state) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  if (!IsLiveThread(thread)) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  if (!MayAccessState(thread)) {
    return FEXMAC_ERROR_BUSY;
  }
  WriteState(thread, state);
  return FEXMAC_OK;
}

fexmac_result fexmac_thread_run(fexmac_thread* thread, fexmac_stop* stop) {
  if (!IsLiveThread(thread)) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  // Running another guest thread from inside a system call callback would nest
  // dispatchers on one host stack.
  if (auto* Current = GetCurrentThread(); Current && Current->InCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  if (thread->Running.exchange(true)) {
    return FEXMAC_ERROR_BUSY;
  }
  InitializeAllocatorForThisThread();

  // Faults are only claimed for the thread this host thread is running.
  auto* Previous = GetCurrentThread();
  SetCurrentThread(thread);
  thread->PendingStop = {};

  thread->Context->CTX->ExecuteThread(thread->Thread);

  SetCurrentThread(Previous);
  const fexmac_stop Result = thread->PendingStop;
  thread->Running.store(false);

  if (stop) {
    *stop = Result;
  }
  return Result.reason == FEXMAC_STOP_NONE ? FEXMAC_ERROR_INTERNAL : FEXMAC_OK;
}

} // extern "C"
