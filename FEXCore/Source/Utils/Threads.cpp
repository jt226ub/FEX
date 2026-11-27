// SPDX-License-Identifier: MIT
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/Threads.h>
#include <FEXCore/fextl/memory.h>

#include <pthread.h>
#include <unistd.h>

#ifdef __APPLE__
#include <FEXCore/Utils/AllocatorHooks.h>
#include <FEXCore/fextl/string.h>
#include <pthread/qos.h>
#include <signal.h>
#endif

namespace FEXCore::Threads {
#ifdef __APPLE__
// macOS: the core's own helper threads (today only the disk cache writer) need a thread
// implementation. On Linux the process emulation layer installs one; the macOS port has
// no such layer, so an embedder that never calls SetInternalPointers gets this plain
// pthread one instead of a fatal error. Helper threads never run guest code, so all
// signals are blocked in them: a host fault handler installed by the embedder must never
// run on one of these threads.
namespace {
  class PThread final : public Thread {
  public:
    PThread(ThreadFunc Func, void* Arg, Flags ThreadFlags, const char* ThreadName)
      : UserFunc {Func}
      , UserArg {Arg}
      , ThreadFlags {ThreadFlags}
      , Name {ThreadName ? ThreadName : ""} {
      sigset_t All, Old;
      sigfillset(&All);
      // The new thread inherits the creating thread's signal mask.
      pthread_sigmask(SIG_SETMASK, &All, &Old);
      Created = pthread_create(&Handle, nullptr, &Entry, this) == 0;
      pthread_sigmask(SIG_SETMASK, &Old, nullptr);
      if (!Created) {
        ERROR_AND_DIE_FMT("pthread_create failed for an internal thread");
      }
    }

    ~PThread() override {
      if (Created && !Joined && !Detached) {
        pthread_detach(Handle);
      }
    }

    bool joinable() override {
      return Created && !Joined && !Detached;
    }
    bool join(void** ret) override {
      if (!joinable()) {
        return false;
      }
      Joined = pthread_join(Handle, ret) == 0;
      return Joined;
    }
    bool detach() override {
      if (!joinable()) {
        return false;
      }
      Detached = pthread_detach(Handle) == 0;
      return Detached;
    }
    bool IsSelf() override {
      return Created && pthread_equal(Handle, pthread_self());
    }

  private:
    static void* Entry(void* Ptr) {
      auto* Self = static_cast<PThread*>(Ptr);
      if (!Self->Name.empty()) {
        pthread_setname_np(Self->Name.c_str());
      }
      if (Self->ThreadFlags.LowPriority) {
        // The macOS way to ask for low priority: a quality-of-service class.
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
      }
      FEXCore::Allocator::InitializeThread();
      return Self->UserFunc(Self->UserArg);
    }

    ThreadFunc UserFunc;
    void* UserArg;
    Flags ThreadFlags;
    fextl::string Name;
    pthread_t Handle {};
    bool Created {};
    bool Joined {};
    bool Detached {};
  };
} // namespace

static fextl::unique_ptr<FEXCore::Threads::Thread>
CreateThread_Default(ThreadFunc Func, void* Arg, FEXCore::Threads::Flags Flags, const char* ThreadName) {
  return fextl::make_unique<PThread>(Func, Arg, Flags, ThreadName);
}

static void CleanupAfterFork_Default() {}
#else
static fextl::unique_ptr<FEXCore::Threads::Thread>
CreateThread_Default(ThreadFunc Func, void* Arg, FEXCore::Threads::Flags Flags, const char* ThreadName) {
  ERROR_AND_DIE_FMT("Frontend didn't setup thread creation!");
}

static void CleanupAfterFork_Default() {
  ERROR_AND_DIE_FMT("Frontend didn't setup thread creation!");
}
#endif

static FEXCore::Threads::Pointers Ptrs = {
  .CreateThread = CreateThread_Default,
  .CleanupAfterFork = CleanupAfterFork_Default,
};

fextl::unique_ptr<FEXCore::Threads::Thread>
FEXCore::Threads::Thread::Create(ThreadFunc Func, void* Arg, FEXCore::Threads::Flags Flags, const char* ThreadName) {
  return Ptrs.CreateThread(Func, Arg, Flags, ThreadName);
}

void FEXCore::Threads::Thread::CleanupAfterFork() {
  return Ptrs.CleanupAfterFork();
}

void FEXCore::Threads::Thread::SetInternalPointers(const Pointers& _Ptrs) {
  Ptrs = _Ptrs;
}
} // namespace FEXCore::Threads
