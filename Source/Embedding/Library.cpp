// SPDX-License-Identifier: MIT
//
// fexmac: contexts, threads, guest state and running guest code. See fexmac.h.
//
// Locking. One registry lock guards the tables of live handles, every thread's Running
// and InCallback flags and every context's ActiveCalls count, so that checking a handle
// and changing its state is one step: a thread cannot be destroyed between the check
// in fexmac_thread_run and the moment it is marked running, and a context cannot be
// destroyed while a call that uses it is in progress. A context's ThreadsMutex may be
// taken while the registry lock is held, never the reverse.
#include "Internal.h"
#ifdef FEXMAC_TESTING
#include "fexmac_testing.h"
#endif

#include "Common/HostFeatures.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/SignalScopeGuards.h>
#include <FEXCore/Utils/TypeDefines.h>
#include <FEXCore/Utils/IntervalList.h>
#include <FEXCore/fextl/set.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
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

void WriteRaw(const char* Message) {
  size_t Length = 0;
  while (Message[Length]) {
    ++Length;
  }
  (void)!::write(STDERR_FILENO, Message, Length);
}

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

void ProtectInterruptPage(fexmac_thread* Thread, bool ReadOnly) {
  ::mprotect(InterruptPage(Thread), FEXCore::Utils::FEX_HOST_PAGE_SIZE, ReadOnly ? PROT_READ : PROT_READ | PROT_WRITE);
}

namespace {
  std::mutex RegistryMutex;
  std::unordered_set<const void*> LiveContexts;
  std::unordered_set<const void*> LiveThreads;
  // The core's configuration and log hooks are process-wide: one context at a time.
  bool ContextExists {};

  // Log routing. The core's hooks are plain function pointers, so the embedder's
  // callback lives here.
  fexmac_log_fn LogFn {};
  void* LogUserData {};

  // Set while the embedder's log callback runs on this host thread. The core can log
  // while the library holds its registry lock, so a callback that called back into the
  // library could deadlock; such calls are refused with FEXMAC_ERROR_BUSY (or ignored,
  // for the functions that return no result). Never read from a signal handler.
  thread_local bool InLogCallback {};

  void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
    if (InFaultHandling()) {
      // Never call the embedder or stdio from a signal handler.
      WriteRaw("fexmac: core message during fault handling: ");
      WriteRaw(Message);
      WriteRaw("\n");
      return;
    }
    if (LogFn) {
      InLogCallback = true;
      LogFn(static_cast<fexmac_log_level>(Level), Message, LogUserData);
      InLogCallback = false;
    } else {
      fprintf(stderr, "[%s] %s\n", LogMan::DebugLevelStr(Level), Message);
      fflush(stderr);
    }
  }

  void AssertHandler(const char* Message) {
    MsgHandler(LogMan::ASSERT, Message);
  }

  void InstallLogging(fexmac_log_fn Fn, void* UserData) {
    LogFn = Fn;
    LogUserData = UserData;
    LogMan::Throw::InstallHandler(AssertHandler);
    LogMan::Msg::InstallHandler(MsgHandler);
  }

  void RemoveLogging() {
    LogMan::Throw::UnInstallHandler();
    LogMan::Msg::UnInstallHandler();
    LogFn = nullptr;
    LogUserData = nullptr;
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

  bool InCallbackOnThisHostThread() {
    auto* Current = GetCurrentThread();
    return Current && Current->InCallback;
  }

  // Holds a context in use for the length of a call that does not keep the registry
  // lock, so fexmac_context_destroy refuses meanwhile.
  class ContextUse {
  public:
    explicit ContextUse(fexmac_context* Context) {
      std::lock_guard Lock {RegistryMutex};
      if (Context && LiveContexts.contains(Context)) {
        ++Context->ActiveCalls;
        Held = Context;
      }
    }
    ~ContextUse() {
      if (Held) {
        std::lock_guard Lock {RegistryMutex};
        --Held->ActiveCalls;
      }
    }
    ContextUse(const ContextUse&) = delete;
    ContextUse& operator=(const ContextUse&) = delete;
    explicit operator bool() const {
      return Held != nullptr;
    }

  private:
    fexmac_context* Held {};
  };

  // ---- Caller-allocated structures with a struct_size ----

  // struct_size must be the size of a layout this library knows, or nothing is read or
  // written: a garbage size (an uninitialised structure) must never make the library write
  // past the caller's object. Version 2.0 is the first layout of each structure, so its
  // size is the only one; a later minor version that grows a structure adds the older
  // size here and handles it field by field.
  template<typename T>
  bool KnownSize(const T* Value) {
    return Value->struct_size == sizeof(T);
  }

  // ---- Leaving the dispatcher ----

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
    Stop.struct_size = sizeof(Stop);
    Stop.reason = FEXMAC_STOP_GUEST_FAULT;
    Stop.instruction_address = Frame->State.rip;
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
    // The core marks a block it could not translate at all (CompileBlockOrFault).
    if (Fault.Untranslatable) {
      Stop.reason = FEXMAC_STOP_UNSUPPORTED_INSTRUCTION;
    }
    // The only page fault the core raises itself is an instruction fetch from memory
    // QueryGuestExecutableRange says is not executable (NoExecOrPartialOp), at the first
    // byte it could not read. Describe it from the registered memory, as for data.
    if (Fault.TrapNo == FEXCore::X86State::X86_TRAPNO_PF && !Fault.Untranslatable) {
      DescribePageFault(Thread->Context, &Stop, Fault.FaultAddress, FEXMAC_ACCESS_EXECUTE, SIGSEGV, true);
    }
    Thread->PendingStop = Stop;
    LeaveDispatcher(Thread, Frame);
  }

  // ---- Guest state ----

  // What a user-mode program can set (popf, sigreturn): CF PF AF ZF SF TF DF OF NT AC ID.
  // Bit 1 and IF (bit 9) always read as set in user mode; IOPL, RF, VM, VIF and VIP are
  // system state that user mode cannot set, and the core does not honour.
  constexpr uint32_t X86UserFlags = 0x00244DD5;
  constexpr uint32_t X86FixedFlags = 0x00000202;

  void ReadState(fexmac_thread* Thread, fexmac_x86_state* Out) {
    auto* CTX = Thread->Context->CTX.get();
    const auto& State = Thread->Thread->CurrentFrame->State;
    const uint32_t Size = Out->struct_size;
    memset(Out, 0, sizeof(*Out));
    Out->struct_size = Size;
    memcpy(Out->gpr, State.gregs, sizeof(Out->gpr));
    Out->rip = State.rip;
    // The core's reconstruction also ORs in the bytes where it keeps NZCV (flag slots 24
    // to 27, shifted into bits 24 to 31), so bit 31 could read as set (0x80000a03 after an
    // rcl that set OF). Only the user-mode flags are reported, with bit 1 and IF set.
    Out->rflags = (CTX->ReconstructCompactedEFLAGS(Thread->Thread, false, nullptr, 0) & X86UserFlags) | X86FixedFlags;
    Out->fs_base = State.fs_cached;
    Out->gs_base = State.gs_cached;
    Out->cs = State.cs_idx;
    Out->ds = State.ds_idx;
    Out->es = State.es_idx;
    Out->fs = State.fs_idx;
    Out->gs = State.gs_idx;
    Out->ss = State.ss_idx;
    Out->mxcsr = State.mxcsr;
    Out->mxcsr_mask = 0xFFFF;
    Out->fcw = State.FCW;
    Out->ftw = State.AbridgedFTW;
    Out->fip = State.FIP;

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
    // Only what a user-mode sigreturn may set; system and reserved bits are ignored.
    CTX->SetFlagsFromCompactedEFLAGS(Thread->Thread, (static_cast<uint32_t>(In->rflags) & X86UserFlags) | X86FixedFlags);
    State.fs_cached = In->fs_base;
    State.gs_cached = In->gs_base;

    // Selectors index the context's 8192-entry table (both TI values), so any 16-bit
    // value reads a valid entry.
    using CPUState = FEXCore::Core::CPUState;
    State.cs_idx = In->cs;
    State.ds_idx = In->ds;
    State.es_idx = In->es;
    State.ss_idx = In->ss;
    State.fs_idx = In->fs;
    State.gs_idx = In->gs;
    State.cs_cached = CPUState::CalculateGDTBase(*CPUState::GetSegmentFromIndex(State, State.cs_idx));
    State.ds_cached = CPUState::CalculateGDTBase(*CPUState::GetSegmentFromIndex(State, State.ds_idx));
    State.es_cached = CPUState::CalculateGDTBase(*CPUState::GetSegmentFromIndex(State, State.es_idx));
    State.ss_cached = CPUState::CalculateGDTBase(*CPUState::GetSegmentFromIndex(State, State.ss_idx));

    State.mxcsr = In->mxcsr;
    State.FCW = In->fcw;
    State.AbridgedFTW = In->ftw;
    State.FIP = In->fip;

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

  // Under the registry lock: state may be touched when the thread is not running, or
  // from its own system call callback on the host thread running it.
  bool MayAccessState(fexmac_thread* Thread) {
    if (!Thread->Running) {
      return true;
    }
    return Thread->InCallback && GetCurrentThread() == Thread;
  }

  // The core keeps a private stack of return addresses so that guest `call` and `ret`
  // pair up as host calls and returns. It has an inaccessible guard page on each side;
  // guard pages are a whole host page, larger than a guest page on Apple silicon.
  // Running into a guard page is handled in Faults.cpp.
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
    Thread->CallRetDefault = Base + Size / 4;
    Thread->Thread->CurrentFrame->State.callret_sp = Thread->CallRetDefault;
    return true;
  }

#ifdef FEXMAC_TESTING
  std::atomic<void (*)()> TestInvalidationHook {};
  // Called where the core reports guest code it starts executing: in CompileBlock after
  // compiling and before storing to the code cache.
  std::atomic<void (*)(uint64_t, uint64_t, void*)> TestMarkHook {};
  std::atomic<void*> TestMarkHookData {};
#endif

  void Invalidate(fexmac_context* Context, FEXCore::Core::InternalThreadState* CallingThread, uint64_t Start, uint64_t Length) {
    std::lock_guard ThreadsLock {Context->ThreadsMutex};
    // Holding the core's invalidation lock keeps other threads from compiling or linking
    // blocks for the range meanwhile. A guest thread defers its signals; any other thread
    // masks the asynchronous ones.
    auto Lock =
      FEXCore::GuardSignalDeferringSectionWithFallback(Context->CTX->GetCodeInvalidationMutex(), CallingThread, InvalidationSignalMask());
#ifdef FEXMAC_TESTING
    if (auto Hook = TestInvalidationHook.load()) {
      Hook();
    }
#endif
    InvalidateLocked(Context, Start, Length);
  }

  void SetInCallback(fexmac_thread* Thread, bool Value) {
    std::lock_guard Lock {RegistryMutex};
    Thread->InCallback = Value;
  }
} // namespace

// The signals a thread that waits for the invalidation lock blocks when it is not a
// guest thread: every asynchronous one, so the core's lock is not taken re-entrantly
// from a host signal handler, but no synchronous fault signal, so a fault while
// waiting still reaches its handler instead of hanging the thread.
uint64_t InvalidationSignalMask() {
  uint64_t Mask = ~0ULL;
  for (int Signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGSYS}) {
    Mask &= ~(1ULL << (Signal - 1));
  }
  return Mask;
}

void InvalidateLocked(fexmac_context* Context, uint64_t Start, uint64_t Length) {
  Context->CTX->InvalidateCodeBuffersCodeRange(Start, Length);
  for (auto* Thread : Context->Threads) {
    Context->CTX->InvalidateThreadCachedCodeRange(Thread->Thread, Start, Length);
  }
}

fexmac_result SetGuestMemory(fexmac_context* Context, uint64_t Address, uint64_t Size, uint8_t Value, bool RequireMapped, bool AllowUnenforced,
                             bool* Unenforced = nullptr);
fexmac_result WriteBegin(fexmac_context* Context, uint64_t Address, uint64_t Size, bool Begin, uint32_t Flags = 0,
                         uint64_t* FirstUnwritable = nullptr);

void SyscallHandler::HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) {
  auto* Thread = static_cast<fexmac_thread*>(Frame->Thread->FrontendPtr);
  // The core leaves rip on the start of the `syscall` instruction, prefixes included,
  // and has just stored the address of the next instruction in rcx, as the instruction
  // itself does. Skipping the instruction is the handler's job; the length comes from
  // there because a `syscall` may carry prefixes (48 0F 05 is three bytes). Done before
  // the callback, so it sees the same rip as the guest's rcx.
  const uint64_t InstructionAddress = Frame->State.rip;
  Frame->State.rip = Frame->State.gregs[FEXCore::X86State::REG_RCX];

  fexmac_syscall_action Action = FEXMAC_SYSCALL_STOP;
  if (Context->SyscallFn) {
    SetInCallback(Thread, true);
    Action = Context->SyscallFn(Thread, Context->SyscallUserData);
    SetInCallback(Thread, false);
  }
  if (Action == FEXMAC_SYSCALL_RESUME) {
    return;
  }
  if (Action == FEXMAC_SYSCALL_RESTART) {
    Frame->State.rip = InstructionAddress;
    return;
  }
  Thread->PendingStop = fexmac_stop {.struct_size = sizeof(fexmac_stop), .reason = FEXMAC_STOP_SYSCALL, .instruction_address = InstructionAddress};
  // The dispatcher marks the thread as inside a system call until the handler returns;
  // this one does not return, so clear the mark here.
  Frame->InSyscallInfo = 0;
  LeaveDispatcher(Thread, Frame);
}

// The core calls this when translated code finds that the guest code it came from has
// changed. Without discarding the stale translations it finds the same block again and
// never makes progress. The calling thread is running, so the context is alive.
void SyscallHandler::InvalidateGuestCodeRange(FEXCore::Core::InternalThreadState* Thread, uint64_t Start, uint64_t Length) {
  Invalidate(Context, Thread, Start, Length);
}

#ifdef FEXMAC_TESTING
void CallTestMarkHook(uint64_t Start, uint64_t Length) {
  if (auto Hook = TestMarkHook.load()) {
    Hook(Start, Length, TestMarkHookData.load());
  }
}
#endif
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
  case FEXMAC_DONE_UNENFORCED: return "done, but the host page keeps the unmapped guest pages accessible";
  }
  return "unknown result";
}

uint32_t fexmac_version(void) {
  return FEXMAC_VERSION;
}

void fexmac_config_init(fexmac_config* config) {
  if (config) {
    memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
  }
}

void fexmac_x86_state_init(fexmac_x86_state* state) {
  if (state) {
    memset(state, 0, sizeof(*state));
    state->struct_size = sizeof(*state);
  }
}

void fexmac_stop_init(fexmac_stop* stop) {
  if (stop) {
    memset(stop, 0, sizeof(*stop));
    stop->struct_size = sizeof(*stop);
  }
}

fexmac_result fexmac_context_create(const fexmac_config* config, fexmac_context** out_context) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  if (!config || !out_context) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  if (!KnownSize(config)) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  *out_context = nullptr;
  if (config->reserved || config->reserved2) {
    return FEXMAC_ERROR_UNSUPPORTED;
  }
  if (config->code_cache_enabled > 1 || (!config->code_cache_enabled && config->code_cache_directory) ||
      (config->code_cache_directory && !config->code_cache_directory[0])) {
    return FEXMAC_ERROR_BAD_CONFIG;
  }

  {
    std::lock_guard Lock {RegistryMutex};
    if (ContextExists) {
      return FEXMAC_ERROR_BUSY;
    }
    ContextExists = true;
  }
  // Undoes everything process-wide on a failure.
  auto Fail = [](fexmac_result Result, bool ConfigInitialised) {
    if (ConfigInitialised) {
      FEXCore::Config::Shutdown();
    }
    RemoveLogging();
    std::lock_guard Lock {RegistryMutex};
    ContextExists = false;
    return Result;
  };

  InstallLogging(config->log, config->log_user_data);
  InitializeAllocatorForThisThread();

  // Library defaults first, so the embedder's options override them.
  ParsedOptions Parsed {
    {FEXCore::Config::CONFIG_DISABLETELEMETRY, "1"},
    // Always on, whatever the core's built-in default; the option is not one an embedder
    // can set. Without it, a locked operation on a word crossing 16 bytes is emulated by
    // two compare-and-swaps that tear under contention and lose the update. The fault
    // handler then takes the core's process-wide split-lock spin lock around each such
    // emulation; why that is safe is in docs/macOS-embedding-interface.md (fault handler
    // locks).
    {FEXCore::Config::CONFIG_STRICTINPROCESSSPLITLOCKS, "1"},
  };
  if (const auto Result = ParseOptions(*config, &Parsed); Result != FEXMAC_OK) {
    return Fail(Result, false);
  }

  // The code cache: the core reads these when the context is created. Its own options
  // for them are refused in ParseOptions; the config fields are the interface.
  if (config->code_cache_enabled) {
    // Before the embedder's options, so DISKCACHEMAXFILESIZE overrides it. 256MB a file:
    // every process reads the whole index at start (fexmac.h, code_cache_enabled).
    Parsed.insert(Parsed.begin(), {FEXCore::Config::CONFIG_DISKCACHEMAXFILESIZE, "268435456"});
    Parsed.emplace_back(FEXCore::Config::CONFIG_DISKCACHE, "1");
    if (config->code_cache_directory) {
      Parsed.emplace_back(FEXCore::Config::CONFIG_DISKCACHEPATH, config->code_cache_directory);
    }
  }
  FEXCore::Config::Initialize();
  FEXCore::Config::AddLayer(CreateOptionsLayer(std::move(Parsed)));
  FEXCore::Config::Load();
  FEXCore::Config::ReloadMetaLayer();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");

  auto* Context = new (std::nothrow) fexmac_context {};
  if (!Context) {
    return Fail(FEXMAC_ERROR_OUT_OF_MEMORY, true);
  }
  if (!Context->Memory.Initialize()) {
    delete Context;
    return Fail(FEXMAC_ERROR_OUT_OF_MEMORY, true);
  }
  // Self-modifying-code policy. Without the option the core keeps its own default,
  // mtrack: with no memory registered that detects nothing, as on macos/v1 before
  // package D (the embedder calls fexmac_invalidate); once memory is registered,
  // translated pages of it are write-protected (mtrack). SMCCHECKS full or none
  // explicitly chooses the core's per-instruction check or nothing.
  Context->WriteProtectCode = FEXCore::Config::Get_SMCCHECKS() == FEXCore::Config::CONFIG_SMC_MTRACK;
  Context->SyscallFn = config->syscall;
  Context->SyscallUserData = config->syscall_user_data;
  Context->CacheRequested = config->code_cache_enabled != 0;

  {
    auto HostFeatures = FEX::FetchHostFeatures(true);
    Context->HostFeatures = HostFeatures;
    Context->CTX = FEXCore::Context::Context::CreateNewContext(HostFeatures);
  }
  Context->Delegator = fextl::make_unique<fexmac::SignalDelegator>();
  Context->Syscalls = fextl::make_unique<fexmac::SyscallHandler>(Context);
  Context->CTX->SetSignalDelegator(Context->Delegator.get());
  Context->CTX->SetSyscallHandler(Context->Syscalls.get());
  if (!Context->CTX->InitCore()) {
    delete Context;
    return Fail(FEXMAC_ERROR_INIT_FAILED, true);
  }

  Context->Descriptors = new (std::nothrow) FEXCore::Core::CPUState::gdt_segment[fexmac_context::DescriptorCount] {};
  if (!Context->Descriptors) {
    delete Context;
    return Fail(FEXMAC_ERROR_OUT_OF_MEMORY, true);
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
  ProcessContext.store(Context);
  *out_context = Context;
  return FEXMAC_OK;
}

fexmac_result fexmac_context_destroy(fexmac_context* context) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  {
    std::lock_guard Lock {RegistryMutex};
    if (!context || !LiveContexts.contains(context)) {
      return FEXMAC_ERROR_INVALID_HANDLE;
    }
    if (context->ActiveCalls != 0) {
      return FEXMAC_ERROR_BUSY;
    }
    std::lock_guard ThreadsLock {context->ThreadsMutex};
    if (!context->Threads.empty()) {
      return FEXMAC_ERROR_BUSY;
    }
    // From here no call can find the context.
    LiveContexts.erase(context);
  }
  ProcessContext.store(nullptr);

  for (auto* Dead : context->DeadThreads) {
    delete Dead;
  }
  context->DeadThreads.clear();
  context->CTX.reset();
  context->Syscalls.reset();
  context->Delegator.reset();
  delete[] context->Descriptors;
  delete context;
  FEXCore::Config::Shutdown();
  RemoveLogging();

  std::lock_guard Lock {RegistryMutex};
  ContextExists = false;
  return FEXMAC_OK;
}

fexmac_result fexmac_cpuid(fexmac_context* context, uint32_t function, uint32_t leaf, uint32_t out[4]) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  if (!out) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  ContextUse Use {context};
  if (!Use) {
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
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  ContextUse Use {context};
  if (!Use) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  if (size == 0) {
    return FEXMAC_OK;
  }
  auto* Current = GetCurrentThread();
  auto* Calling = Current && Current->Context == context ? Current->Thread : nullptr;
  Invalidate(context, Calling, address, size);
  return FEXMAC_OK;
}

void fexmac_translation_counters_init(fexmac_translation_counters* counters) {
  if (counters) {
    memset(counters, 0, sizeof(*counters));
    counters->struct_size = sizeof(*counters);
  }
}

fexmac_result fexmac_context_get_counters(fexmac_context* context, fexmac_translation_counters* counters) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  if (!counters || !KnownSize(counters)) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  ContextUse Use {context};
  if (!Use) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  const auto Counters = context->CTX->GetTranslationCounters();
  counters->blocks_compiled = Counters.BlocksCompiled;
  counters->blocks_loaded = Counters.BlocksLoadedFromCache;
  counters->cache_entries_rejected = Counters.CacheEntriesRejected;
  counters->cache_entries_queued = Counters.CacheEntriesQueued;
  counters->cache_entries_dropped = Counters.CacheEntriesDropped;
  counters->untrusted_files_replaced = Counters.UntrustedFilesReplaced;
  counters->damaged_databases_reset = Counters.DamagedDatabasesReset;
  // The core's states, in fexmac_cache_state's order.
  counters->cache_state = context->CacheRequested ? static_cast<fexmac_cache_state>(Counters.CacheState) : FEXMAC_CACHE_OFF;
  if (counters->cache_state > FEXMAC_CACHE_UNAVAILABLE) {
    counters->cache_state = FEXMAC_CACHE_UNAVAILABLE;
  }
  return FEXMAC_OK;
}

fexmac_result fexmac_context_flush_cache(fexmac_context* context) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  ContextUse Use {context};
  if (!Use) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  if (!context->CTX->GetTranslationCounters().CacheWriting) {
    return FEXMAC_ERROR_UNSUPPORTED;
  }
  return context->CTX->FlushDiskCache() ? FEXMAC_OK : FEXMAC_ERROR_INTERNAL;
}

namespace {
  // The context whose locks fexmac_before_fork holds, until fexmac_after_fork. Checked
  // before taking the registry lock: the caller of a second fexmac_before_fork may be the
  // thread that holds it.
  std::atomic<fexmac_context*> ForkingContext {};
} // namespace

fexmac_result fexmac_before_fork(fexmac_context* context) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  if (ForkingContext.load()) {
    return FEXMAC_ERROR_BUSY;
  }
  RegistryMutex.lock();
  if (!context || !LiveContexts.contains(context)) {
    RegistryMutex.unlock();
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  for (auto* Thread : context->Threads) {
    if (Thread->Running) {
      RegistryMutex.unlock();
      return FEXMAC_ERROR_BUSY;
    }
  }
  // Every lock the library and the core might hold on another host thread, so the child
  // does not inherit one held by a thread it does not have. The registry lock stays held
  // until fexmac_after_fork.
  ForkingContext = context;
  context->ThreadsMutex.lock();
  context->Memory.Mutex.lock();
  context->CTX->LockBeforeFork(nullptr);
  return FEXMAC_OK;
}

fexmac_result fexmac_after_fork(fexmac_context* context, int child) {
  if (!context || context != ForkingContext.load()) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  context->CTX->UnlockAfterFork(nullptr, child != 0);
  context->Memory.Mutex.unlock();
  context->ThreadsMutex.unlock();
  ForkingContext = nullptr;
  RegistryMutex.unlock();
  return FEXMAC_OK;
}

fexmac_result fexmac_thread_create(fexmac_context* context, fexmac_thread** out_thread) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  if (!out_thread) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  *out_thread = nullptr;
  ContextUse Use {context};
  if (!Use) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  InitializeAllocatorForThisThread();

  auto* Thread = new (std::nothrow) fexmac_thread {};
  if (!Thread) {
    return FEXMAC_ERROR_OUT_OF_MEMORY;
  }
  Thread->Context = context;

  {
    std::lock_guard Lock {context->ThreadsMutex};
    // The context's first code buffer is made with its first thread; making it here
    // first turns a failure into an error instead of a stop inside the core.
    if (!context->CTX->PrepareCodeBuffer()) {
      delete Thread;
      return FEXMAC_ERROR_OUT_OF_MEMORY;
    }
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
  }
  {
    std::lock_guard RegistryLock {RegistryMutex};
    LiveThreads.insert(Thread);
  }
  *out_thread = Thread;
  return FEXMAC_OK;
}

fexmac_result fexmac_thread_destroy(fexmac_thread* thread) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  {
    std::lock_guard Lock {RegistryMutex};
    if (!thread || !LiveThreads.contains(thread)) {
      return FEXMAC_ERROR_INVALID_HANDLE;
    }
    if (thread->Running) {
      return FEXMAC_ERROR_BUSY;
    }
    // From here no call can find the thread.
    LiveThreads.erase(thread);
  }

  // A stop request in progress on another thread (or in a signal handler) still uses the
  // core's thread state: wait for it, and make later ones find the handle dead.
  thread->Dead.store(1, std::memory_order_seq_cst);
  while (thread->StopCallers.load(std::memory_order_seq_cst) != 0) {
    sched_yield();
  }

  // Still listed in its context, so the context cannot be destroyed until this is done.
  auto* Context = thread->Context;
  {
    std::lock_guard Lock {Context->ThreadsMutex};
    auto& Threads = Context->Threads;
    Threads.erase(std::remove(Threads.begin(), Threads.end(), thread), Threads.end());
    Context->CTX->DestroyThread(thread->Thread);
    thread->Thread = nullptr;
    // The handle stays allocated until the context is destroyed (fexmac_thread::Dead), so
    // that fexmac_thread_request_stop on a destroyed handle is refused instead of touching
    // freed memory. The cost is a small allocation per destroyed thread held for the life
    // of the context (documented in fexmac.h): an embedder that creates and destroys
    // threads for ever in one context grows by that much each time.
    Context->DeadThreads.push_back(thread);
  }
  if (thread->CallRetAllocation) {
    ::munmap(thread->CallRetAllocation, thread->CallRetAllocationSize);
    thread->CallRetAllocation = nullptr;
  }
  return FEXMAC_OK;
}

fexmac_context* fexmac_thread_context(fexmac_thread* thread) {
  if (InLogCallback) {
    return nullptr;
  }
  std::lock_guard Lock {RegistryMutex};
  return thread && LiveThreads.contains(thread) ? thread->Context : nullptr;
}

void fexmac_thread_set_user_data(fexmac_thread* thread, void* user_data) {
  if (InLogCallback) {
    return;
  }
  std::lock_guard Lock {RegistryMutex};
  if (thread && LiveThreads.contains(thread)) {
    thread->UserData = user_data;
  }
}

void* fexmac_thread_get_user_data(fexmac_thread* thread) {
  if (InLogCallback) {
    return nullptr;
  }
  std::lock_guard Lock {RegistryMutex};
  return thread && LiveThreads.contains(thread) ? thread->UserData : nullptr;
}

fexmac_result fexmac_thread_get_state(fexmac_thread* thread, fexmac_x86_state* state) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  if (!state) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  if (!KnownSize(state)) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  std::lock_guard Lock {RegistryMutex};
  if (!thread || !LiveThreads.contains(thread)) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  if (!MayAccessState(thread)) {
    return FEXMAC_ERROR_BUSY;
  }
  ReadState(thread, state);
  return FEXMAC_OK;
}

fexmac_result fexmac_thread_set_state(fexmac_thread* thread, const fexmac_x86_state* state) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  if (!state) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  if (!KnownSize(state)) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  std::lock_guard Lock {RegistryMutex};
  if (!thread || !LiveThreads.contains(thread)) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  if (!MayAccessState(thread)) {
    return FEXMAC_ERROR_BUSY;
  }
  WriteState(thread, state);
  return FEXMAC_OK;
}

fexmac_result fexmac_thread_run(fexmac_thread* thread, fexmac_stop* stop) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  if (stop && !KnownSize(stop)) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  {
    std::lock_guard Lock {RegistryMutex};
    if (!thread || !LiveThreads.contains(thread)) {
      return FEXMAC_ERROR_INVALID_HANDLE;
    }
    // Running any guest thread from inside a system call callback would nest
    // dispatchers on one host stack.
    if (thread->Running || InCallbackOnThisHostThread()) {
      return FEXMAC_ERROR_BUSY;
    }
    thread->Running = true;
  }
  InitializeAllocatorForThisThread();

  // Faults are only claimed for the thread this host thread is running.
  auto* Previous = GetCurrentThread();
  SetCurrentThread(thread);
  thread->PendingStop = {};

  if (thread->StopRequested.exchange(0)) {
    // A stop requested while the thread was not running (or one that arrived as the last
    // run ended): answer it before running anything.
    ProtectInterruptPage(thread, false);
    thread->PendingStop = fexmac_stop {.struct_size = sizeof(fexmac_stop),
                                       .reason = FEXMAC_STOP_REQUESTED,
                                       .instruction_address = thread->Thread->CurrentFrame->State.rip};
  } else {
    thread->Context->CTX->ExecuteThread(thread->Thread);
    // The run can end (a guest fault, a stop) before a store to write-protected code
    // that was being single-stepped reached the next block.
    FinishCodeWrite(thread);
  }

  SetCurrentThread(Previous);
  const fexmac_stop Result = thread->PendingStop;
  {
    std::lock_guard Lock {RegistryMutex};
    thread->Running = false;
  }

  if (stop) {
    const uint32_t Size = stop->struct_size;
    *stop = Result;
    stop->struct_size = Size;
  }
  return Result.reason == FEXMAC_STOP_NONE ? FEXMAC_ERROR_INTERNAL : FEXMAC_OK;
}

fexmac_result fexmac_thread_request_stop(fexmac_thread* thread) {
  // Async-signal-safe: no lock, no allocation. The store comes first so that the fault
  // the read-only page causes always finds the request.
  if (!thread) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  thread->StopCallers.fetch_add(1, std::memory_order_seq_cst);
  if (thread->Dead.load(std::memory_order_seq_cst)) {
    thread->StopCallers.fetch_sub(1, std::memory_order_seq_cst);
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  thread->StopRequested.store(1, std::memory_order_seq_cst);
  ProtectInterruptPage(thread, true);
  thread->StopCallers.fetch_sub(1, std::memory_order_seq_cst);
  return FEXMAC_OK;
}

fexmac_result fexmac_memory_map(fexmac_context* context, uint64_t address, uint64_t size, uint32_t prot) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  ContextUse Use {context};
  if (!Use) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  const bool AllowUnenforced = prot & FEXMAC_PROT_ALLOW_UNENFORCED;
  prot &= ~FEXMAC_PROT_ALLOW_UNENFORCED;
  if (prot & ~GuestMemory::ProtMask) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  return SetGuestMemory(context, address, size, static_cast<uint8_t>(prot | GuestMemory::Mapped), false, AllowUnenforced);
}

fexmac_result fexmac_memory_protect(fexmac_context* context, uint64_t address, uint64_t size, uint32_t prot) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  ContextUse Use {context};
  if (!Use) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  const bool AllowUnenforced = prot & FEXMAC_PROT_ALLOW_UNENFORCED;
  prot &= ~FEXMAC_PROT_ALLOW_UNENFORCED;
  if (prot & ~GuestMemory::ProtMask) {
    return FEXMAC_ERROR_INVALID_ARGUMENT;
  }
  return SetGuestMemory(context, address, size, static_cast<uint8_t>(prot | GuestMemory::Mapped), true, AllowUnenforced);
}

fexmac_result fexmac_memory_unmap(fexmac_context* context, uint64_t address, uint64_t size) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  ContextUse Use {context};
  if (!Use) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  // Unmapping part of a host page is always done (the memory is gone for the guest), but
  // if the rest of the host page stays mapped and accessible the guest can still reach
  // the unmapped part: counted in fexmac_memory_unenforced_count and reported as
  // FEXMAC_DONE_UNENFORCED, so that an embedder cannot miss it.
  bool Unenforced = false;
  const fexmac_result Result = SetGuestMemory(context, address, size, 0, false, true, &Unenforced);
  return Result == FEXMAC_OK && Unenforced ? FEXMAC_DONE_UNENFORCED : Result;
}

fexmac_result fexmac_memory_query(fexmac_context* context, uint64_t address, uint32_t* prot, int* mapped) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  ContextUse Use {context};
  if (!Use) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  const uint8_t Info = context->Memory.Page(address);
  if (prot) {
    *prot = Info & GuestMemory::ProtMask;
  }
  if (mapped) {
    *mapped = (Info & GuestMemory::Mapped) != 0;
  }
  return FEXMAC_OK;
}

fexmac_result fexmac_memory_write_begin(fexmac_context* context, uint64_t address, uint64_t size) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  ContextUse Use {context};
  if (!Use) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  return WriteBegin(context, address, size, true);
}

fexmac_result fexmac_memory_write_begin_ex(fexmac_context* context, uint64_t address, uint64_t size, uint32_t flags, uint64_t* first_unwritable) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  ContextUse Use {context};
  if (!Use) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  return WriteBegin(context, address, size, true, flags, first_unwritable);
}

fexmac_result fexmac_memory_write_end(fexmac_context* context, uint64_t address, uint64_t size) {
  if (InLogCallback) {
    return FEXMAC_ERROR_BUSY;
  }
  ContextUse Use {context};
  if (!Use) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  return WriteBegin(context, address, size, false);
}

uint64_t fexmac_memory_unenforced_count(fexmac_context* context) {
  if (InLogCallback) {
    return 0;
  }
  ContextUse Use {context};
  if (!Use) {
    return 0;
  }
  return context->Memory.UnenforcedChanges.load(std::memory_order_relaxed);
}

uint64_t fexmac_memory_code_write_count(fexmac_context* context) {
  if (InLogCallback) {
    return 0;
  }
  ContextUse Use {context};
  if (!Use) {
    return 0;
  }
  return context->Memory.CodeWriteFaults.load(std::memory_order_relaxed);
}

#ifdef FEXMAC_TESTING
void fexmac_testing_guard_pages(fexmac_thread* thread, uint64_t* callret_low, uint64_t* callret_high, uint64_t* scratch) {
  std::lock_guard Lock {RegistryMutex};
  if (!thread || !LiveThreads.contains(thread)) {
    return;
  }
  constexpr uint64_t HostPage = FEXCore::Utils::FEX_HOST_PAGE_SIZE;
  const auto Base = reinterpret_cast<uint64_t>(thread->CallRetAllocation);
  *callret_low = Base;
  *callret_high = Base + thread->CallRetAllocationSize - HostPage;
  // The core arms JITGuardPage with the exact first address of the guard.
  *scratch = thread->Thread->JITGuardPage;
}

uint64_t fexmac_testing_scratch_overflows(fexmac_thread* thread) {
  return thread ? thread->Thread->JITScratchOverflows : 0;
}

void fexmac_testing_set_scratch_limit(fexmac_thread* thread, uint64_t limit) {
  if (thread) {
    thread->Thread->JITScratchBufferLimit = limit;
  }
}

void fexmac_testing_set_scratch_start(fexmac_thread* thread, uint32_t bytes_per_node) {
  if (thread) {
    thread->Thread->JITScratchStartMultiplier = bytes_per_node;
  }
}

fexmac_result fexmac_testing_fail_code_buffers(fexmac_context* context, int count) {
  ContextUse Use {context};
  if (!Use) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  context->CTX->FailCodeBufferAllocationsForTesting(count);
  return FEXMAC_OK;
}

void fexmac_testing_code_buffer_use(fexmac_thread* thread, uint64_t* used, uint64_t* usable) {
  thread->Thread->CTX->GetCodeBufferUse(thread->Thread, used, usable);
}

void fexmac_testing_set_code_buffer_used(fexmac_thread* thread, uint64_t used) {
  thread->Thread->CTX->SetCodeBufferUsedForTesting(thread->Thread, used);
}

uint64_t fexmac_testing_code_buffer_guard(fexmac_thread* thread) {
  return thread ? thread->Thread->CTX->GetCodeBufferGuard(thread->Thread) : 0;
}

void fexmac_testing_overrun_code_buffer(fexmac_thread* thread) {
  auto* End = reinterpret_cast<uint8_t*>(fexmac_testing_code_buffer_guard(thread));
  if (!End) {
    return;
  }
  static const uint8_t Bytes[64] {};
  pthread_jit_write_protect_np(0);
  // volatile: the store must happen, byte by byte, as an overrunning copy would.
  volatile uint8_t* Destination = End - 32;
  for (size_t i = 0; i < sizeof(Bytes); ++i) {
    Destination[i] = Bytes[i];
  }
  pthread_jit_write_protect_np(1);
}

void fexmac_testing_set_invalidation_hook(void (*hook)(void)) {
  TestInvalidationHook.store(hook);
}

// Code generation state outside the configuration, for the code cache tests.
fexmac_result fexmac_testing_add_thunk_trampoline(fexmac_context* context, uint64_t entry, uint64_t target) {
  ContextUse Use {context};
  if (!Use) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  context->CTX->AddThunkTrampolineIRHandler(entry, target);
  return FEXMAC_OK;
}

fexmac_result fexmac_testing_rcpc_features(fexmac_context* context, int* rcpc, int* rcpc2, int* rcpc3) {
  ContextUse Use {context};
  if (!Use || !rcpc || !rcpc2 || !rcpc3) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  *rcpc = context->HostFeatures.SupportsRCPC;
  *rcpc2 = context->HostFeatures.SupportsTSOImm9;
  *rcpc3 = context->HostFeatures.SupportsLRCPC3;
  return FEXMAC_OK;
}

fexmac_result fexmac_testing_set_hardware_tso(fexmac_context* context, int supported) {
  ContextUse Use {context};
  if (!Use) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  context->CTX->SetHardwareTSOSupport(supported != 0);
  return FEXMAC_OK;
}

fexmac_result fexmac_testing_force_tso(fexmac_context* context, uint64_t begin, uint64_t end, uint64_t instruction) {
  ContextUse Use {context};
  if (!Use) {
    return FEXMAC_ERROR_INVALID_HANDLE;
  }
  std::unique_lock Lock {context->CTX->GetCodeInvalidationMutex()};
  FEXCore::IntervalList<uint64_t> Ranges;
  Ranges.Insert({begin, end});
  fextl::set<uint64_t> Instructions {instruction};
  context->CTX->AddForceTSOInformation(Ranges, std::move(Instructions));
  return FEXMAC_OK;
}

void fexmac_testing_set_mark_executable_hook(void (*hook)(uint64_t start, uint64_t length, void* user_data), void* user_data) {
  TestMarkHookData.store(user_data);
  TestMarkHook.store(hook);
}
#endif

} // extern "C"
