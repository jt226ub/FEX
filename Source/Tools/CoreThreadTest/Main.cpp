// SPDX-License-Identifier: MIT
//
// Runs the same guest loop on several host threads at once, each with its own guest
// registers, for long enough that the system switches between them many times.
//
// It checks two things the single-thread smoke test cannot:
//  - Threads sharing one translator context do not disturb each other: each thread sums
//    a different count and must get its own answer.
//  - Guest state survives being switched out. macOS may clear register x18 when it
//    switches threads, so translated code must never keep anything there.
//
// Each thread stops at a `hlt`. Uses only the fexmac interface (fexmac.h); each guest
// thread is created on the host thread that runs it.

#include <fexmac.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
// clang-format off
constexpr uint8_t GuestProgram[] = {
  // loop:
  0x48, 0x01, 0xC8, // add rax, rcx
  0x48, 0xFF, 0xC9, // dec rcx
  0x75, 0xF8,       // jnz loop
  0xF4,             // hlt
};
// clang-format on

struct Worker {
  uint64_t Count {};
  uint64_t Result {};
  uint64_t FinalCounter {};
  bool Started {};
  bool Halted {};
};

void Run(fexmac_context* Context, uint64_t Code, Worker* Self) {
  fexmac_thread* Thread {};
  if (fexmac_thread_create(Context, &Thread) != FEXMAC_OK) {
    return;
  }

  fexmac_x86_state State;
  fexmac_thread_get_state(Thread, &State);
  State.rip = Code;
  State.gpr[0] = 0;           // rax
  State.gpr[1] = Self->Count; // rcx
  fexmac_thread_set_state(Thread, &State);
  Self->Started = true;

  fexmac_stop Stop {};
  if (fexmac_thread_run(Thread, &Stop) == FEXMAC_OK) {
    Self->Halted = Stop.reason == FEXMAC_STOP_HALT;
  }
  fexmac_thread_get_state(Thread, &State);
  Self->Result = State.gpr[0];
  Self->FinalCounter = State.gpr[1];
  fexmac_thread_destroy(Thread);
}
} // namespace

int main(int argc, char** argv) {
  const unsigned ThreadCount = argc > 1 ? static_cast<unsigned>(atoi(argv[1])) : 8;
  const uint64_t BaseCount = argc > 2 ? strtoull(argv[2], nullptr, 0) : 600'000'000ULL;

  auto* Code = static_cast<uint8_t*>(::mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (Code == MAP_FAILED) {
    fprintf(stderr, "FAIL: could not allocate guest memory\n");
    return 1;
  }
  memcpy(Code, GuestProgram, sizeof(GuestProgram));

  fexmac_config Config;
  fexmac_config_init(&Config);
  fexmac_context* Context {};
  if (auto Result = fexmac_context_create(&Config, &Context); Result != FEXMAC_OK) {
    fprintf(stderr, "FAIL: could not create the context: %s\n", fexmac_result_string(Result));
    return 1;
  }

  std::vector<Worker> Workers(ThreadCount);
  std::vector<std::thread> Threads;
  for (unsigned i = 0; i < ThreadCount; ++i) {
    Workers[i].Count = BaseCount + i * 1'000'003ULL;
    Threads.emplace_back(Run, Context, reinterpret_cast<uint64_t>(Code), &Workers[i]);
  }
  for (auto& Thread : Threads) {
    Thread.join();
  }

  int Failures = 0;
  for (unsigned i = 0; i < ThreadCount; ++i) {
    const uint64_t N = Workers[i].Count;
    const uint64_t Expected = (N % 2 == 0) ? (N / 2) * (N + 1) : N * ((N + 1) / 2);
    const bool Passed = Workers[i].Started && Workers[i].Halted && Workers[i].Result == Expected && Workers[i].FinalCounter == 0;
    printf("thread %u: summed 1 to %llu, got %llu, expected %llu: %s\n", i, static_cast<unsigned long long>(N),
           static_cast<unsigned long long>(Workers[i].Result), static_cast<unsigned long long>(Expected), Passed ? "ok" : "WRONG");
    Failures += Passed ? 0 : 1;
  }

  // Every thread was destroyed by its own host thread, so the context can go.
  const fexmac_result Destroyed = fexmac_context_destroy(Context);
  if (Destroyed != FEXMAC_OK) {
    printf("context destroy: %s\n", fexmac_result_string(Destroyed));
    ++Failures;
  }
  printf("%s: %u threads, %d wrong\n", Failures ? "FAIL" : "PASS", ThreadCount, Failures);
  fflush(stdout);
  return Failures ? 1 : 0;
}
