// SPDX-License-Identifier: MIT
//
// Checks that FEXCore's WritePriorityMutex never loses a wake-up.
//
// The mutex keeps readers and writers waiting on the same 32-bit word. Linux wakes only
// writers or only readers by giving each a futex bitset; Windows waits readers on the
// upper half of the word. macOS's os_sync_wait_on_address has neither, so a wake meant for
// one waiting writer can be taken by a waiting reader instead, which sees the writer still
// queued, sleeps again, and leaves the writer asleep for good. FEXCoreOrderingTest found
// this as an occasional hang when several threads translate their first block at once
// (the code invalidation mutex is a WritePriorityMutex).
//
// Each scenario queues waiters in a chosen order and then releases the lock. Before each
// step the test waits until the thread it just started is blocked in the kernel, read from
// the thread's Mach run state, so a slow or loaded machine changes only how long the test
// takes, not what it checks. Every waiter must get the lock within five seconds.
//
// Scenarios that catch the old bug (one unfiltered waiter woken on a writer hand-off):
//  - write-to-write hand-off: a writer owns, a reader queues, a writer queues, the owner
//    unlocks and its single wake reaches the reader.
//  - read-to-write hand-off: a reader owns, a writer queues, a reader queues behind it; a
//    spurious wake (which the kernel may deliver at any time, and which the test sends)
//    puts the writer back in the queue behind the reader; the owner's last unlock_shared
//    then wakes the reader.

#include <FEXCore/Utils/WritePriorityMutex.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mach/mach.h>
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using FEXCore::Utils::WritePriorityMutex::Mutex;
using namespace std::chrono_literals;

// True once the thread is blocked in the kernel. A waiter that found the lock taken has
// nothing else to wait on.
bool IsBlocked(pthread_t Thread) {
  thread_basic_info_data_t Info {};
  mach_msg_type_number_t Count = THREAD_BASIC_INFO_COUNT;
  if (thread_info(pthread_mach_thread_np(Thread), THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&Info), &Count) != KERN_SUCCESS) {
    return false;
  }
  return Info.run_state == TH_STATE_WAITING;
}

// Waits until all the threads have been blocked for several polls in a row.
bool WaitBlocked(const std::vector<pthread_t>& Threads) {
  const auto Deadline = std::chrono::steady_clock::now() + 5s;
  int Stable = 0;
  while (Stable < 5) {
    bool All = true;
    for (auto T : Threads) {
      All &= IsBlocked(T);
    }
    Stable = All ? Stable + 1 : 0;
    if (std::chrono::steady_clock::now() > Deadline) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

struct Waiter {
  std::thread Thread;
  pthread_t Handle {};
};

struct Run {
  Mutex* M = new Mutex; // leaked if a waiter is left asleep on it
  std::atomic<int> Done {};
  std::vector<Waiter*> Waiters;
  bool Inconclusive = false;

  // Starts a thread that takes the lock (shared or not), releases it and counts itself
  // done, then waits until that thread is asleep in the kernel.
  void Queue(bool Shared) {
    auto* W = new Waiter;
    std::atomic<bool> Started {};
    W->Thread = std::thread([this, Shared, &Started] {
      Started = true;
      if (Shared) {
        M->lock_shared();
        M->unlock_shared();
      } else {
        M->lock();
        M->unlock();
      }
      Done.fetch_add(1);
    });
    W->Handle = W->Thread.native_handle();
    while (!Started) {
      std::this_thread::yield();
    }
    Waiters.push_back(W);
    std::vector<pthread_t> All;
    for (auto* X : Waiters) {
      All.push_back(X->Handle);
    }
    Inconclusive |= !WaitBlocked(All);
  }

  bool Finish(const char* Name) {
    const auto Deadline = std::chrono::steady_clock::now() + 5s;
    while (Done.load() < int(Waiters.size()) && std::chrono::steady_clock::now() < Deadline) {
      std::this_thread::sleep_for(1ms);
    }
    const bool Hung = Done.load() < int(Waiters.size());
    printf("%-58s %s\n", Name,
           Inconclusive ? "INCONCLUSIVE (a waiter never blocked)" :
           Hung         ? "HUNG (a waiter was never woken)" :
                          "ok");
    for (auto* W : Waiters) {
      if (Hung) {
        W->Thread.detach();
      } else {
        W->Thread.join();
      }
    }
    const bool Passed = !Hung && !Inconclusive;
    if (!Hung) {
      delete M;
      delete this;
    }
    return Passed;
  }
};

bool WriterOwns(const char* Name, bool FirstShared, bool SecondShared) {
  // Left allocated if a waiter hangs: its thread still refers to it.
  auto& R = *new Run;
  R.M->lock();
  R.Queue(FirstShared);
  R.Queue(SecondShared);
  R.M->unlock();
  return R.Finish(Name);
}

bool ReaderOwnsWriterRequeued(const char* Name) {
  // Left allocated if a waiter hangs: its thread still refers to it.
  auto& R = *new Run;
  R.M->lock_shared();
  R.Queue(false); // the writer waits for the reader to leave
  R.Queue(true);  // a reader may not pass a waiting writer, so it queues behind it
  // A spurious wake for the first waiter in the kernel's queue, the writer. It finds the
  // lock still read-owned and sleeps again, now behind the reader.
  os_sync_wake_by_address_any(R.M, sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
  std::vector<pthread_t> All;
  for (auto* W : R.Waiters) {
    All.push_back(W->Handle);
  }
  R.Inconclusive |= !WaitBlocked(All);
  R.M->unlock_shared();
  return R.Finish(Name);
}

bool ReaderOwns(const char* Name) {
  // Left allocated if a waiter hangs: its thread still refers to it.
  auto& R = *new Run;
  R.M->lock_shared();
  R.Queue(false);
  R.Queue(true);
  R.M->unlock_shared();
  return R.Finish(Name);
}
} // namespace

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  static_assert(sizeof(Mutex) == sizeof(uint32_t), "the test wakes the mutex's word directly");
  int Failures = 0;
  Failures += !WriterOwns("writer owns; reader then writer wait", true, false);
  Failures += !WriterOwns("writer owns; writer then reader wait", false, true);
  Failures += !WriterOwns("writer owns; writer then writer wait", false, false);
  Failures += !WriterOwns("writer owns; reader then reader wait", true, true);
  Failures += !ReaderOwns("reader owns; writer then reader wait");
  Failures += !ReaderOwnsWriterRequeued("reader owns; writer, reader wait; writer requeued");
  printf("%s: %d scenario(s) failed\n", Failures ? "FAIL" : "PASS", Failures);
  fflush(stdout);
  _exit(Failures ? 1 : 0);
}
