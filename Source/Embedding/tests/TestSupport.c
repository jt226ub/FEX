/* SPDX-License-Identifier: MIT */
#include "TestSupport.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

int Failures;
long PageSize;
volatile uint32_t* Shared;

void Check(int Condition, const char* Format, ...) {
  va_list Args;
  va_start(Args, Format);
  printf("  %s: ", Condition ? "ok  " : "FAIL");
  vprintf(Format, Args);
  printf("\n");
  va_end(Args);
  fflush(stdout);
  if (!Condition) {
    ++Failures;
  }
}

void CheckResult(fexmac_result Got, fexmac_result Expected, const char* What) {
  Check(Got == Expected, "%s returns %s (got %s)", What, fexmac_result_string(Expected), fexmac_result_string(Got));
}

uint8_t* MapGuestMemory(size_t Size) {
  void* Memory = mmap(NULL, Size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (Memory == MAP_FAILED) {
    fprintf(stderr, "could not map guest memory\n");
    exit(2);
  }
  return (uint8_t*)Memory;
}

fexmac_context* CreateContextWithOptions(const fexmac_option* Options, size_t Count, fexmac_syscall_fn Syscall, void* UserData) {
  fexmac_config Config;
  fexmac_config_init(&Config);
  Config.options = Options;
  Config.option_count = Count;
  Config.syscall = Syscall;
  Config.syscall_user_data = UserData;
  fexmac_context* Context = NULL;
  const fexmac_result Result = fexmac_context_create(&Config, &Context);
  if (Result != FEXMAC_OK) {
    fprintf(stderr, "fexmac_context_create: %s\n", fexmac_result_string(Result));
    exit(2);
  }
  return Context;
}

fexmac_context* CreateContext(fexmac_syscall_fn Syscall, void* UserData) {
  return CreateContextWithOptions(NULL, 0, Syscall, UserData);
}

fexmac_x86_state GetState(fexmac_thread* Thread) {
  fexmac_x86_state State;
  fexmac_x86_state_init(&State);
  const fexmac_result Result = fexmac_thread_get_state(Thread, &State);
  if (Result != FEXMAC_OK) {
    fprintf(stderr, "fexmac_thread_get_state: %s\n", fexmac_result_string(Result));
    exit(2);
  }
  return State;
}

void SetState(fexmac_thread* Thread, const fexmac_x86_state* State) {
  const fexmac_result Result = fexmac_thread_set_state(Thread, State);
  if (Result != FEXMAC_OK) {
    fprintf(stderr, "fexmac_thread_set_state: %s\n", fexmac_result_string(Result));
    exit(2);
  }
}

fexmac_thread* CreateThreadAt(fexmac_context* Context, uint64_t RIP, uint64_t StackPointer) {
  fexmac_thread* Thread = NULL;
  if (fexmac_thread_create(Context, &Thread) != FEXMAC_OK) {
    fprintf(stderr, "fexmac_thread_create failed\n");
    exit(2);
  }
  fexmac_x86_state State = GetState(Thread);
  State.rip = RIP;
  State.gpr[RSP] = StackPointer;
  SetState(Thread, &State);
  return Thread;
}

fexmac_stop Run(fexmac_thread* Thread) {
  fexmac_stop Stop;
  fexmac_stop_init(&Stop);
  const fexmac_result Result = fexmac_thread_run(Thread, &Stop);
  if (Result != FEXMAC_OK) {
    fprintf(stderr, "fexmac_thread_run: %s\n", fexmac_result_string(Result));
    exit(2);
  }
  return Stop;
}

/* Reads a child's standard error until it closes, echoing it indented. */
static char* ReadAll(int Fd) {
  size_t Size = 0, Capacity = 4096;
  char* Text = malloc(Capacity);
  for (;;) {
    if (Size + 1024 + 1 > Capacity) {
      Capacity *= 2;
      Text = realloc(Text, Capacity);
    }
    const ssize_t Got = read(Fd, Text + Size, 1024);
    if (Got <= 0) {
      break;
    }
    Size += (size_t)Got;
  }
  Text[Size] = '\0';
  return Text;
}

/* Every temporary file or directory a test makes lives under one root that the runner
 * creates and removes at its end, also when a subtest crashes or the run is killed by a
 * timeout or an interrupt (the review once filled the disk with leftover caches). */
static char TempRoot[64];
static pid_t TempRootOwner;

static void RemoveTempRoot(void) {
  if (TempRoot[0] && getpid() == TempRootOwner) {
    char Command[160];
    snprintf(Command, sizeof(Command), "chmod -RN %s 2>/dev/null; chmod -R u+rwx %s 2>/dev/null; rm -rf %s", TempRoot, TempRoot, TempRoot);
    (void)!system(Command);
    TempRoot[0] = 0;
  }
}

static void RemoveTempRootOnSignal(int Signal) {
  RemoveTempRoot();
  signal(Signal, SIG_DFL);
  raise(Signal);
}

void MakeTestTempDir(char* Out, size_t Size, const char* Name) {
  if (!TempRoot[0]) {
    strcpy(TempRoot, "/tmp/fexmac-tests-XXXXXX");
    if (!mkdtemp(TempRoot)) {
      fprintf(stderr, "mkdtemp failed\n");
      exit(2);
    }
    TempRootOwner = getpid();
    atexit(RemoveTempRoot);
    signal(SIGINT, RemoveTempRootOnSignal);
    signal(SIGTERM, RemoveTempRootOnSignal);
    signal(SIGALRM, RemoveTempRootOnSignal);
    signal(SIGHUP, RemoveTempRootOnSignal);
  }
  snprintf(Out, Size, "%s/%s-XXXXXX", TempRoot, Name);
  if (!mkdtemp(Out)) {
    fprintf(stderr, "mkdtemp failed\n");
    exit(2);
  }
}

int RunSubtests(int argc, char** argv, const struct Subtest* Subtests, size_t Count, unsigned TimeoutSeconds) {
  PageSize = sysconf(_SC_PAGESIZE);
  /* The root exists before any subtest forks, so the parent owns it and removes it. */
  {
    char Probe[96];
    MakeTestTempDir(Probe, sizeof(Probe), "root");
  }
  Shared = mmap(NULL, (size_t)PageSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  setvbuf(stdout, NULL, _IOLBF, 0);

  if (argc > 1) {
    for (size_t i = 0; i < Count; ++i) {
      if (strcmp(argv[1], Subtests[i].Name) == 0) {
        Subtests[i].Run();
        return Failures ? 1 : 0;
      }
    }
    fprintf(stderr, "unknown subtest %s\n", argv[1]);
    return 2;
  }

  int Failed = 0;
  for (size_t i = 0; i < Count; ++i) {
    const struct Subtest* T = &Subtests[i];
    printf("%s\n", T->Name);
    memset((void*)Shared, 0, (size_t)PageSize);
    int Pipe[2];
    if (pipe(Pipe) != 0) {
      return 2;
    }
    fflush(stdout);
    const pid_t Child = fork();
    if (Child == 0) {
      close(Pipe[0]);
      dup2(Pipe[1], STDERR_FILENO);
      close(Pipe[1]);
      alarm(TimeoutSeconds);
      T->Run();
      fflush(stdout);
      _exit(Failures ? 1 : 0);
    }
    close(Pipe[1]);
    char* Errors = ReadAll(Pipe[0]);
    close(Pipe[0]);
    int Status = 0;
    waitpid(Child, &Status, 0);

    for (char* Line = Errors; *Line;) {
      char* End = strchr(Line, '\n');
      if (End) {
        *End = '\0';
      }
      printf("  stderr: %s\n", Line);
      if (!End) {
        break;
      }
      *End = '\n';
      Line = End + 1;
    }

    int Passed = 1;
    const int Signal = WIFSIGNALED(Status) ? WTERMSIG(Status) : 0;
    if (Signal == SIGALRM) {
      printf("  FAIL: hung, killed after %u seconds\n", TimeoutSeconds);
      Passed = 0;
    } else if (T->ExpectSignal == -1) {
      /* SIGILL is what macOS ends a process with when it cannot deliver a signal because
       * the stack is exhausted (a handler that faults recursively). */
      Passed = Signal == SIGSEGV || Signal == SIGBUS || Signal == SIGILL;
      printf("  %s: process died by signal %d (expected SIGSEGV, SIGBUS or SIGILL)\n", Passed ? "ok  " : "FAIL", Signal);
    } else if (T->ExpectSignal) {
      Passed = Signal == T->ExpectSignal;
      printf("  %s: process died by signal %d (expected %d)\n", Passed ? "ok  " : "FAIL", Signal, T->ExpectSignal);
    } else if (!WIFEXITED(Status) || WEXITSTATUS(Status) != 0) {
      printf("  FAIL: exit status %d, signal %d\n", WIFEXITED(Status) ? WEXITSTATUS(Status) : -1, Signal);
      Passed = 0;
    }
    if (T->ExpectStderr) {
      const int Found = strstr(Errors, T->ExpectStderr) != NULL;
      printf("  %s: standard error says \"%s\"\n", Found ? "ok  " : "FAIL", T->ExpectStderr);
      Passed &= Found;
    }
    if (T->ForbidStderr) {
      const int Found = strstr(Errors, T->ForbidStderr) != NULL;
      printf("  %s: standard error does not say \"%s\"\n", Found ? "FAIL" : "ok  ", T->ForbidStderr);
      Passed &= !Found;
    }
    if (T->ExpectShared0) {
      const int Match = Shared[0] == T->ExpectShared0;
      printf("  %s: recorded value %u (expected %u)\n", Match ? "ok  " : "FAIL", Shared[0], T->ExpectShared0);
      Passed &= Match;
    }
    free(Errors);
    Failed += Passed ? 0 : 1;
  }
  printf("%s: %zu subtests, %d failed\n", Failed ? "FAIL" : "PASS", Count, Failed);
  return Failed ? 1 : 0;
}

/* ---- large blocks ---- */

/* Writes `Count` rep movsq with an `add rax, 1` after every `Every`-th one, then hlt.
 * Returns the bytes written; *Expected is rax at the hlt when rax starts at 0. */
size_t WriteBigBlock(uint8_t* Code, size_t Count, size_t Every, uint64_t* Expected) {
  static const uint8_t RepMovsq[] = {0xF3, 0x48, 0xA5};
  static const uint8_t AddRax1[] = {0x48, 0x83, 0xC0, 0x01};
  size_t Offset = 0;
  *Expected = 0;
  for (size_t i = 1; i <= Count; ++i) {
    memcpy(Code + Offset, RepMovsq, sizeof(RepMovsq));
    Offset += sizeof(RepMovsq);
    if (i % Every == 0) {
      memcpy(Code + Offset, AddRax1, sizeof(AddRax1));
      Offset += sizeof(AddRax1);
      ++*Expected;
    }
  }
  Code[Offset++] = 0xF4;
  return Offset;
}

const fexmac_option BigBlockOptions[2] = {{"MAXINST", "100000"}, {"MULTIBLOCK", "1"}};

/* Runs the block at Code from its start with rax = rcx = 0; returns whether it halted
 * at its end with the expected rax. */
int RunBigBlock(fexmac_thread* Thread, uint8_t* Code, size_t Length, uint64_t Expected, uint8_t* Data) {
  fexmac_x86_state State = GetState(Thread);
  State.rip = (uint64_t)(uintptr_t)Code;
  State.gpr[RAX] = 0;
  State.gpr[RCX] = 0;
  State.gpr[RSI] = State.gpr[RDI] = (uint64_t)(uintptr_t)Data;
  SetState(Thread, &State);
  const fexmac_stop Stop = Run(Thread);
  State = GetState(Thread);
  const int Ok = Stop.reason == FEXMAC_STOP_HALT && State.rip == (uint64_t)(uintptr_t)Code + Length - 1 && State.gpr[RAX] == Expected;
  return Ok;
}
