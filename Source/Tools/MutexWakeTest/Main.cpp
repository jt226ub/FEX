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
// The scenario, made deterministic by letting each waiter fall asleep before the next
// step: a writer holds the lock, a reader waits, a second writer waits, and the first
// writer unlocks. Both waiters must get the lock within a few seconds. Repeated with the
// waiters in the other order, and for the read-to-write hand-off.

#include <FEXCore/Utils/WritePriorityMutex.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <unistd.h>

namespace {
using FEXCore::Utils::WritePriorityMutex::Mutex;
using namespace std::chrono_literals;

// Long enough for a thread that found the lock taken to be asleep in the kernel.
constexpr auto Settle = 100ms;

bool WaitFor(const std::atomic<int>& Done, int Expected) {
  const auto Deadline = std::chrono::steady_clock::now() + 5s;
  while (Done.load() < Expected) {
    if (std::chrono::steady_clock::now() > Deadline) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

// The first owner holds the lock (exclusively or shared); two waiters of the given kinds
// queue in order; the owner releases. Returns whether both waiters got the lock.
bool Scenario(const char* Name, bool OwnerShared, bool FirstShared, bool SecondShared) {
  // Leaked on failure: a thread may still be asleep on it.
  auto* M = new Mutex;
  std::atomic<int> Done {};
  auto Waiter = [M, &Done](bool Shared) {
    if (Shared) {
      M->lock_shared();
      M->unlock_shared();
    } else {
      M->lock();
      M->unlock();
    }
    Done.fetch_add(1);
  };

  if (OwnerShared) {
    M->lock_shared();
  } else {
    M->lock();
  }
  std::thread First(Waiter, FirstShared);
  std::this_thread::sleep_for(Settle);
  std::thread Second(Waiter, SecondShared);
  std::this_thread::sleep_for(Settle);
  if (OwnerShared) {
    M->unlock_shared();
  } else {
    M->unlock();
  }

  const bool Passed = WaitFor(Done, 2);
  printf("%-44s %s\n", Name, Passed ? "ok" : "HUNG (a waiter was never woken)");
  if (Passed) {
    First.join();
    Second.join();
    delete M;
  } else {
    First.detach();
    Second.detach();
  }
  return Passed;
}
} // namespace

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  int Failures = 0;
  Failures += !Scenario("writer owns; reader then writer wait", false, true, false);
  Failures += !Scenario("writer owns; writer then reader wait", false, false, true);
  Failures += !Scenario("writer owns; writer then writer wait", false, false, false);
  // A reader cannot queue behind a shared owner unless a writer is already waiting.
  Failures += !Scenario("reader owns; writer then reader wait", true, false, true);
  printf("%s: %d scenario(s) hung\n", Failures ? "FAIL" : "PASS", Failures);
  fflush(stdout);
  _exit(Failures ? 1 : 0);
}
