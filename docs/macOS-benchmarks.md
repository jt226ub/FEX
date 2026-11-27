# macOS benchmarks: a guest program under this port, Rosetta 2 and native

This is the evidence for exit criteria 7 ("a real guest program runs") and 8 ("speed is
measured") of `docs/macOS-v1.0-plan.md`, and the method behind it.

**The timing numbers below are provisional.** They were taken while other work was
loading the machine, with the load recorded. The final measurement is to be made on an
idle machine with the same script (`--final --write-docs`, see "How to run it"), which
replaces the generated block in this document and labels it FINAL only if the machine
was quiet enough.

## Two modes

| Mode | Host services the guest uses | What it is for |
|---|---|---|
| **literal** (`--literal`) | **exit only**: one system call, carrying the number of wrong checksums and the number of kernels run | Criterion 7 to the letter: the 16 single-threaded kernels run once each, with no output, no clock and no threads; the loader refuses any other system call and requires 0 wrong and all 16 run |
| **timed** (default) | a private ABI: exit, write to standard output, a monotonic clock, start and join a guest thread, end a guest thread | Criterion 8 (timing needs a clock and a way to report) and the two thread kernels |

Neither mode uses any Linux or macOS system call: the ABI is the benchmark's own
(`0xBE00`-`0xBE05`, `platform_guest.c`), answered by the loader's fexmac system call
callback. The literal mode is the one that meets "no system calls beyond exit"; the timed
mode uses the wider private ABI and says so.

## What runs

One benchmark program, written for this port (MIT, `unittests/macOS/Bench/`), built from
the same C source three ways:

| Build | What it is | Run by |
|---|---|---|
| guest | Freestanding x86-64 image: no libc, no OS calls, all memory static | `FEXCoreBench` (`Source/Tools/CoreBench/`) through `fexmac.h` |
| rosetta | x86-64 macOS program (libc, pthreads) | Rosetta 2, `arch -x86_64` |
| native | arm64 macOS program | the M4 directly: the ceiling |

`kernels.c` (the kernels and the driver) is compiled with identical flags for all three,
`-O2 -std=c11 -ffreestanding -ffp-contract=off -fno-stack-protector -fno-stack-check`;
only the target differs (`x86_64-apple-macos11` or `arm64-apple-macos11`). The guest and
Rosetta builds of `kernels.c` are the same object code. The builds differ only in the
small platform file behind `bench.h`: `platform_guest.c` (one `syscall` per service) or
`platform_native.c` (libc).

The image format: `ld64` writes only Mach-O, so the guest is linked as a static,
non-position-independent x86-64 Mach-O at a fixed address (`-static -no_pie
-image_base 0x20000000000`, 2TB: above macOS's 4GB page zero and clear of its reserved
ranges). It has no imports and no relocations; `FEXCoreBench` checks every size and
offset in it without overflow, maps its segments at their link addresses (refusing if
the range is taken: macOS has no `MAP_FIXED_NOREPLACE`, so it asks with a hint and
checks), copies them and starts at the `LC_UNIXTHREAD` entry. No tool beyond the command
line tools is needed.

The loader's contract: only the main guest thread may exit the program, and only a
started thread may end itself; a guest that exits with threads not joined, makes an
unknown call or (literal mode) any call but exit, fails (exit status 101), and the
context is not destroyed under a running guest thread. A guest fault that the library
reports as a stop is printed with the thread it happened on, and the process dies with
the fault's signal. `contract_guest.c` breaks each rule once and the quick check
verifies the outcome.

### Kernels

| Kernel | Stresses | Full size |
|---|---|---|
| `crc32` | table-driven CRC-32: byte loads, table lookups | 24 passes over 1 MiB |
| `hash64` | 64x64->128 multiply, shifts | 192 passes over 1 MiB |
| `sort` | quicksort on 64-bit keys: compares, branches, swaps | 3 x 200,000 keys |
| `interp` | register-machine interpreter: `switch` jump table (indirect jump) and calls through a function-pointer table (indirect call) | 6 million guest instructions |
| `dfa` | tokenizer state machine, data-dependent branches | 40 passes over 512 KiB |
| `rep_movs` | `rep stosb`, `rep stosq`, `rep movsb` (misaligned, odd lengths), `rep movsq` (not 8-aligned), `rep movsl`, an overlapping forward `rep movsb` (replicates), a `rep movsb` with the direction flag set (overlapping, backward) | 200 iterations over 1 MiB |
| `copy_loop` | vectorised copy-and-xor (SSE2 / NEON), length varying so the remainder runs | 600 x 1 MiB |
| `ptr_chase` | dependent loads through an 8 MiB single-cycle list | 4 million steps |
| `matmul_f64` | scalar double matrix multiply (SSE2 scalar), not vectorised | 24 x 128^3 |
| `matmul_f32x4` | 4-wide float matrix multiply with clang vector types (SSE / NEON) | 20 x 192^3 |
| `nbody` | double n-body, `sqrt` and division | 48 bodies, 3,000 steps |
| `fft` | radix-2 complex FFT forward and inverse | 240 x 4096 points |
| `x87` | `long double`: 80-bit x87 on x86-64 (64-bit double on arm64) | 750,000 iterations |
| `muldiv` | Miller-Rabin with Montgomery multiplication (`mul`), 64-bit `div` and `idiv` | 60,000 candidates near 2^62 |
| `calls` | recursive Fibonacci: call and return | fib(34) |
| `deep_recursion` | 50,000-deep non-tail recursion (deeper than the core's call/return stack) | 100 times |
| `threads_lock` | 4 guest threads: spin lock (`xchg`) around shared updates, `lock xadd` | 4 x 400,000 |
| `threads_mp` | 2 producer/consumer pairs through rings; data published with plain stores and release/acquire, which on x86 are plain moves relying on TSO | 2 x 1,000,000 messages |

The random inputs come from splitmix64 (public-domain algorithm, re-implemented); sine and
cosine for the FFT twiddles are our own Taylor series.

### Checksums: bit-exact, and covering the work

Every kernel returns a 64-bit checksum of what it computed. The expected values for the
quick and full sizes are recorded in `kernels.c`; each run checks itself, and the script
also compares the builds with each other.

Floating point is compared **bit-exactly**, not within a tolerance: the kernels use only
operations IEEE 754 requires to be correctly rounded and that SSE2 and NEON both
implement so (+, -, *, /, sqrt, conversions), no library functions, and
`-ffp-contract=off` so that no build fuses a multiply and an add. The exception is `x87`:
`long double` is 64-bit double on arm64, so that checksum is compared between the two
x86-64 builds only, and arm64 has its own recorded value. Under FEX with
`X87REDUCEDPRECISION=1` it gives a different value, which is recorded too and checked.

The checksums cover everything the kernels write: the memory kernels hash every byte of
the destination after each iteration (outside the timed work: the time of these checks
is measured and subtracted), each iteration's writes go to separate regions so one hash
sees all of them, and every run starts from the same destination contents; the FFT and
n-body hash every output, Miller-Rabin every candidate's verdict, the recursion is
combined through a bijection at every level. `Scripts/macos_bench_mutations.py` checks
this: it applies 53 mutations, one at a time, to a copy of `kernels.c` and requires each
to change its kernel's checksum (natively on arm64, and under Rosetta 2 for the three
that exist only in the x86-64 string-instruction code). They include, per kernel:

| Kernel | Mutations (each changes the checksum) |
|---|---|
| crc32 | last byte of a pass skipped; polynomial off by one bit |
| hash64 | last 16-byte block skipped; high half of the 128-bit product dropped |
| sort | last key left out; insertion sort one short |
| interp | xor run as or; indirect call to the wrong function; a branch never taken |
| dfa | numbers counted twice; '9' classified as a letter; identifier length off by one |
| rep_movs | `rep stosb` 1 byte short; `rep movsb` 600 and 1 bytes short; `rep stosq` 1 short; `rep movsq` 40 and 1 short; `rep movsl` 1 short; overlapping copy 1 byte further and 1 short; backward copy 1 short; backward copy run forwards (arm64 loop); `rep stosq` as `rep stosl`, `rep movsl` as `rep movsw`, direction flag not set (x86-64) |
| copy_loop | 64 bytes short; 8 bytes short; one element not xored |
| ptr_chase | last step not taken; one value misread |
| matmul_f64 | last term of each dot product left out; one element wrong |
| matmul_f32x4 | last row of B left out; one lane initialised wrong |
| nbody | one pair interaction left out; wrong mass in one velocity component; one position not integrated at the last step |
| fft | last butterfly stage skipped; imaginary part scaled wrong |
| x87 | a product term changed; a zeta term changed (both arm64 and x86-64) |
| muldiv | candidates shifted by one; wrong divisor; modular inverse one Newton step short |
| calls | base case wrong |
| deep_recursion | one level off by one; one level shallower |
| threads_lock | one lost counter update; one lost sum update; atomic add of the wrong value |
| threads_mp | one message read wrong; one message sent wrong |

The first version of these checksums missed three of them: the copy kernels sampled one
byte in 4,093, `muldiv` hashed only the primes, and `deep_recursion` shifted away all but
the top levels. A mutation that the test could not catch by design is not listed: making
the Montgomery reduction's final comparison `>` instead of `>=` changes nothing here
because the case `r == n` does not occur for these inputs.

## Correctness (criterion 7)

At the branch tip, under FEX (as built and signed for the hardened runtime), Rosetta 2
and natively:

- **Literal mode**: all 16 single-threaded kernels right, with no system call but exit,
  in quick size (as built and hardened) and full size, with all three ordering switches
  on and with them off.
- **Timed mode**: every kernel gives the same checksum as Rosetta 2 and native arm64
  (x87: as Rosetta 2) in every launch of every configuration in the table below, quick
  and full size, with two stated exceptions. With the ordering emulation off
  (`tso_off`) the message-passing kernel reads slots before their data (wrong checksum
  in every full-size launch; it can come out right by chance in the quick size), and the
  spin-lock kernel may lose an update (its unlock is a plain store, which without the
  emulation can move before the critical section's stores); both are allowed any value
  there, but the run must complete with every other kernel right. With
  `X87REDUCEDPRECISION=1` the x87 checksum is the recorded reduced-precision value.
- Quick size also with `MAXINST=1` (ordering on and off), `MAXINST=5000` with
  half-barriers off, `SMCCHECKS=full` and `HOSTFEATURES=disableatomics` (checked before
  the coverage changes, with the earlier checksums).

The string-instruction cases (overlapping forward copies, direction-flag backward
copies, odd lengths and misaligned starts for every element size) all match. No
translator or port bug was found: there is no checksum difference to reduce.

## Speed (criterion 8)

### Method

- Every build and configuration is launched `--launches` times (5), and the launches are
  interleaved: each round runs every build once, starting one later each round, so drift
  in the machine's state reaches all of them alike. Each launch does one warm-up and
  `--reps` (3) timed runs of each kernel; a launch's value is the median of its runs, and
  the report gives the median of the launch medians and their range.
- Every thread runs at the user-interactive quality of service, which keeps it on the
  performance cores when they are free.
- The summary sets are fixed in the script and reported separately:
  - **headline**: the 15 single-threaded integer, memory and floating-point kernels;
    geometric mean of the ratio to Rosetta 2;
  - **x87**: x86-64 builds only (Rosetta 2 and FEX), never against native arm64,
    whose `long double` is a different type;
  - **thread kernels**: reported with their spread, in no mean.
  A kernel enters a configuration's mean only if it was right in every launch of it.
- Load: other processes' CPU use (from `ps`, which macOS reports as a decaying average)
  and the load average are sampled every 2 s and stored in the JSON with every
  individual time. The load average on this Mac includes 4 processes stuck in Rosetta 2
  since a launch earlier in this work (see "Rosetta 2 on this machine"); they use no CPU.
- Translation overhead: time from launch to the first kernel result of a short run
  (`--only calls --quick`), at the first launch of a fresh build and at later launches.
  The core exposes no count of blocks compiled through `fexmac` yet; package F's
  translation counters will supply it.

### Configurations

All FEX configurations use `MAXINST=500 MULTIBLOCK=1` unless stated. Rosetta 2 runs x86
code with the hardware TSO mode of Apple's cores, which macOS does not let an ordinary
process switch on, so the like-for-like comparison is **`tso_on`**, all three of FEX's
ordering switches on: that is the configuration that is correct for any guest.
`tso_off` is **not correct for multi-threaded guests** (this benchmark's own
message-passing kernel fails in it) and is given only as an upper bound for a guest
known not to need ordering. `tso_scalar` (FEX's defaults), `tso_vector` and `tso_memcpy`
add the vector and string-instruction switches one at a time, to show what each costs.

### Results (provisional)

<!-- bench-results:begin -->
<!-- Generated by Scripts/macos_bench.py; replace by running it again, do not edit by hand. -->

**PROVISIONAL** numbers: other work was loading the machine (other processes used 2.94 cores on average, at most 8.8, of 10; load average 10.36 on average). They show the shape, not final values.

Apple M4, 4 performance + 6 efficiency cores, 16 GB, macOS 27.0.1, Apple clang version 21.0.0 (clang-2100.4.1.1); commit `0966fa89f`; 2026-11-27T08:10:19-05:00 to 2026-11-27T08:14:58-05:00. 5 launches per build and configuration, interleaved, each with one warm-up and 3 timed runs per kernel. Cells: median of the launch medians in ms [lowest-highest launch median]. Ratios are to Rosetta 2's median (below 1: faster than Rosetta 2).

FEX configurations (all with `MAXINST=500 MULTIBLOCK=1` unless stated):

- `tso_on`: all three ordering switches on: the like-for-like comparison with Rosetta 2 (`MULTIBLOCK=1 TSOENABLED=1 VECTORTSOENABLED=1 MEMCPYSETTSOENABLED=1`)
- `tso_scalar`: ordering for scalar accesses only (FEX's defaults for the vector and string switches) (`MULTIBLOCK=1 TSOENABLED=1 VECTORTSOENABLED=0 MEMCPYSETTSOENABLED=0`)
- `tso_vector`: scalar and vector ordering, string instructions without (`MULTIBLOCK=1 TSOENABLED=1 VECTORTSOENABLED=1 MEMCPYSETTSOENABLED=0`)
- `tso_memcpy`: scalar and string-instruction ordering, vectors without (`MULTIBLOCK=1 TSOENABLED=1 VECTORTSOENABLED=0 MEMCPYSETTSOENABLED=1`)
- `tso_on_nomb`: all ordering on, multiblock off (`MULTIBLOCK=0 TSOENABLED=1 VECTORTSOENABLED=1 MEMCPYSETTSOENABLED=1`)
- `tso_on_x87rp`: all ordering on, x87 in 64-bit precision (`MULTIBLOCK=1 TSOENABLED=1 VECTORTSOENABLED=1 MEMCPYSETTSOENABLED=1 X87REDUCEDPRECISION=1`)
- `tso_off`: no ordering emulation: not correct for multi-threaded guests; an upper bound for guests known not to need it (`MULTIBLOCK=1 TSOENABLED=0 VECTORTSOENABLED=0 MEMCPYSETTSOENABLED=0`)

#### Headline: single-threaded integer, memory and floating-point kernels

| kernel | native arm64 | Rosetta 2 | tso_on | tso_scalar | tso_vector | tso_memcpy | tso_on_nomb | tso_on_x87rp | tso_off |
|---|---|---|---|---|---|---|---|---|---|
| crc32 | 47.8 [47.7-63.4] | 58.0 [57.6-62.5] | 66.6 [66.4-71.5] | 74.3 [66.4-111.8] | 70.0 [66.5-107.8] | 69.1 [66.6-108.0] | 66.6 [66.4-89.3] | 67.4 [66.1-79.4] | 59.3 [57.7-63.7] |
| hash64 | 20.6 [20.5-24.5] | 23.7 [23.6-27.5] | 22.8 [22.8-26.5] | 23.5 [22.8-42.7] | 27.1 [22.8-34.9] | 24.3 [22.7-37.5] | 22.5 [22.5-23.4] | 22.8 [22.6-32.5] | 23.5 [22.7-34.7] |
| sort | 25.0 [24.8-35.3] | 27.8 [27.0-35.9] | 36.3 [33.7-40.8] | 31.4 [28.7-46.9] | 38.5 [37.0-49.2] | 31.0 [28.5-40.5] | 40.1 [38.2-64.1] | 35.7 [33.1-45.0] | 29.3 [27.2-32.4] |
| interp | 54.2 [51.5-62.6] | 80.2 [76.4-89.2] | 152.9 [152.1-161.9] | 157.1 [152.2-227.5] | 159.5 [152.3-222.4] | 160.5 [153.1-185.0] | 164.3 [161.3-249.4] | 156.9 [152.0-177.1] | 74.7 [73.1-104.2] |
| dfa | 43.2 [42.2-60.0] | 57.0 [53.5-59.3] | 57.4 [53.9-65.3] | 68.1 [57.9-74.9] | 63.4 [56.8-71.0] | 63.1 [56.5-73.4] | 69.7 [68.4-97.2] | 61.9 [54.4-65.7] | 55.0 [49.0-76.2] |
| rep_movs | 16.7 [16.5-19.2] | 82.0 [80.8-88.6] | 347.7 [323.8-370.9] | 20.3 [17.7-36.9] | 18.3 [18.0-22.9] | 341.8 [276.1-357.4] | 352.5 [296.9-367.4] | 334.0 [284.8-355.1] | 18.4 [18.3-23.3] |
| copy_loop | 10.7 [8.0-15.0] | 12.3 [12.0-13.5] | 332.7 [159.3-978.6] | 10.7 [9.8-26.2] | 447.1 [280.1-1042.2] | 10.6 [8.5-13.4] | 772.3 [231.3-954.9] | 449.0 [106.6-1022.3] | 8.7 [8.5-9.8] |
| ptr_chase | 37.4 [30.4-88.2] | 42.2 [36.4-55.6] | 43.9 [30.0-78.2] | 43.1 [34.8-103.4] | 42.6 [38.1-62.9] | 42.5 [35.9-62.4] | 34.9 [31.2-58.1] | 53.4 [29.9-66.7] | 45.7 [32.5-72.1] |
| matmul_f64 | 22.2 [22.0-34.6] | 35.3 [32.7-42.8] | 73.4 [65.2-78.9] | 17.6 [17.3-49.4] | 71.1 [64.5-72.9] | 19.3 [17.6-38.2] | 64.8 [60.4-91.1] | 66.5 [59.6-86.7] | 17.5 [17.3-19.7] |
| matmul_f32x4 | 11.0 [10.9-17.2] | 10.7 [10.2-14.1] | 53.8 [53.1-58.5] | 12.3 [10.6-14.1] | 53.9 [52.8-54.5] | 11.1 [10.6-30.4] | 53.5 [52.9-53.9] | 54.6 [53.1-65.0] | 10.7 [10.6-10.8] |
| nbody | 10.1 [10.1-10.3] | 10.7 [10.2-16.8] | 34.3 [32.6-35.2] | 10.1 [10.1-19.9] | 32.8 [32.6-35.2] | 10.5 [10.1-15.3] | 32.6 [32.4-33.1] | 33.9 [32.6-38.2] | 10.1 [10.1-12.6] |
| fft | 12.2 [12.0-18.9] | 16.9 [15.9-18.4] | 73.1 [71.0-81.9] | 25.0 [21.1-30.2] | 73.7 [70.9-75.6] | 21.4 [21.3-41.3] | 74.5 [73.4-88.0] | 72.1 [71.0-81.7] | 15.2 [15.0-22.1] |
| muldiv | 14.6 [14.6-15.5] | 18.1 [16.7-21.5] | 14.7 [14.7-18.0] | 16.7 [14.7-23.9] | 15.6 [14.7-17.3] | 14.7 [14.7-17.2] | 15.4 [15.3-16.9] | 14.7 [14.7-23.7] | 15.2 [14.6-17.1] |
| calls | 11.9 [11.8-13.0] | 12.1 [10.6-14.6] | 15.4 [14.0-19.4] | 15.3 [13.0-26.1] | 15.6 [13.9-21.1] | 15.2 [13.0-21.7] | 62.2 [61.1-63.9] | 15.2 [13.1-18.4] | 15.2 [13.9-17.4] |
| deep_recursion | 28.3 [28.1-32.9] | 27.3 [20.6-51.6] | 36.8 [34.8-38.2] | 38.1 [34.9-43.3] | 36.3 [34.8-40.5] | 34.9 [34.8-42.5] | 38.4 [38.2-43.1] | 35.9 [34.7-40.1] | 35.5 [34.8-39.9] |

Ratio to Rosetta 2:

| kernel | native arm64 | tso_on | tso_scalar | tso_vector | tso_memcpy | tso_on_nomb | tso_on_x87rp | tso_off |
|---|---|---|---|---|---|---|---|---|
| crc32 | 0.82 | 1.15 | 1.28 | 1.21 | 1.19 | 1.15 | 1.16 | 1.02 |
| hash64 | 0.87 | 0.96 | 0.99 | 1.14 | 1.03 | 0.95 | 0.96 | 0.99 |
| sort | 0.90 | 1.31 | 1.13 | 1.39 | 1.12 | 1.44 | 1.29 | 1.05 |
| interp | 0.68 | 1.91 | 1.96 | 1.99 | 2.00 | 2.05 | 1.96 | 0.93 |
| dfa | 0.76 | 1.01 | 1.20 | 1.11 | 1.11 | 1.22 | 1.09 | 0.96 |
| rep_movs | 0.20 | 4.24 | 0.25 | 0.22 | 4.17 | 4.30 | 4.07 | 0.22 |
| copy_loop | 0.87 | 26.99 | 0.86 | 36.27 | 0.86 | 62.65 | 36.42 | 0.70 |
| ptr_chase | 0.88 | 1.04 | 1.02 | 1.01 | 1.01 | 0.83 | 1.26 | 1.08 |
| matmul_f64 | 0.63 | 2.08 | 0.50 | 2.01 | 0.55 | 1.84 | 1.88 | 0.50 |
| matmul_f32x4 | 1.02 | 5.02 | 1.15 | 5.02 | 1.03 | 4.99 | 5.09 | 1.00 |
| nbody | 0.95 | 3.21 | 0.95 | 3.07 | 0.99 | 3.05 | 3.17 | 0.95 |
| fft | 0.72 | 4.32 | 1.48 | 4.36 | 1.27 | 4.40 | 4.26 | 0.90 |
| muldiv | 0.81 | 0.81 | 0.92 | 0.86 | 0.81 | 0.85 | 0.81 | 0.84 |
| calls | 0.98 | 1.27 | 1.27 | 1.28 | 1.26 | 5.13 | 1.26 | 1.25 |
| deep_recursion | 1.04 | 1.35 | 1.40 | 1.33 | 1.28 | 1.41 | 1.32 | 1.30 |
| **geometric mean** | **0.77** (15 kernels) | **2.10** (15 kernels) | **1.00** (15 kernels) | **1.80** (15 kernels) | **1.17** (15 kernels) | **2.44** (15 kernels) | **2.15** (15 kernels) | **0.85** (15 kernels) |

#### x87 (x86-64 builds only; native arm64 has no 80-bit type)

| kernel | Rosetta 2 | tso_on | tso_scalar | tso_vector | tso_memcpy | tso_on_nomb | tso_on_x87rp | tso_off |
|---|---|---|---|---|---|---|---|---|
| x87 | 598.3 [581.2-685.3] | 439.0 [415.2-461.4] | 449.4 [413.2-547.0] | 456.7 [415.3-592.0] | 419.5 [412.5-463.3] | 421.5 [410.8-460.5] | 30.5 [29.6-37.7] | 425.0 [412.6-541.8] |

Ratio to Rosetta 2:

| kernel | tso_on | tso_scalar | tso_vector | tso_memcpy | tso_on_nomb | tso_on_x87rp | tso_off |
|---|---|---|---|---|---|---|---|
| x87 | 0.73 | 0.75 | 0.76 | 0.70 | 0.70 | 0.05 | 0.71 |

#### Thread kernels (in no mean; spread is large)

| kernel | native arm64 | Rosetta 2 | tso_on | tso_scalar | tso_vector | tso_memcpy | tso_on_nomb | tso_on_x87rp | tso_off |
|---|---|---|---|---|---|---|---|---|---|
| threads_lock | 43.3 [35.6-60.5] | 39.2 [21.2-56.6] | 68.8 [52.9-87.8] | 39.0 [32.1-49.1] | 60.7 [44.9-90.9] | 42.2 [31.3-58.1] | 57.6 [49.0-90.5] | 63.0 [46.2-81.2] | 44.4 [31.7-47.6] |
| threads_mp | 40.5 [23.5-56.6] | 8.1 [5.5-10.8] | 33.8 [9.9-40.6] | 32.2 [30.5-41.4] | 41.7 [32.3-52.0] | 35.4 [35.1-57.0] | 33.9 [28.4-40.3] | 34.4 [9.9-42.0] | 3.8 [2.4-6.2] wrong in 5 |

#### Time to first kernel result (`--only calls --quick`), seconds

| | native arm64 | Rosetta 2 | tso_on |
|---|---|---|---|
| first launch of the fresh build | 0.041 | 0.136 | 0.094 |
| later launches, median [range] | 0.003 [0.002-0.004] | 0.009 [0.008-0.012] | 0.005 [0.003-0.009] |

<!-- bench-results:end -->

### Where this port is far from Rosetta 2, and why

In the provisional runs the like-for-like figure, all ordering on, is about **2.1 times
Rosetta 2's time** over the headline kernels (geometric mean; 2.23 and 2.10 in the two
runs). Scalar ordering alone costs about nothing against Rosetta overall (1.00 and
1.06): it costs 1.1 to 2 times on the kernels dominated by scalar loads and stores, and
FEX is faster on others. From there, vector ordering adds most of the rest (`tso_vector`
1.80 and 2.13) and string-instruction ordering a smaller part, all of it in `rep_movs`
(`tso_memcpy` 1.17 and 1.24). The worst kernels:

1. **SSE kernels with vector ordering on.** `copy_loop` 21 to 63 times Rosetta 2 (and
   the most variable number in the table, see below), `matmul_f32x4` 5x, `fft` 4.3x,
   `nbody` 3.2x, `matmul_f64` 2x (scalar `movsd` is a vector access in FEX). With vector
   ordering on, FEX puts a `dmb ishld` after every vector load and a `dmb ish` before
   every vector store (`FEXCore/Source/Interface/Core/JIT/MemoryOps.cpp`, the
   `IsVectorAtomicTSOEnabled()` paths of the TSO load and store ops), whatever the host
   is. In the disassembly of `copy_loop`'s block:

   ```
   ldr q17, [x20]        ; movdqa xmm1, [r8+r10*8]
   dmb ishld
   eor v17.16b, v17.16b, v16.16b
   ...
   dmb ish
   str q17, [x20]        ; movdqa [r9+r10*8], xmm1
   ```

   `dmb ishld` orders earlier loads before later loads and stores: a half barrier;
   `dmb ish` is a full barrier. Scalar accesses use RCpc loads and release stores
   (`ldapr`/`ldapur`, `stlr`) instead. Arm's FEAT_LRCPC3 adds RCpc vector loads and
   stores, but the core has no support for them (`Addressing.cpp`: "FEAT_LRCPC3
   (Unsupported by FEXCore currently)"), and the M4 does not have the feature
   (`hw.optional.arm.FEAT_LRCPC3` does not exist), so neither is the cause here: the
   barriers are FEX's only method for vector ordering. With vector ordering off these
   kernels run at 0.5x to 1.5x Rosetta.

   `copy_loop` with vector ordering is the number that depends most on the machine's
   state: launch medians from 107 ms to 1,042 ms within one run, across the configurations with
   vector ordering on. Earlier, two runs
   alone gave 1.0 to 1.3 s and two with six other cores busy gave 0.1 to 0.75 s: a loop
   that waits on a barrier after every load is bound by memory latency, which here
   appears to be lower when the machine is busy. Its ratio is not a stable figure.

2. **`rep_movs` with string ordering on: about 4.2x Rosetta 2, 17 to 19 times slower than
   without it.** With `MEMCPYSETTSOENABLED=1`, `rep movsb` becomes a loop of one `ldaprb`
   and one `stlrb` per byte (and `rep movsq` one `ldapr`/`stlr` per quadword); without
   it, 64 bytes per iteration with `ldp q`/`stp q`. Rosetta 2 is itself 5 times slower
   than native here.

3. **`interp`: about 1.9x Rosetta 2 with scalar ordering, 0.93x without.** Every guest
   load that is not to the stack becomes `ldapur`/`ldapr` and every store `stlr`,
   including the interpreter's reads of its instruction stream and register array:

   ```
   ldapurb w10, [x4]          ; movzbl (%rax), %esi    (opcode)
   ldapurb w16, [x4, #1]      ; movzbl 0x1(%rax), %r12d
   ldapur  w20, [x4, #4]      ; movslq 0x4(%rax), %rcx
   ```

   Indirect branches are not the problem: the jump-table `jmp *%rax` becomes an inline
   lookup in the core's L1 code cache (`ldp` from the thread's table, compare, `br`),
   six instructions on a hit. `sort` (1.1 to 1.3x) and `crc32` (1.15 to 1.3x) pay the
   same, less.

4. **`calls` with multiblock off: 5 to 6 times Rosetta 2** (1.3x with it on). Each call
   and return ends a block and goes through the dispatcher.

5. **Threads.** `threads_mp` is 4 to 5 times Rosetta 2 with ordering on and
   `threads_lock` 1.5 to 1.8 times, with launch medians varying up to 4 times. Native
   arm64 is itself much slower than Rosetta 2 on `threads_mp`, which is not explained.
   Numbers this noisy on a shared machine are not a basis for conclusions.

**x87** is slow in every x86-64 build (Rosetta 2 about 590 ms, FEX about 430 ms, against
3.6 ms for native arm64's double arithmetic, which is not comparable). FEX implements
80-bit x87 arithmetic in software; Rosetta 2 appears to do the same. FEX is about 1.4
times faster than Rosetta 2 at full precision, and about 20 times faster with
`X87REDUCEDPRECISION=1`, at the cost of a different answer.

### Candidates (recorded, not done in this package)

- Use FEAT_LRCPC3 RCpc vector loads and stores for vector ordering where the host has
  them (the core does not support them yet; no Apple chip so far has them).
- Cheaper vector ordering on hosts without them: drop a `dmb ishld` that is followed by
  a `dmb ish` with no memory access between them (`copy_loop`'s block has this pattern),
  and merge barriers between consecutive vector accesses where x86 ordering allows it.
- Treat provably thread-private memory (beyond the stack, which is already exempt) as
  needing no ordering.
- `rep movs`/`rep stos` with string ordering: x86 makes no promise about the order of
  the individual stores of one fast string instruction; copying in larger ordered units,
  or with plain wide stores bracketed by barriers, would keep the guarantee that matters
  (ordering against other instructions) at a fraction of the cost.
- Translation counters through `fexmac` (package F).

## How the code was examined

`fexmac` refuses the core's diagnostic options (`DUMPIR`, `DISASSEMBLE` and the like are
not in its list), and the disassembler needs a build with `-DENABLE_VIXL_DISASSEMBLER=ON`.
For this analysis only, a separate local build (not committed, not part of any test)
added `DISASSEMBLE` to `Options.cpp` and ran `FEXCoreBench` with `--option DISASSEMBLE=6`
and `SILENTLOG=0`; the core then writes each block's host code to standard error. Two
things found on the way:

- The `External/vixl` submodule is not initialised in the port's worktrees; it had to be
  fetched (`git submodule update --init --depth 1 External/vixl`).
- With the macOS 27 SDK's libc++, `External/vixl/src/aarch64/disasm-aarch64.cc` does not
  compile (`std::map` used without `#include <map>`); `-DCMAKE_CXX_FLAGS="-include map"`
  works around it. The default build does not compile that file.

No debugger or profiler was used.

## Rosetta 2 on this machine

- **An unsigned x86-64 program hangs at launch, unkillably.** Four freshly linked,
  unsigned x86-64 programs (including a three-line hello world) stayed in state `U`
  (uninterruptible) at launch, ignored `SIGALRM` and `SIGKILL` and are still there; the
  system log shows Rosetta's analysis failing on the missing signature
  (`SecStaticCodeCreate Failed`). Every ad hoc signed one (`codesign -s -`) ran at once.
  Rosetta 2 had been installed on this Mac the same morning. The scripts therefore sign
  the x86-64 builds, and `macos_bench.py` gives every launch a deadline after which it
  kills the process group and, if the process still does not end, reports it and goes
  on.
- The first launch of a new x86-64 program costs Rosetta 2 about 0.13 to 0.17 s before
  its first kernel result (ahead-of-time translation); later launches about 0.01 s.

## How to run it

```
ninja -j4 -C build FEXCoreBench
# The measurement: writes bench-out/results.json and bench-out/results.md; with
# --write-docs also replaces the generated block above. FINAL only if the machine was
# quiet (other processes at most 0.5 cores on average and 1.5 at any sample; exit
# status 2 otherwise).
python3 Scripts/macos_bench.py --loader build/Bin/FEXCoreBench --out bench-out --final --write-docs
# The correctness check (what the Mac workflow runs, as built and with --hardened):
python3 Scripts/macos_bench.py --loader build/Bin/FEXCoreBench --out bench-quick --quick [--hardened]
python3 Scripts/macos_bench_mutations.py
```

`--configs`, `--only`, `--launches`, `--reps` narrow or widen a run. `FEXCoreBench`
alone:

```
FEXCoreBench [--option NAME=VALUE]... [--quick] [--literal [--expect-kernels N]] [--reps N] [--only k1,k2] bench-out/bench-guest.macho
```
