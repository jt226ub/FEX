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
checks), copies them, registers the image with `fexmac_memory_map` (executable segments R+X) and starts at
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
<!-- Generated by Scripts/macos_bench.py from docs/macOS-benchmarks-results.json; do not edit by hand. -->

**PROVISIONAL** measurement of `FEX-2611-286-g3b12faa03`, 2026-11-27T19:20:57-05:00: other processes kept 0.33 cores busy on average (at most 2.36) of the machine's 10; load average 1.9. Provisional numbers show the shape, not final values.

- **Headline** (15 single-threaded integer, memory and floating-point kernels), geometric mean of the time relative to Rosetta 2: all ordering on (`tso_on`), like for like with Rosetta 2's hardware TSO **2.07**; scalar ordering only (`tso_scalar`, FEX's defaults) **0.98**; scalar and vector ordering (`tso_vector`) **1.63**; scalar and string-instruction ordering (`tso_memcpy`) **1.21**; all on, multiblock off **2.30**; all on, reduced x87 precision **2.03**; all on, code cache cold **2.08**; all on, code cache warm **2.04**; native arm64 0.73; ordering off 0.91 (an upper bound only: not correct for multi-threaded guests).
- **Slowest against Rosetta 2 with all ordering on** (ratio; in brackets the same kernel with scalar ordering only and with ordering off): `copy_loop` 56.13 (tso_scalar 2.54, tso_off 2.48); `rep_movs` 4.87 (tso_scalar 0.19, tso_off 0.20); `matmul_f32x4` 4.82 (tso_scalar 1.05, tso_off 1.06); `fft` 4.22 (tso_scalar 1.45, tso_off 1.12); `nbody` 2.95 (tso_scalar 1.00, tso_off 1.00); `matmul_f64` 1.93 (tso_scalar 0.67, tso_off 0.68).
- **Vector ordering** (`tso_vector` against `tso_scalar`, ratios to Rosetta 2), kernels it slows by half or more: `copy_loop` 49.49 against 2.54; `matmul_f64` 1.79 against 0.67; `matmul_f32x4` 4.81 against 1.05; `nbody` 2.94 against 1.00; `fft` 4.23 against 1.45.
- **String-instruction ordering** (`tso_memcpy` against `tso_scalar`), kernels it slows by half or more: `rep_movs` 4.93 against 0.19.
- **Multiblock off** (`tso_on_nomb` against `tso_on`), kernels it slows by half or more: `calls` 5.19 against 1.85.
- **x87** (x86-64 builds only): Rosetta 2 557 ms; `tso_on` 0.72 of Rosetta 2's time; `tso_on_x87rp` 0.06 of Rosetta 2's time.
- **Thread kernels**, all ordering on against Rosetta 2 (in no mean): `threads_lock` 3.71 (launch medians 36.3 to 42.3 ms; Rosetta 2 9.2 to 12.8 ms); `threads_mp` 9.11 (launch medians 23.5 to 25.0 ms; Rosetta 2 2.6 to 2.7 ms).
- **Translation and the code cache**: `tso_on` compiles 86 blocks per launch; 587 with multiblock off; a cold-cache launch compiles 83 and loads 3; a warm-cache launch compiles 0 and loads 80. Launch to first kernel result: `tso_on` 0.064 s, `tso_on_cache_cold` 0.065 s, `tso_on_cache_warm` 0.064 s; whole launch: `tso_on` 8.64 s, `tso_on_cache_cold` 8.62 s, `tso_on_cache_warm` 8.36 s.
- **Clock read** (calibrated by the program, subtracted from the times): native-arm64 6 ns, rosetta 15 ns, tso_on 136 ns.
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
`tso_on_deferred` is `tso_on` with `fexmac`'s `TSODEFERRED` option (round 2): no ordering
until the program creates its second thread, which it does only for its last two (thread)
kernels; see "Candidates".

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
`FEXCoreBench` registers the image's executable segments with `fexmac_memory_map`
in every configuration.

### Results

<!-- bench-results:begin -->
<!-- Generated by Scripts/macos_bench.py; replace by running it again, do not edit by hand. -->

**PROVISIONAL** numbers: other work was loading the machine (other processes used 0.33 cores on average, at most 2.36, of 10; load average 1.9 on average). They show the shape, not final values.

Apple M4, 4 performance + 6 efficiency cores, 16 GB, macOS 27.0.1, Apple clang version 21.0.0 (clang-2100.4.1.1); commit `FEX-2611-286-g3b12faa03`; 2026-11-27T19:20:57-05:00 to 2026-11-27T19:26:51-05:00. 5 launches per build and configuration, interleaved, each with one warm-up and 3 timed runs per kernel. Cells: median of the launch medians in ms [lowest-highest launch median]. Ratios are to Rosetta 2's median (below 1: faster than Rosetta 2).

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
| crc32 | 42.5 [42.5-42.5] | 51.4 [51.4-51.4] | 58.4 [58.1-59.0] | 58.4 [58.2-58.4] | 58.2 [58.2-58.4] | 58.3 [58.1-58.4] | 58.2 [58.1-58.2] | 58.4 [58.1-58.4] | 51.5 [51.5-51.9] | 57.8 [57.8-58.0] | 58.0 [57.8-58.1] |
| hash64 | 19.1 [18.9-19.3] | 21.7 [21.6-22.1] | 21.5 [21.4-24.8] | 21.4 [21.3-21.7] | 21.7 [21.2-21.7] | 21.7 [21.5-21.7] | 21.4 [21.2-21.7] | 21.5 [21.4-21.7] | 21.7 [21.4-21.9] | 21.5 [21.5-21.5] | 21.8 [21.5-21.8] |
| sort | 21.9 [21.8-22.4] | 24.4 [24.3-24.7] | 31.0 [30.1-36.5] | 27.1 [27.1-27.6] | 31.1 [30.2-31.2] | 27.4 [27.1-27.4] | 32.8 [32.7-33.5] | 30.1 [30.0-30.4] | 25.4 [25.2-26.0] | 30.9 [30.9-31.3] | 31.8 [30.8-31.9] |
| interp | 49.7 [49.7-49.9] | 81.4 [75.5-95.1] | 144.2 [143.5-146.1] | 144.0 [142.9-144.3] | 144.3 [144.0-145.4] | 144.0 [143.7-145.1] | 154.4 [153.9-154.8] | 145.1 [144.7-145.9] | 73.3 [68.5-74.0] | 145.1 [144.2-145.9] | 144.5 [144.3-144.7] |
| dfa | 39.8 [39.4-40.3] | 50.4 [49.7-52.9] | 53.5 [52.8-54.9] | 53.6 [53.2-53.7] | 53.6 [53.5-54.1] | 53.6 [52.8-53.9] | 67.9 [67.1-68.1] | 54.0 [52.7-54.5] | 49.9 [49.6-50.3] | 58.1 [56.9-58.7] | 58.0 [57.5-58.8] |
| rep_movs | 15.9 [15.7-16.0] | 76.0 [75.4-76.5] | 369.7 [367.5-372.5] | 14.4 [14.3-14.5] | 14.2 [14.1-14.4] | 374.9 [370.3-382.9] | 374.0 [358.7-383.5] | 378.7 [370.3-386.9] | 15.4 [14.9-15.5] | 367.4 [361.8-371.2] | 367.1 [361.8-376.6] |
| copy_loop | 10.4 [10.3-10.5] | 11.6 [11.5-11.6] | 651.5 [575.5-657.2] | 29.4 [29.3-29.6] | 574.5 [574.3-656.5] | 28.6 [28.4-29.1] | 650.9 [576.8-652.5] | 574.9 [574.3-587.3] | 28.8 [28.7-29.7] | 650.6 [574.5-654.7] | 577.0 [574.5-654.7] |
| ptr_chase | 30.9 [27.8-31.2] | 28.1 [27.9-31.9] | 30.7 [26.7-31.1] | 30.9 [28.3-30.9] | 26.8 [26.7-31.6] | 26.7 [26.7-30.7] | 30.7 [26.6-30.7] | 26.7 [26.7-27.0] | 30.7 [26.6-31.2] | 30.7 [26.8-31.0] | 26.9 [26.6-30.9] |
| matmul_f64 | 21.3 [21.2-21.6] | 28.6 [28.5-30.6] | 55.0 [50.2-56.4] | 19.2 [19.1-19.4] | 51.0 [49.8-55.5] | 19.3 [19.2-19.4] | 57.9 [52.3-59.2] | 50.5 [50.0-50.8] | 19.4 [19.2-19.5] | 54.3 [50.0-55.0] | 50.6 [50.0-54.8] |
| matmul_f32x4 | 10.7 [10.6-14.0] | 10.4 [10.3-10.5] | 49.9 [49.3-50.1] | 10.8 [10.6-10.9] | 49.7 [49.6-50.4] | 10.8 [10.6-10.9] | 50.2 [49.9-50.3] | 49.9 [49.5-50.1] | 10.9 [10.9-11.0] | 49.9 [48.3-50.0] | 49.6 [49.5-50.0] |
| nbody | 9.7 [9.6-9.7] | 9.9 [9.9-9.9] | 29.3 [28.8-29.6] | 9.9 [9.9-9.9] | 29.1 [28.8-29.6] | 9.9 [9.9-10.0] | 29.2 [28.9-29.7] | 29.3 [28.9-29.6] | 9.9 [9.9-9.9] | 29.8 [29.6-30.2] | 29.8 [29.7-29.8] |
| fft | 11.8 [11.7-11.9] | 15.6 [15.5-15.9] | 65.6 [65.2-66.5] | 22.6 [22.3-22.7] | 65.8 [65.3-67.1] | 22.3 [22.2-22.7] | 71.7 [71.4-72.9] | 65.8 [65.4-65.8] | 17.4 [17.3-17.7] | 65.6 [65.5-66.4] | 65.5 [65.4-66.3] |
| muldiv | 39.7 [39.4-40.0] | 192.3 [191.6-194.0] | 39.0 [38.8-39.3] | 39.1 [39.0-39.4] | 39.1 [39.0-39.2] | 39.3 [38.9-39.3] | 39.0 [38.8-39.7] | 39.3 [39.0-39.6] | 38.3 [38.0-38.7] | 39.0 [38.9-39.2] | 39.1 [39.0-39.4] |
| calls | 9.3 [9.1-9.5] | 10.3 [10.3-10.3] | 19.1 [19.0-19.2] | 19.1 [19.1-19.3] | 19.1 [19.1-19.2] | 19.1 [19.1-19.1] | 53.7 [53.5-54.1] | 19.1 [19.1-19.2] | 19.1 [19.0-19.2] | 19.2 [19.1-19.2] | 19.1 [19.1-19.2] |
| deep_recursion | 25.4 [25.1-25.6] | 20.0 [19.7-20.3] | 32.2 [32.1-32.7] | 32.3 [32.2-32.5] | 32.4 [32.2-32.5] | 32.2 [32.2-32.3] | 33.2 [33.0-33.2] | 32.2 [32.2-32.6] | 32.2 [32.2-32.3] | 32.2 [32.2-32.2] | 32.2 [32.2-32.3] |

Ratio to Rosetta 2:

| kernel | native arm64 | tso_on | tso_scalar | tso_vector | tso_memcpy | tso_on_nomb | tso_on_x87rp | tso_off | tso_on_cache_cold | tso_on_cache_warm |
|---|---|---|---|---|---|---|---|---|---|---|
| crc32 | 0.83 | 1.14 | 1.14 | 1.13 | 1.14 | 1.13 | 1.14 | 1.00 | 1.12 | 1.13 |
| hash64 | 0.88 | 0.99 | 0.99 | 1.00 | 1.00 | 0.99 | 0.99 | 1.00 | 0.99 | 1.00 |
| sort | 0.90 | 1.27 | 1.11 | 1.28 | 1.12 | 1.35 | 1.23 | 1.04 | 1.27 | 1.30 |
| interp | 0.61 | 1.77 | 1.77 | 1.77 | 1.77 | 1.90 | 1.78 | 0.90 | 1.78 | 1.78 |
| dfa | 0.79 | 1.06 | 1.06 | 1.06 | 1.06 | 1.35 | 1.07 | 0.99 | 1.15 | 1.15 |
| rep_movs | 0.21 | 4.87 | 0.19 | 0.19 | 4.93 | 4.92 | 4.98 | 0.20 | 4.83 | 4.83 |
| copy_loop | 0.90 | 56.13 | 2.54 | 49.49 | 2.46 | 56.08 | 49.53 | 2.48 | 56.05 | 49.71 |
| ptr_chase | 1.10 | 1.09 | 1.10 | 0.95 | 0.95 | 1.09 | 0.95 | 1.10 | 1.09 | 0.96 |
| matmul_f64 | 0.74 | 1.93 | 0.67 | 1.79 | 0.68 | 2.03 | 1.77 | 0.68 | 1.90 | 1.77 |
| matmul_f32x4 | 1.03 | 4.82 | 1.05 | 4.81 | 1.05 | 4.85 | 4.82 | 1.06 | 4.82 | 4.79 |
| nbody | 0.98 | 2.95 | 1.00 | 2.94 | 1.00 | 2.94 | 2.96 | 1.00 | 3.01 | 3.00 |
| fft | 0.76 | 4.22 | 1.45 | 4.23 | 1.43 | 4.61 | 4.23 | 1.12 | 4.22 | 4.21 |
| muldiv | 0.21 | 0.20 | 0.20 | 0.20 | 0.20 | 0.20 | 0.20 | 0.20 | 0.20 | 0.20 |
| calls | 0.90 | 1.85 | 1.85 | 1.85 | 1.85 | 5.19 | 1.85 | 1.85 | 1.85 | 1.85 |
| deep_recursion | 1.27 | 1.61 | 1.62 | 1.62 | 1.61 | 1.66 | 1.61 | 1.61 | 1.61 | 1.61 |
| **geometric mean** | **0.73** (15 kernels) | **2.07** (15 kernels) | **0.98** (15 kernels) | **1.63** (15 kernels) | **1.21** (15 kernels) | **2.30** (15 kernels) | **2.03** (15 kernels) | **0.91** (15 kernels) | **2.08** (15 kernels) | **2.04** (15 kernels) |

#### x87 (x86-64 builds only; native arm64 has no 80-bit type)

| kernel | Rosetta 2 | tso_on | tso_scalar | tso_vector | tso_memcpy | tso_on_nomb | tso_on_x87rp | tso_off | tso_on_cache_cold | tso_on_cache_warm |
|---|---|---|---|---|---|---|---|---|---|---|
| x87 | 557.5 [556.2-563.9] | 401.6 [400.7-402.1] | 398.7 [398.1-399.6] | 400.9 [400.7-401.9] | 398.0 [397.6-398.9] | 400.8 [400.6-402.3] | 32.8 [32.8-33.3] | 399.2 [398.1-399.4] | 402.1 [400.6-403.1] | 401.5 [400.2-401.8] |

Ratio to Rosetta 2:

| kernel | tso_on | tso_scalar | tso_vector | tso_memcpy | tso_on_nomb | tso_on_x87rp | tso_off | tso_on_cache_cold | tso_on_cache_warm |
|---|---|---|---|---|---|---|---|---|---|
| x87 | 0.72 | 0.72 | 0.72 | 0.71 | 0.72 | 0.06 | 0.72 | 0.72 | 0.72 |

#### Thread kernels (in no mean; spread is large)

| kernel | native arm64 | Rosetta 2 | tso_on | tso_scalar | tso_vector | tso_memcpy | tso_on_nomb | tso_on_x87rp | tso_off | tso_on_cache_cold | tso_on_cache_warm |
|---|---|---|---|---|---|---|---|---|---|---|---|
| threads_lock | 29.3 [28.7-31.3] | 10.2 [9.2-12.8] | 37.7 [36.3-42.3] | 42.6 [41.8-43.0] | 37.7 [37.3-42.5] | 41.6 [34.0-42.0] | 45.6 [40.8-47.1] | 43.6 [37.6-45.1] | 24.4 [22.3-28.7] | 37.7 [35.7-38.0] | 40.0 [36.4-45.3] |
| threads_mp | 43.7 [42.2-48.6] | 2.6 [2.6-2.7] | 24.1 [23.5-25.0] | 24.0 [22.4-24.5] | 24.1 [22.8-25.7] | 24.6 [23.1-25.0] | 26.2 [24.1-27.0] | 24.3 [24.2-24.7] | 1.7 [1.7-1.7] wrong in 4 | 24.4 [23.3-24.9] | 24.3 [22.9-25.8] |

#### Time to first kernel result (`--only calls --quick`), seconds

| | native arm64 | Rosetta 2 | tso_on |
|---|---|---|---|
| first launch of the fresh build | 0.125 | 0.177 | 0.006 |
| later launches, median [range] | 0.002 [0.002-0.003] | 0.007 [0.007-0.007] | 0.005 [0.005-0.005] |

#### Translation and the code cache (per launch of the full program; median [range])

Blocks compiled and loaded come from `fexmac_context_get_counters`. First result: launch to the end of the first kernel's warm-up. Launch: the whole run, all kernels, every repetition.

| configuration | blocks compiled | blocks loaded from the cache | first result, s | launch, s |
|---|---|---|---|---|
| tso_on | 86 [85-87] | 0 [0-0] | 0.064 [0.064-0.065] | 8.64 [8.30-8.74] |
| tso_scalar | 85 [84-86] | 0 [0-0] | 0.065 [0.064-0.065] | 4.15 [4.13-4.16] |
| tso_vector | 86 [86-86] | 0 [0-0] | 0.065 [0.064-0.065] | 6.88 [6.86-7.26] |
| tso_memcpy | 84 [83-86] | 0 [0-0] | 0.065 [0.064-0.065] | 5.56 [5.55-5.56] |
| tso_on_nomb | 587 [586-592] | 0 [0-0] | 0.064 [0.064-0.065] | 8.94 [8.61-8.95] |
| tso_on_x87rp | 85 [84-86] | 0 [0-0] | 0.065 [0.064-0.065] | 6.86 [6.83-7.01] |
| tso_off | 85 [84-87] | 0 [0-0] | 0.058 [0.057-0.058] | 3.63 [3.62-3.65] |
| tso_on_cache_cold | 83 [83-84] | 3 [2-3] | 0.065 [0.064-0.065] | 8.62 [8.31-8.67] |
| tso_on_cache_warm | 0 [0-0] | 80 [80-82] | 0.064 [0.064-0.064] | 8.36 [8.31-8.67] |

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
  them (the core does not support them yet). The M4 measured here does not report the
  feature (`sysctl hw.optional.arm.FEAT_LRCPC3`: unknown oid, macOS 27.0.1); other Apple
  chips were not checked. The port's macOS feature detection asks for it and records it
  without using it (`fexmac_hook_test host-features-rcpc` prints the value), so a host
  that has it would be noticed.
- Cheaper vector ordering on hosts without them (round 2, task E2): tried, behind
  options that are off by default (`docs/macOS-port-status.md`, "Cheaper vector
  ordering"); `Scripts/macos_bench.py --configs e2` measures them. The generated table
  above does not include them; the provisional figures, taken while other work loaded the
  machine, are in the round 2 report (task E2), and the final ones are to be taken on an
  idle machine with
  `python3 Scripts/macos_bench.py --loader build/Bin/FEXCoreBench --out bench-e2 --configs e2`
  (and with `--qos background --out bench-e2-ecores` for the efficiency cores). In short:
  - `VECTORTSOSPLIT=3` (`tso_on_fp64`): vector-register accesses of 8 bytes or less as one
    acquire load or release store, no barrier. Every access stays one access. It lowers
    the headline and clearly helps `matmul_f64`, `nbody` and `fft`; `copy_loop` (16-byte
    `movdqa`) is unchanged.
  - `TSOBARRIERMERGE=1` (`tso_on_merge`): lowers the headline about as much as
    `VECTORTSOSPLIT=3`, mostly through `matmul_f32x4`, `nbody` and `fft`; both together
    (`tso_on_fp64_merge`) a little more. `copy_loop` is unchanged: in its loop (`movdqa`
    load, `pxor`, `movdqa` load, `pxor`, `movdqa` store, `movdqa` store) only the second
    load's `dmb ishld` is followed by a store's `dmb ish`, so three of the four barriers
    remain, and the 16-byte accesses keep theirs.
  - For the record, 16-byte accesses as two 8-byte halves (`VECTORTSOSPLIT=15`) gave the
    largest gain (`copy_loop` most of all) but made `matmul_f32x4` slower, and is dropped:
    it breaks the 16-byte atomicity x86 guarantees for aligned accesses on processors with
    AVX (Intel SDM vol. 3A 11.1.1, AMD APM vol. 2 7.3.2), and `TEAR-sse`, `TEAR-sse-st` and
    `TEAR-sse-ld` see torn values under it. The production library now refuses those bits
    (testing build only), so the configuration is no longer in `--configs e2`; its figures
    are in the round 2 report.
  - `tso_on_r2`: the round 2 set together (`VECTORTSOSPLIT=3`, `TSOBARRIERMERGE=1`,
    `MEMCPYSETTSOWIDE=1`).
  - Not done: merging barriers between consecutive loads or consecutive stores (each load
    needs its own barrier after it, each store one before it; nothing there is
    redundant).
- Treat provably thread-private memory (beyond the stack, which is already exempt) as
  needing no ordering. Round 2 (experiment 6) looked at an embedder-declared variant, a
  `fexmac_memory_map` flag for regions only one guest thread touches, and did not
  implement it: ordering is chosen per instruction at translation time but the address
  is known only at run time, so the flag needs a check per access or a guarded per-block
  speculation. By the round 2 E1 probe a range check (compare and branch, or a dependent
  table load) costs about what it saves on scalar accesses; only vector accesses (load
  plus `dmb ishld`, `dmb ish` plus store) and string copies could gain. A wrong
  declaration silently removes the ordering guarantee, and no caller in this project
  knows of such regions (a game's thread-local memory is
  allocated inside the guest). Written up in `macOS-embedding-interface.md` ("Not done:
  memory the embedder declares private to one thread").
- **No ordering while the guest has one thread (round 2, experiment 5; done as an
  option, `TSODEFERRED`, default off).** `fexmac` translates without ordering emulation
  until the embedder creates a second guest thread or calls
  `fexmac_context_mark_memory_shared`, then switches it on for good: every translation is
  discarded and no thread runs guest code until each thread that was in code made
  without ordering has left it (`macOS-embedding-interface.md`, "Ordering deferred while
  single-threaded", has the handshake and the argument). Configuration `tso_on_deferred`
  (all three switches on, plus `TSODEFERRED=1`): this program's single-threaded kernels
  all run before its first thread kernel creates the second thread, so they run at
  ordering-off speed, and the thread kernels then run with ordering on and are right.
  A provisional measurement under heavy load (the round 2 report and
  `macOS-port-status.md`, "x86 memory ordering between threads", give the figures; this
  document carries only generated ones) puts `tso_on_deferred` at `tso_off`'s speed, within that
  load's noise, on every single-threaded kernel, `copy_loop`, `rep_movs`, `matmul_f32x4`
  and `fft` included, with every checksum right. The switch costs a discard of every translation
  (`FEXCoreOrderingSwitchTest cost`: milliseconds for ten thousand translated blocks,
  tens of milliseconds for a hundred thousand) and a translation of each block again on
  first use. Exact, but **little help for a game**, which is multi-threaded from the
  start: it helps single-threaded programs and phases (tools, tests, loaders,
  start-up), not the like-for-like figure. The measurement for an idle machine:
  `python3 Scripts/macos_bench.py --loader build/Bin/FEXCoreBench --out bench-deferred
  --configs tso_on,tso_off,tso_on_deferred` (writes `bench-deferred/results.md`; do not
  pass `--write-docs` unless the owner accepts the figures).
- `rep movs`/`rep stos` with string ordering (round 2, E2). Intel lets the stores of one
  fast string operation be seen out of order, AMD documents no such exception and Intel
  none for its loads (citations in `docs/macOS-port-status.md`, "String instructions in
  wider ordered units"), so plain stores between two barriers were not implemented.
  Implemented instead, behind `MEMCPYSETTSOWIDE` (off by default): the same acquire loads
  and release stores, eight bytes at a time. The configurations `tso_on_wide` and
  `tso_memcpy_wide` of `Scripts/macos_bench.py` (not in the default set) measure it, and
  `Scripts/macos_string_bench.py` times `rep movsb`, `movsq`, `stosb` and `stosq` at four
  sizes and in the kernel's shapes (skewed source, misaligned elements, overlapping
  copies); results are in the round 2 report, not in the generated block, which still
  holds the measurement taken for the first release.

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

The string-instruction microbenchmark (round 2), at the default QoS and on the
efficiency cores:

```
python3 Scripts/macos_string_bench.py --loader build/Bin/FEXCoreBench --out string-bench --launches 5
python3 Scripts/macos_string_bench.py --loader build/Bin/FEXCoreBench --out string-bench-e --launches 5 --background
```

`--configs`, `--only`, `--launches`, `--reps` narrow or widen a run. `FEXCoreBench`
alone:

```
FEXCoreBench [--option NAME=VALUE]... [--code-cache DIR] [--quick] [--literal [--expect-kernels N]] [--reps N] [--only k1,k2] bench-out/bench-guest.macho
```
