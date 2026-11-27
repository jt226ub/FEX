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

int RunSubtests(int argc, char** argv, const struct Subtest* Subtests, size_t Count, unsigned TimeoutSeconds) {
  PageSize = sysconf(_SC_PAGESIZE);
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
