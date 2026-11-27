// SPDX-License-Identifier: MIT
//
// Smallest end-to-end check of the translator core: translate a short x86-64
// program, run it, and confirm the result it hands back.
//
// The guest program sums 1 to 100 in a loop and then makes a system call with the
// sum as its first argument. Reaching the system call with 5050 proves that the
// dispatcher was emitted and runs, that blocks are compiled and copied into
// executable memory, and that guest state reaches the host intact. Multiblock is
// turned off so the loop's backward branch goes through the block linker, which
// patches code that has already been made executable.
//
// Uses only the fexmac interface (fexmac.h). The program never faults, so it installs
// no fault handlers.

#include <fexmac.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace {
constexpr uint64_t ExpectedSum = 5050;

// Offset of the instruction after `syscall` in GuestProgram. The x86-64 `syscall`
// instruction stores the address of the next instruction in rcx.
constexpr uint64_t OffsetAfterSyscall = 22;

// clang-format off
constexpr uint8_t GuestProgram[] = {
  0x31, 0xC0,                   // xor eax, eax
  0xB9, 0x64, 0x00, 0x00, 0x00, // mov ecx, 100
  // loop:
  0x01, 0xC8,                   // add eax, ecx
  0xFF, 0xC9,                   // dec ecx
  0x75, 0xFA,                   // jnz loop
  0x89, 0xC7,                   // mov edi, eax
  0xB8, 0x3C, 0x00, 0x00, 0x00, // mov eax, 60
  0x0F, 0x05,                   // syscall
  0xF4,                         // hlt (never reached)
};
// clang-format on

int Fail(const char* What, fexmac_result Result) {
  fprintf(stderr, "FAIL: %s: %s\n", What, fexmac_result_string(Result));
  return 1;
}
} // namespace

int main() {
  // Guest memory is ordinary host memory: the guest's addresses are the host's.
  constexpr size_t CodeSize = 0x10000;
  constexpr size_t StackSize = 0x10000;
  auto* Code = static_cast<uint8_t*>(::mmap(nullptr, CodeSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  auto* Stack = static_cast<uint8_t*>(::mmap(nullptr, StackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (Code == MAP_FAILED || Stack == MAP_FAILED) {
    fprintf(stderr, "FAIL: could not allocate guest memory\n");
    return 1;
  }
  memcpy(Code, GuestProgram, sizeof(GuestProgram));
  const auto GuestCodeBase = reinterpret_cast<uint64_t>(Code);

  printf("Host page size: %ld\n", sysconf(_SC_PAGESIZE));
  printf("Creating translator context\n");
  fflush(stdout);

  const fexmac_option Options[] = {{"MULTIBLOCK", "0"}};
  fexmac_config Config;
  fexmac_config_init(&Config);
  Config.options = Options;
  Config.option_count = 1;
  // No system call callback: the system call stops the run and is handled below.

  fexmac_context* Context {};
  if (auto Result = fexmac_context_create(&Config, &Context); Result != FEXMAC_OK) {
    return Fail("could not create the context", Result);
  }
  printf("Core initialised, dispatcher emitted\n");
  fflush(stdout);

  fexmac_thread* Thread {};
  if (auto Result = fexmac_thread_create(Context, &Thread); Result != FEXMAC_OK) {
    return Fail("could not create the thread", Result);
  }
  fexmac_x86_state State;
  fexmac_thread_get_state(Thread, &State);
  State.rip = GuestCodeBase;
  State.gpr[4] = reinterpret_cast<uint64_t>(Stack) + StackSize - 64; // rsp
  fexmac_thread_set_state(Thread, &State);

  printf("Running guest code at %p\n", static_cast<void*>(Code));
  fflush(stdout);

  fexmac_stop Stop;
  if (auto Result = fexmac_thread_run(Thread, &Stop); Result != FEXMAC_OK) {
    return Fail("run failed", Result);
  }
  if (Stop.reason != FEXMAC_STOP_SYSCALL) {
    fprintf(stderr, "FAIL: guest stopped (reason %d) without reaching the system call\n", Stop.reason);
    return 1;
  }

  fexmac_thread_get_state(Thread, &State);
  const uint64_t Number = State.gpr[0];        // rax
  const uint64_t Argument = State.gpr[7];      // rdi
  const uint64_t ReturnAddress = State.gpr[1]; // rcx
  const uint64_t ExpectedReturn = GuestCodeBase + OffsetAfterSyscall;

  printf("Guest made system call %llu with argument %llu, return address 0x%llx\n", static_cast<unsigned long long>(Number),
         static_cast<unsigned long long>(Argument), static_cast<unsigned long long>(ReturnAddress));

  const bool Passed = Number == 60 && Argument == ExpectedSum && ReturnAddress == ExpectedReturn && State.rip == ExpectedReturn;
  printf("%s: expected system call 60 with argument %llu, return address 0x%llx\n", Passed ? "PASS" : "FAIL",
         static_cast<unsigned long long>(ExpectedSum), static_cast<unsigned long long>(ExpectedReturn));
  fflush(stdout);

  fexmac_thread_destroy(Thread);
  fexmac_context_destroy(Context);
  return Passed ? 0 : 1;
}
