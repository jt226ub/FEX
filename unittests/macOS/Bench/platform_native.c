/* SPDX-License-Identifier: MIT */
/*
 * The native side of the benchmark (see bench.h): an ordinary macOS program, built for
 * x86-64 (run under Rosetta 2) or arm64, using libc and pthreads. Every thread runs at
 * the user-interactive quality of service, as FEXCoreBench's threads do.
 *
 *   bench-native [--quick] [--reps N] [--only name,name]
 */
#include "bench.h"

#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint64_t bench_time_ns(void) {
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void bench_write(const char* text, size_t len) {
  while (len) {
    const ssize_t n = write(1, text, len);
    if (n <= 0) {
      return;
    }
    text += n;
    len -= (size_t)n;
  }
}

const char* bench_platform(void) {
#if defined(__x86_64__)
  return "native-x86_64";
#else
  return "native-arm64";
#endif
}

typedef struct {
  void (*fn)(void*);
  void* arg;
  pthread_t thread;
  int used;
} slot_t;
static slot_t g_slots[BENCH_MAX_THREADS];

static void* thread_main(void* p) {
  slot_t* s = p;
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  s->fn(s->arg);
  return NULL;
}

int bench_thread_start(void (*fn)(void*), void* arg) {
  for (int i = 0; i < BENCH_MAX_THREADS; ++i) {
    if (!g_slots[i].used) {
      g_slots[i].fn = fn;
      g_slots[i].arg = arg;
      g_slots[i].used = 1;
      if (pthread_create(&g_slots[i].thread, NULL, thread_main, &g_slots[i]) != 0) {
        g_slots[i].used = 0;
        return -1;
      }
      return i;
    }
  }
  return -1;
}

void bench_thread_join(int handle) {
  if (handle >= 0 && handle < BENCH_MAX_THREADS && g_slots[handle].used) {
    pthread_join(g_slots[handle].thread, NULL);
    g_slots[handle].used = 0;
  }
}

int main(int argc, char** argv) {
  int quick = 0, reps = 5;
  const char* only = NULL;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--quick")) {
      quick = 1;
    } else if (!strcmp(argv[i], "--reps") && i + 1 < argc) {
      reps = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--only") && i + 1 < argc) {
      only = argv[++i];
    } else {
      fprintf(stderr, "usage: %s [--quick] [--reps N] [--only name,name]\n", argv[0]);
      return 2;
    }
  }
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  return bench_main(quick, reps, only);
}
