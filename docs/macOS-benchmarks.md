# macOS benchmarks: a guest program under this port, Rosetta 2 and native

This is the evidence for exit criteria 7 ("a real guest program runs") and 8 ("speed is
measured") of `docs/macOS-v1.0-plan.md`, and the method behind it.

**The timing numbers in this document come from the generated blocks, which say whether
they are provisional or final.** A final measurement is made on an idle machine with the
same script (`--final --write-docs`, see "How to run it"), which replaces every generated
block in this document and in the status document and labels them FINAL only if the
machine was quiet enough.

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
checks), copies them, registers the executable ones with `fexmac_add_code_range` and starts at
the `LC_UNIXTHREAD` entry. No tool beyond the command
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
| `muldiv` | Miller-Rabin with Montgomery multiplication (`mul`); 64-bit `div` and `idiv` with divisors from 1 to 2^64 (quotients up to 2^64); 128-by-64 `divq` and `idivq` with large high halves | 60,000 candidates near 2^62 |
| `calls` | recursive Fibonacci: call and return | fib(34) |
| `deep_recursion` | 50,000-deep non-tail recursion (deeper than the core's call/return stack) | 100 repetitions |
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
this: it applies 58 mutations, one at a time, to a copy of `kernels.c` and requires each
to change its kernel's checksum (natively on arm64, and under Rosetta 2 for those that exist only in the x86-64 code). They include, per kernel:

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
| muldiv | candidates shifted by one; wrong divisor; small divisors replaced by large ones; 128-by-64 division with the high half dropped; 128-by-64 quotient low bit lost; signed 128-by-64 division with a positive dividend only (arm64 and x86-64); `divq` given a zero high half (x86-64); modular inverse one Newton step short |
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

Every figure about speed in this document and in `docs/macOS-port-status.md` is in a
block that `Scripts/macos_bench.py --write-docs` generates from one results file, which
is committed beside this document as `docs/macOS-benchmarks-results.json`; the prose
around the blocks states conclusions only and refers to them. `--check-docs` fails if a
block differs from what that file gives or if a measured figure (a ratio, a time, a
percentage) appears anywhere else, so a new measurement leaves nothing stale behind.

### Summary

<!-- bench-summary:begin -->
<!-- bench-summary:end -->

### Method

- Every build and configuration is launched `--launches` times (five by default), and
  the launches are interleaved: each round runs every build once, starting one later each
  round, so drift in the machine's state reaches all of them alike. Each launch does one
  warm-up and `--reps` (three by default) timed runs of each kernel; a launch's value is
  the median of its runs, and the report gives the median of the launch medians and
  their range.
- Every thread runs at the user-interactive quality of service, which keeps it on the
  performance cores when they are free.
- The summary sets are fixed in the script and reported separately:
  - **headline**: the 15 single-threaded integer, memory and floating-point kernels;
    geometric mean of the ratio to Rosetta 2;
  - **x87**: x86-64 builds only (Rosetta 2 and FEX), never against native arm64,
    whose `long double` is a different type;
  - **thread kernels**: reported with their spread, in no mean.
  A kernel enters a configuration's mean only if it was right in every launch of it.
- Load: every two seconds the script reads the whole machine's busy CPU time from the
  kernel (`host_processor_info`) and subtracts its own CPU time and that of the program it
  is running (`getrusage`, `proc_pid_rusage`); the difference is the load from other
  processes. (`ps`'s `%cpu` is a decaying average that barely counts short-lived
  processes such as a build's compiler jobs, and under-reported a saturated machine
  several times over.) The samples, with the load average, go into the JSON. A run is
  labelled FINAL only with `--final`, from a clean commit (`git describe --dirty`), and
  only if other processes used at most half a core on average and one and a half at any
  sample; `--measure-load SECONDS` reports the same verdict without running anything.
  The load average on this Mac includes four processes stuck in Rosetta 2 since a launch
  earlier in this work (see "Rosetta 2 on this machine"); they use no CPU.
- Clock reads: the kernels hash their buffers outside the timed work, bracketed by two
  clock reads, which under this port are system calls. The program measures the cost of
  one read at start and subtracts it for every bracket, so the bias this would add is
  removed; the measured cost is in the summary.
- Translation overhead: blocks compiled and blocks loaded from the code cache per launch
  (`fexmac_context_get_counters`, printed by `FEXCoreBench` at exit), the time from
  launch to the first kernel result and the whole launch's time, per configuration; and
  the time to the first kernel result of a short run (`--only calls --quick`) at the
  first launch of a fresh build and at later launches.

### Configurations

All FEX configurations use `MAXINST=500 MULTIBLOCK=1` unless stated. Rosetta 2 runs x86
code with the hardware TSO mode of Apple's cores, which macOS does not let an ordinary
process switch on, so the like-for-like comparison is **`tso_on`**, all three of FEX's
ordering switches on: that is the configuration that is correct for any guest.
`tso_off` is **not correct for multi-threaded guests** (this benchmark's own
message-passing kernel fails in it) and is given only as an upper bound for a guest
known not to need ordering. `tso_scalar` (FEX's defaults), `tso_vector` and `tso_memcpy`
add the vector and string-instruction switches one at a time, to show what each costs.

Two configurations show the code cache (exit criterion 6) on this program, both
`tso_on` with `FEXCoreBench --code-cache DIR`, which sets `fexmac`'s
`code_cache_enabled` and `code_cache_directory`: **`tso_on_cache_cold`** starts every
launch with a fresh, empty directory, and **`tso_on_cache_warm`** uses a directory that
one run with the same arguments filled first. The directories are private ones under
`--out/code-cache`, never `~/Library/Caches`. Their checksums are checked like every
other configuration's; a warm launch must load its blocks (at most one in twenty
compiled), and a cold one must compile and queue them. A cold launch also loads a few
blocks: ones it wrote itself earlier in the same launch. A cache entry records every
entry point of its translated code, and when the guest later enters such a code region
at an address the in-memory lookup does not hold (most likely the resume point after a
system call, which is inside an already translated block), the entry the cache writer
has stored meanwhile is found. The literal mode, which makes no system call until exit,
loads none on a cold cache; the timed mode loads a few, and many more with multiblock
off, where blocks are short and every system call return is a new entry.
`FEXCoreBench` registers the image's executable segments with `fexmac_add_code_range`
in every configuration.

### Results

<!-- bench-results:begin -->
<!-- Generated by Scripts/macos_bench.py; replace by running it again, do not edit by hand. -->

**PROVISIONAL** numbers: other work was loading the machine (other processes used 3.69 cores on average, at most 9.02, of 10; load average 21.75 on average). They show the shape, not final values.

Apple M4, 4 performance + 6 efficiency cores, 16 GB, macOS 27.0.1, Apple clang version 21.0.0 (clang-2100.4.1.1); commit `c8ef43e3b`; 2026-11-27T08:29:12-05:00 to 2026-11-27T08:37:42-05:00. 5 launches per build and configuration, interleaved, each with one warm-up and 3 timed runs per kernel. Cells: median of the launch medians in ms [lowest-highest launch median]. Ratios are to Rosetta 2's median (below 1: faster than Rosetta 2).

FEX configurations (all with `MAXINST=500 MULTIBLOCK=1` unless stated):

- `tso_on`: all three ordering switches on: the like-for-like comparison with Rosetta 2 (`MULTIBLOCK=1 TSOENABLED=1 VECTORTSOENABLED=1 MEMCPYSETTSOENABLED=1`)
- `tso_scalar`: ordering for scalar accesses only (FEX's defaults for the vector and string switches) (`MULTIBLOCK=1 TSOENABLED=1 VECTORTSOENABLED=0 MEMCPYSETTSOENABLED=0`)
- `tso_vector`: scalar and vector ordering, string instructions without (`MULTIBLOCK=1 TSOENABLED=1 VECTORTSOENABLED=1 MEMCPYSETTSOENABLED=0`)
- `tso_memcpy`: scalar and string-instruction ordering, vectors without (`MULTIBLOCK=1 TSOENABLED=1 VECTORTSOENABLED=0 MEMCPYSETTSOENABLED=1`)
- `tso_on_nomb`: all ordering on, multiblock off (`MULTIBLOCK=0 TSOENABLED=1 VECTORTSOENABLED=1 MEMCPYSETTSOENABLED=1`)
- `tso_on_x87rp`: all ordering on, x87 in 64-bit precision (`MULTIBLOCK=1 TSOENABLED=1 VECTORTSOENABLED=1 MEMCPYSETTSOENABLED=1 X87REDUCEDPRECISION=1`)
- `tso_off`: no ordering emulation: not correct for multi-threaded guests; an upper bound for guests known not to need it (`MULTIBLOCK=1 TSOENABLED=0 VECTORTSOENABLED=0 MEMCPYSETTSOENABLED=0`)
- `tso_on_cache_cold`: as tso_on, with the code cache on in a fresh, empty directory for every launch: everything is compiled and written to the cache (`MULTIBLOCK=1 TSOENABLED=1 VECTORTSOENABLED=1 MEMCPYSETTSOENABLED=1`)
- `tso_on_cache_warm`: as tso_on, with the code cache on in a directory one run with the same arguments filled first: translations are loaded, not compiled (`MULTIBLOCK=1 TSOENABLED=1 VECTORTSOENABLED=1 MEMCPYSETTSOENABLED=1`)

#### Headline: single-threaded integer, memory and floating-point kernels

| kernel | native arm64 | Rosetta 2 | tso_on | tso_scalar | tso_vector | tso_memcpy | tso_on_nomb | tso_on_x87rp | tso_off | tso_on_cache_cold | tso_on_cache_warm |
|---|---|---|---|---|---|---|---|---|---|---|---|
| crc32 | 50.1 [48.4-110.5] | 82.3 [58.5-131.5] | 73.7 [66.4-242.1] | 106.2 [70.3-118.8] | 94.3 [69.8-114.8] | 85.4 [78.1-236.7] | 112.8 [90.6-362.9] | 71.7 [68.0-105.9] | 87.0 [60.4-128.3] | 120.7 [90.7-207.1] | 108.6 [82.5-159.4] |
| hash64 | 24.4 [20.5-37.2] | 41.1 [23.6-43.8] | 24.8 [22.8-32.8] | 30.7 [24.6-55.7] | 34.7 [23.1-67.6] | 32.2 [26.0-110.7] | 34.5 [22.4-48.2] | 33.6 [22.8-42.0] | 45.0 [25.0-62.8] | 37.1 [24.3-72.2] | 31.3 [23.8-63.0] |
| sort | 26.7 [25.9-46.9] | 42.3 [35.6-99.0] | 36.9 [33.1-40.1] | 34.2 [30.9-122.8] | 48.1 [34.1-119.2] | 44.3 [33.5-152.0] | 64.0 [35.9-125.3] | 35.1 [33.3-56.2] | 42.6 [33.6-63.7] | 73.5 [45.6-97.5] | 53.1 [35.7-96.2] |
| interp | 70.7 [52.1-185.5] | 162.4 [82.2-177.6] | 166.9 [152.3-426.9] | 187.1 [164.1-591.2] | 200.6 [152.7-490.1] | 175.8 [165.4-459.8] | 339.0 [222.6-442.9] | 177.2 [159.7-217.4] | 143.9 [89.5-289.3] | 257.8 [204.2-445.4] | 219.3 [150.9-413.7] |
| dfa | 51.8 [43.1-133.9] | 117.4 [62.3-156.6] | 66.1 [54.0-295.7] | 110.4 [66.4-227.6] | 93.1 [56.5-250.7] | 84.1 [76.6-235.2] | 116.9 [102.8-213.7] | 71.3 [54.0-91.2] | 109.1 [71.5-181.0] | 128.4 [69.6-198.3] | 107.4 [57.7-202.2] |
| rep_movs | 18.0 [16.6-21.1] | 128.7 [93.8-141.0] | 268.9 [241.1-546.7] | 35.4 [24.3-132.9] | 23.6 [20.4-58.8] | 219.3 [194.5-476.2] | 355.5 [160.4-369.1] | 261.2 [184.4-358.2] | 32.1 [22.6-110.4] | 330.3 [210.7-559.8] | 342.1 [208.6-476.7] |
| copy_loop | 11.1 [9.7-21.0] | 15.0 [12.3-34.6] | 299.9 [138.4-361.6] | 18.0 [9.5-88.1] | 160.4 [98.6-321.5] | 16.9 [11.4-33.7] | 269.3 [111.0-639.3] | 191.3 [78.3-375.8] | 11.8 [8.8-61.5] | 283.0 [109.7-376.1] | 265.0 [148.7-383.4] |
| ptr_chase | 55.9 [38.7-212.5] | 87.0 [35.4-151.2] | 82.0 [41.7-278.3] | 111.2 [69.4-247.4] | 57.9 [38.2-215.7] | 64.5 [39.1-141.6] | 52.3 [30.2-183.1] | 54.2 [34.3-108.4] | 65.7 [32.5-324.5] | 67.1 [36.8-266.0] | 64.3 [32.1-97.2] |
| matmul_f64 | 37.0 [26.3-44.0] | 63.8 [34.1-103.5] | 81.4 [62.8-269.2] | 28.1 [17.6-68.9] | 93.0 [63.8-309.1] | 34.7 [31.6-56.1] | 79.6 [64.7-181.7] | 97.3 [71.0-142.7] | 26.0 [17.4-154.1] | 116.3 [82.3-216.9] | 105.5 [61.8-247.3] |
| matmul_f32x4 | 14.5 [10.9-35.3] | 15.4 [10.2-19.8] | 56.0 [53.6-160.2] | 14.5 [11.2-18.0] | 74.2 [54.9-132.0] | 12.6 [10.6-35.3] | 67.0 [53.5-154.0] | 64.6 [55.2-114.2] | 29.9 [10.7-40.4] | 104.4 [54.8-126.7] | 57.9 [53.2-158.5] |
| nbody | 12.0 [10.1-16.8] | 10.6 [10.2-15.0] | 35.3 [32.7-109.2] | 13.5 [10.6-19.2] | 51.4 [37.0-101.6] | 10.8 [10.1-33.0] | 53.7 [32.6-78.2] | 35.4 [32.6-42.0] | 28.2 [10.1-49.8] | 82.0 [37.2-93.6] | 58.7 [45.9-110.3] |
| fft | 12.0 [12.0-18.3] | 20.3 [16.2-30.5] | 77.8 [73.7-240.9] | 41.7 [24.0-57.1] | 104.0 [89.3-202.8] | 37.3 [27.5-99.2] | 110.8 [85.9-175.7] | 95.8 [70.6-249.5] | 35.2 [21.0-39.9] | 94.1 [73.3-221.5] | 103.7 [74.1-223.2] |
| muldiv | 18.9 [14.6-86.2] | 17.4 [16.6-64.7] | 14.7 [14.7-19.3] | 27.9 [16.3-60.0] | 25.7 [14.7-46.6] | 14.7 [14.6-33.0] | 17.2 [15.4-35.8] | 23.9 [19.1-34.2] | 20.8 [14.9-81.9] | 19.2 [14.7-59.9] | 25.8 [14.7-37.4] |
| calls | 21.6 [16.5-29.5] | 10.6 [10.6-12.2] | 16.4 [15.1-44.2] | 28.2 [16.4-92.6] | 17.3 [15.0-25.3] | 23.2 [13.0-80.1] | 74.6 [62.6-108.2] | 16.6 [13.0-25.6] | 50.2 [21.5-56.7] | 24.4 [17.0-57.4] | 15.3 [15.1-27.3] |
| deep_recursion | 32.7 [28.3-58.6] | 47.8 [30.4-75.4] | 40.0 [35.8-87.4] | 50.2 [40.5-81.8] | 65.0 [34.8-156.1] | 93.9 [37.5-125.8] | 60.2 [39.3-66.0] | 56.6 [38.3-105.7] | 58.7 [42.0-90.6] | 40.5 [37.4-109.0] | 36.8 [34.8-116.6] |

Ratio to Rosetta 2:

| kernel | native arm64 | tso_on | tso_scalar | tso_vector | tso_memcpy | tso_on_nomb | tso_on_x87rp | tso_off | tso_on_cache_cold | tso_on_cache_warm |
|---|---|---|---|---|---|---|---|---|---|---|
| crc32 | 0.61 | 0.89 | 1.29 | 1.15 | 1.04 | 1.37 | 0.87 | 1.06 | 1.47 | 1.32 |
| hash64 | 0.59 | 0.60 | 0.74 | 0.84 | 0.78 | 0.84 | 0.82 | 1.09 | 0.90 | 0.76 |
| sort | 0.63 | 0.87 | 0.81 | 1.14 | 1.05 | 1.51 | 0.83 | 1.01 | 1.74 | 1.26 |
| interp | 0.44 | 1.03 | 1.15 | 1.24 | 1.08 | 2.09 | 1.09 | 0.89 | 1.59 | 1.35 |
| dfa | 0.44 | 0.56 | 0.94 | 0.79 | 0.72 | 0.99 | 0.61 | 0.93 | 1.09 | 0.91 |
| rep_movs | 0.14 | 2.09 | 0.27 | 0.18 | 1.70 | 2.76 | 2.03 | 0.25 | 2.57 | 2.66 |
| copy_loop | 0.74 | 20.01 | 1.20 | 10.70 | 1.13 | 17.97 | 12.77 | 0.79 | 18.88 | 17.68 |
| ptr_chase | 0.64 | 0.94 | 1.28 | 0.67 | 0.74 | 0.60 | 0.62 | 0.75 | 0.77 | 0.74 |
| matmul_f64 | 0.58 | 1.28 | 0.44 | 1.46 | 0.54 | 1.25 | 1.52 | 0.41 | 1.82 | 1.65 |
| matmul_f32x4 | 0.94 | 3.63 | 0.94 | 4.81 | 0.82 | 4.34 | 4.18 | 1.94 | 6.76 | 3.75 |
| nbody | 1.13 | 3.33 | 1.28 | 4.84 | 1.02 | 5.06 | 3.34 | 2.66 | 7.73 | 5.53 |
| fft | 0.59 | 3.82 | 2.05 | 5.11 | 1.83 | 5.45 | 4.71 | 1.73 | 4.63 | 5.10 |
| muldiv | 1.09 | 0.84 | 1.61 | 1.48 | 0.84 | 0.99 | 1.38 | 1.20 | 1.11 | 1.48 |
| calls | 2.03 | 1.55 | 2.65 | 1.62 | 2.19 | 7.02 | 1.57 | 4.73 | 2.30 | 1.44 |
| deep_recursion | 0.68 | 0.84 | 1.05 | 1.36 | 1.97 | 1.26 | 1.18 | 1.23 | 0.85 | 0.77 |
| **geometric mean** | **0.65** (15 kernels) | **1.55** (15 kernels) | **1.04** (15 kernels) | **1.56** (15 kernels) | **1.07** (15 kernels) | **2.20** (15 kernels) | **1.64** (15 kernels) | **1.09** (15 kernels) | **2.21** (15 kernels) | **1.90** (15 kernels) |

#### x87 (x86-64 builds only; native arm64 has no 80-bit type)

| kernel | Rosetta 2 | tso_on | tso_scalar | tso_vector | tso_memcpy | tso_on_nomb | tso_on_x87rp | tso_off | tso_on_cache_cold | tso_on_cache_warm |
|---|---|---|---|---|---|---|---|---|---|---|
| x87 | 1177.3 [586.6-1472.8] | 510.8 [433.0-1348.1] | 625.4 [510.1-1450.7] | 746.8 [490.6-829.1] | 794.1 [582.5-1154.3] | 549.4 [472.1-949.4] | 47.4 [35.4-68.3] | 676.1 [588.1-1249.1] | 604.7 [524.6-1197.5] | 665.8 [545.0-880.1] |

Ratio to Rosetta 2:

| kernel | tso_on | tso_scalar | tso_vector | tso_memcpy | tso_on_nomb | tso_on_x87rp | tso_off | tso_on_cache_cold | tso_on_cache_warm |
|---|---|---|---|---|---|---|---|---|---|
| x87 | 0.43 | 0.53 | 0.63 | 0.67 | 0.47 | 0.04 | 0.57 | 0.51 | 0.57 |

#### Thread kernels (in no mean; spread is large)

| kernel | native arm64 | Rosetta 2 | tso_on | tso_scalar | tso_vector | tso_memcpy | tso_on_nomb | tso_on_x87rp | tso_off | tso_on_cache_cold | tso_on_cache_warm |
|---|---|---|---|---|---|---|---|---|---|---|---|
| threads_lock | 43.7 [38.6-50.7] | 33.0 [28.2-48.1] | 50.3 [45.8-62.5] | 42.0 [34.0-57.5] | 51.6 [45.4-59.6] | 35.9 [33.4-53.7] | 59.6 [56.7-71.0] | 48.9 [33.4-50.1] | 32.6 [25.3-39.5] | 46.9 [18.9-54.8] | 35.7 [31.7-52.7] |
| threads_mp | 47.9 [13.7-54.0] | 9.0 [4.6-10.7] | 29.9 [9.5-54.9] | 28.6 [15.4-37.8] | 31.9 [10.0-35.9] | 34.4 [27.5-64.4] | 31.4 [25.8-37.7] | 31.4 [8.8-38.7] | 4.5 [3.7-5.7] wrong in 5 | 28.0 [16.9-43.3] | 26.8 [20.4-44.5] |

#### Time to first kernel result (`--only calls --quick`), seconds

| | native arm64 | Rosetta 2 | tso_on |
|---|---|---|---|
| first launch of the fresh build | 0.049 | 0.142 | 0.005 |
| later launches, median [range] | 0.003 [0.003-0.005] | 0.012 [0.009-0.016] | 0.005 [0.004-0.008] |

#### Translation and the code cache (per launch of the full program; median [range])

Blocks compiled and loaded come from `fexmac_context_get_counters`. First result: launch to the end of the first kernel's warm-up. Launch: the whole run, all kernels, every repetition.

| configuration | blocks compiled | blocks loaded from the cache | first result, s | launch, s |
|---|---|---|---|---|
| tso_on | 83 [79-87] | 0 [0-0] | 0.074 [0.071-0.080] | 8.75 [7.34-18.56] |
| tso_scalar | 85 [80-85] | 0 [0-0] | 0.093 [0.074-0.113] | 6.73 [5.04-11.73] |
| tso_vector | 85 [84-87] | 0 [0-0] | 0.087 [0.075-0.111] | 8.63 [6.77-15.78] |
| tso_memcpy | 86 [83-87] | 0 [0-0] | 0.084 [0.070-0.158] | 9.25 [6.95-13.14] |
| tso_on_nomb | 547 [538-556] | 0 [0-0] | 0.081 [0.074-0.197] | 12.70 [9.72-12.76] |
| tso_on_x87rp | 85 [81-85] | 0 [0-0] | 0.076 [0.071-0.101] | 6.68 [6.23-6.94] |
| tso_off | 84 [82-85] | 0 [0-0] | 0.069 [0.066-0.102] | 7.83 [5.36-10.83] |
| tso_on_cache_cold | 83 [80-83] | 1 [1-3] | 0.091 [0.072-0.115] | 12.66 [8.44-16.64] |
| tso_on_cache_warm | 0 [0-0] | 82 [79-86] | 0.079 [0.074-0.084] | 10.03 [8.32-15.71] |

<!-- bench-results:end -->

### Where this port is far from Rosetta 2, and why

The summary gives the figures; this section explains them. With all ordering on, FEX
is slower than Rosetta 2 overall, and almost all of the difference is the cost of
emulating x86 memory ordering in software, which Rosetta 2 gets from the hardware.
Scalar ordering alone costs little overall: it slows the kernels dominated by scalar
loads and stores, and FEX is faster than Rosetta 2 on others. Vector ordering adds most
of the rest, and string-instruction ordering a smaller part, all of it in `rep_movs`. The
summary lists the kernels each switch slows. Runs on a loaded machine move these ratios a
lot, even between configurations that run the same code (`tso_on` and the two cache
ones), so only a quiet run gives figures. The mechanisms:

1. **SSE kernels with vector ordering on** (`copy_loop`, `matmul_f32x4`, `fft`,
   `nbody`, `matmul_f64`, where scalar `movsd` is a vector access in FEX). With vector
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
   kernels are close to Rosetta 2.

   `copy_loop` with vector ordering is the result that depends most on the machine's
   state (its range of launch medians in the table is the widest). In an earlier
   experiment it ran several times faster with six other cores kept busy than with them
   idle: a loop that waits on a barrier after every load is bound by memory latency,
   which here appears to be lower when the machine is busy. Its ratio is not a stable
   figure.

2. **`rep_movs` with string ordering on.** With `MEMCPYSETTSOENABLED=1`, `rep movsb`
   becomes a loop of one `ldaprb` and one `stlrb` per byte (and `rep movsq` one
   `ldapr`/`stlr` per quadword); without it, 64 bytes per iteration with `ldp q`/`stp q`.
   Rosetta 2 is itself much slower than native here.

3. **`interp` with scalar ordering.** Every guest load that is not to the stack becomes
   `ldapur`/`ldapr` and every store `stlr`, including the interpreter's reads of its
   instruction stream and register array:

   ```
   ldapurb w10, [x4]          ; movzbl (%rax), %esi    (opcode)
   ldapurb w16, [x4, #1]      ; movzbl 0x1(%rax), %r12d
   ldapur  w20, [x4, #4]      ; movslq 0x4(%rax), %rcx
   ```

   With ordering off it runs at about Rosetta 2's speed. Indirect branches are not the
   problem: the jump-table `jmp *%rax` becomes an inline lookup in the core's L1 code
   cache (`ldp` from the thread's table, compare, `br`), six instructions on a hit.
   `sort` and `crc32` pay the same, less.

4. **`calls` with multiblock off.** Each call and return ends a block and goes through
   the dispatcher; with multiblock on it is close to Rosetta 2.

5. **Threads.** The thread kernels' launch medians vary widely, under every build.
   Native arm64 is itself much slower than Rosetta 2 on `threads_mp`, which is not
   explained. Numbers this noisy on a shared machine are not a basis for conclusions.

**The code cache** works on this program: a warm launch compiles no block and loads all
of them, a cold one compiles and queues them (see "Configurations" for the few it loads).
Its effect on time is small here: translating the whole program is a small part of a
launch, so the times with and without the cache in the translation table are close, and
on a loaded machine within the noise. A cold launch also pays for writing the cache. A
program with much more code than this one is needed to show the time the cache saves.

**x87** is slow in every x86-64 build, against native arm64's double arithmetic, which
is not comparable. FEX implements 80-bit x87 arithmetic in software; Rosetta 2 appears to
do the same. FEX is faster than Rosetta 2 at full precision, and much faster with
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
- The first launch of a new x86-64 program costs Rosetta 2 noticeably more time before
  its first kernel result than later launches (ahead-of-time translation); the table
  "Time to first kernel result" has the figures.

## How to run it

```
ninja -j4 -C build FEXCoreBench
# The measurement: writes bench-out/results.json and bench-out/results.md; with
# --write-docs also replaces the generated blocks in both documents and copies the
# results to docs/macOS-benchmarks-results.json. FINAL only from a clean commit and if
# the machine was quiet (exit status 2 otherwise).
python3 Scripts/macos_bench.py --loader build/Bin/FEXCoreBench --out bench-out --final --write-docs
# Is the machine quiet enough now? And do the documents match the committed results?
python3 Scripts/macos_bench.py --measure-load 30
python3 Scripts/macos_bench.py --check-docs
# The correctness check (what the Mac workflow runs, as built and with --hardened):
python3 Scripts/macos_bench.py --loader build/Bin/FEXCoreBench --out bench-quick --quick [--hardened]
python3 Scripts/macos_bench_mutations.py
```

`--configs`, `--only`, `--launches`, `--reps` narrow or widen a run. `FEXCoreBench`
alone:

```
FEXCoreBench [--option NAME=VALUE]... [--code-cache DIR] [--quick] [--literal [--expect-kernels N]] [--reps N] [--only k1,k2] bench-out/bench-guest.macho
```
