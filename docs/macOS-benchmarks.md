# macOS benchmarks: a guest program under this port, Rosetta 2 and native

This is the evidence for exit criteria 7 ("a real guest program runs") and 8 ("speed is
measured") of `docs/macOS-v1.0-plan.md`, and the method behind it.

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
only the target differs (`x86_64-apple-macos11` or `arm64-apple-macos11`). The builds
differ only in the small platform file behind `bench.h`: `platform_guest.c` (one
`syscall` per service) or `platform_native.c` (libc).

The guest's whole host surface is a private system call ABI answered by the loader's
fexmac system call callback: exit, write bytes to standard output, read the clock
(`CLOCK_UPTIME_RAW`, as the native builds use), start a guest thread, wait for it, end a
guest thread. These are not Linux or macOS numbers (`0xBE00`-`0xBE05`). So criterion 7's
"no system calls beyond exit" is met in its intended sense, no OS dependency at all, with
result reporting, a clock and thread creation added; the threads are guest threads the
loader creates with `fexmac_thread_create`, one host thread each.

The image format: `ld64` writes only Mach-O, so the guest is linked as a static,
non-position-independent x86-64 Mach-O at a fixed address (`-static -no_pie
-image_base 0x20000000000`, 2TB: above macOS's 4GB page zero and clear of its reserved
ranges). It has no imports and no relocations; `FEXCoreBench` maps its segments at their
link addresses (refusing if the range is taken: macOS has no `MAP_FIXED_NOREPLACE`, so it
asks with a hint and checks), copies them and starts at the `LC_UNIXTHREAD` entry. The
guest never sees macOS. No tool beyond the command line tools is needed.

### Kernels

| Kernel | Stresses | Full size |
|---|---|---|
| `crc32` | table-driven CRC-32: byte loads, table lookups | 24 passes over 1 MiB |
| `hash64` | 64x64->128 multiply, shifts | 192 passes over 1 MiB |
| `sort` | quicksort on 64-bit keys: compares, branches, swaps | 3 x 200,000 keys |
| `interp` | register-machine interpreter: `switch` jump table (indirect jump) and calls through a function-pointer table (indirect call) | 6 million guest instructions |
| `dfa` | tokenizer state machine, data-dependent branches | 40 passes over 512 KiB |
| `rep_movs` | `rep stosb`, misaligned `rep movsb`, `rep movsq` (arm64: plain loops) | 200 x 3 MiB |
| `copy_loop` | vectorised copy-and-xor (SSE2 / NEON) | 600 x 1 MiB |
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

### Checksums and floating point

Every kernel returns a 64-bit checksum of all it computed. The expected values for the
quick and full sizes are recorded in `kernels.c` and each run checks itself (exit status
= number of wrong checksums); `Scripts/macos_bench.py` also compares the builds with each
other.

Floating point is compared **bit-exactly**, not within a tolerance. That holds because
the kernels use only operations IEEE 754 requires to be correctly rounded and that SSE2
and NEON both implement so (+, -, *, /, sqrt, conversions), no library functions, and
`-ffp-contract=off` so that no build fuses a multiply and an add (clang does by default
on arm64). Reductions are written in a fixed order and nothing is built with fast-math.

The exception is `x87`: `long double` is a different type on arm64 (64-bit double), so
that checksum is compared between the two x86-64 builds only, and arm64 has its own
recorded value. Under FEX with `X87REDUCEDPRECISION=1` it differs, as it should.

## Results

### Correctness (criterion 7)

Every kernel gives the same checksum in the guest build under this port as under
Rosetta 2 and natively (x87: as under Rosetta 2), in quick and full sizes, in every
configuration below except the two differences that are expected:

| Configuration | Checksums |
|---|---|
| `MAXINST=500 MULTIBLOCK=1`, TSO on (`TSOENABLED`, `VECTORTSOENABLED`, `MEMCPYSETTSOENABLED` = 1) | all 18 right |
| the same, TSO off (all three 0) | 17 right; `threads_mp` wrong and different from run to run: without the ordering emulation the consumer sees ring slots before their data, which is what that kernel tests |
| TSO for scalar accesses only (`VECTORTSOENABLED=0`, `MEMCPYSETTSOENABLED=0`: FEX's defaults) | all 18 right |
| TSO on, `MULTIBLOCK=0` | all 18 right |
| TSO on, `X87REDUCEDPRECISION=1` | 17 right; `x87` differs (64-bit precision), as expected |
| Quick size also: `MAXINST=1 MULTIBLOCK=0` with TSO on and off; `MAXINST=5000` with `HALFBARRIERTSOENABLED=0`; `SMCCHECKS=full`; `HOSTFEATURES=disableatomics` | all right (TSO off: `threads_mp` wrong in most runs, as above; it can come out right by chance, which the script allows) |

Also as built and signed for the hardened runtime (`--hardened`). No translator or port
bug was found: there is no checksum difference to reduce.

### Speed (criterion 8)

Machine: Mac mini, Apple M4 (4 performance and 6 efficiency cores), 16 GB, macOS 27.0.1,
Apple clang 2100.4.1.1. Date: 27 November 2026, 07:19 EST. Every thread at the
user-interactive quality of service (`pthread_set_qos_class_self_np`), which keeps it on
the performance cores when they are free. Load: other workers were building and testing
on the machine throughout; during this run the other processes used on average 2.0 cores
(at most 5.6) of 10, sampled every 2 s (load average about 9, of which 4 are processes
stuck in Rosetta, see below, that use no CPU). Earlier runs under heavier load (5 to 10
cores busy) gave the same picture with more spread; this is the quietest of three.

One warm-up run, then 9 timed runs per kernel; median in milliseconds, minimum in
brackets, and the ratio of the median to Rosetta's (below 1: faster than Rosetta). FEX
configurations are `MAXINST=500 MULTIBLOCK=1` plus: `tso_off` all three TSO switches 0;
`tso_on` all three 1; `tso_scalar` only `TSOENABLED=1`; `tso_on_nomb` as `tso_on` with
`MULTIBLOCK=0`; `tso_on_x87rp` as `tso_on` with `X87REDUCEDPRECISION=1`.

| Kernel | native arm64 | Rosetta 2 | FEX tso_off | FEX tso_on | FEX tso_scalar | FEX tso_on_nomb | FEX tso_on_x87rp |
|---|---|---|---|---|---|---|---|
| crc32 | 47.9 (47.7) | 57.7 (57.7) | 57.9 (57.6) 1.00 | 66.6 (66.4) 1.15 | 66.7 (66.4) 1.16 | 66.6 (66.3) 1.15 | 66.9 (66.7) 1.16 |
| hash64 | 20.5 (20.5) | 23.7 (23.6) | 22.8 (22.7) 0.96 | 22.8 (22.7) 0.96 | 22.8 (22.6) 0.96 | 22.7 (22.5) 0.96 | 22.9 (22.8) 0.97 |
| sort | 25.0 (25.0) | 27.4 (26.7) | 27.1 (27.0) 0.99 | 33.5 (33.0) 1.22 | 28.6 (28.5) 1.04 | 36.7 (36.0) 1.34 | 34.0 (33.4) 1.24 |
| interp | 50.8 (50.5) | 78.4 (78.3) | 74.8 (72.8) 0.95 | 153.3 (152.9) 1.96 | 153.3 (153.0) 1.96 | 164.6 (163.8) 2.10 | 153.6 (153.0) 1.96 |
| dfa | 40.4 (39.7) | 54.5 (54.1) | 50.5 (49.1) 0.93 | 54.9 (53.4) 1.01 | 55.9 (55.7) 1.03 | 69.1 (68.5) 1.27 | 54.4 (52.6) 1.00 |
| rep_movs | 6.5 (6.4) | 114.5 (112.9) | 7.6 (6.2) 0.07 | 134.4 (133.9) 1.17 | 6.5 (6.3) 0.06 | 135.3 (134.9) 1.18 | 134.4 (134.2) 1.17 |
| copy_loop | 8.0 (7.8) | 12.3 (12.2) | 7.8 (7.7) 0.63 | 1210.1 (1131.0) 98.1 | 9.1 (7.7) 0.74 | 1143.7 (1003.5) 92.8 | 1187.5 (1100.8) 96.3 |
| ptr_chase | 31.6 (30.1) | 32.5 (31.5) | 32.6 (31.0) 1.00 | 32.6 (30.1) 1.00 | 32.8 (30.5) 1.01 | 32.2 (30.1) 0.99 | 32.8 (30.7) 1.01 |
| matmul_f64 | 22.2 (22.1) | 34.8 (33.0) | 17.6 (17.4) 0.51 | 64.4 (63.2) 1.85 | 17.5 (17.4) 0.50 | 67.1 (64.4) 1.93 | 64.9 (59.6) 1.87 |
| matmul_f32x4 | 10.9 (10.8) | 10.1 (10.0) | 10.7 (10.6) 1.05 | 52.9 (52.6) 5.22 | 10.6 (10.6) 1.05 | 53.3 (53.0) 5.26 | 52.7 (52.4) 5.19 |
| nbody | 10.1 (10.1) | 10.2 (10.2) | 10.1 (10.1) 0.99 | 32.8 (32.7) 3.21 | 10.1 (10.1) 0.99 | 32.7 (32.6) 3.20 | 32.8 (32.7) 3.21 |
| fft | 12.0 (12.0) | 16.0 (15.8) | 15.1 (14.9) 0.94 | 71.1 (70.8) 4.45 | 21.3 (21.1) 1.33 | 74.5 (74.2) 4.66 | 70.9 (70.7) 4.44 |
| x87 | 3.6 (3.6) [double] | 577.7 (575.3) | 411.5 (410.5) 0.71 | 416.2 (415.3) 0.72 | 414.7 (413.9) 0.72 | 417.8 (416.1) 0.72 | 30.3 (30.2) 0.05 |
| muldiv | 14.6 (14.6) | 16.5 (16.4) | 14.6 (14.6) 0.89 | 14.6 (14.6) 0.89 | 14.7 (14.6) 0.89 | 15.5 (15.4) 0.94 | 14.7 (14.6) 0.89 |
| calls | 9.9 (9.7) | 10.6 (10.6) | 15.2 (13.0) 1.43 | 13.4 (13.1) 1.26 | 15.2 (14.3) 1.44 | 61.8 (60.9) 5.84 | 13.9 (13.1) 1.31 |
| deep_recursion | 28.3 (28.2) | 40.6 (39.2) | 33.8 (33.8) 0.83 | 33.6 (33.5) 0.83 | 34.0 (33.8) 0.84 | 44.7 (44.3) 1.10 | 33.6 (33.5) 0.83 |
| threads_lock | 40.8 (36.6) | 33.4 (22.0) | 39.2 (32.1) 1.18 | 49.3 (46.3) 1.48 | 34.3 (26.9) 1.03 | 58.2 (47.6) 1.74 | 46.6 (43.1) 1.40 |
| threads_mp | 56.2 (37.5) | 11.6 (6.2) | 3.5 (2.6) 0.30 [wrong] | 31.9 (25.8) 2.76 | 27.4 (23.3) 2.37 | 30.3 (20.0) 2.62 | 33.5 (15.7) 2.90 |
| **geometric mean / Rosetta** | 0.61 | 1 | 0.74 | 1.91 | 0.89 | 2.19 | 1.66 |

Measured on the branch base (`macos/v1` at `98a72c187`). After merging `macos/v1` at
`c4cd33855` (package E follow-ups), the TSO-sensitive kernels were timed again (3 runs,
TSO on and off: `interp`, `rep_movs`, `copy_loop`, `matmul_f32x4`, `fft`, `x87`) and
are within 5% of the table, except `copy_loop` with TSO on at 1.4 s (machine idle, see
below); the checksums are unchanged.

`x87` on arm64 is double arithmetic, not comparable; `threads_mp` with TSO off computes a
wrong answer, so its time means nothing. The geometric means include every kernel.

The barrier-bound `copy_loop` with TSO on is the one number that depends strongly on the
state of the machine: 1,000 to 1,300 ms per run with the other cores idle, but 100 to
750 ms with six other cores spinning (checked directly: two runs alone, two with six
busy loops), and medians of 147 to 400 ms (minimum 70 ms) in the earlier, busier runs. Every other kernel varied by
a few per cent between runs. A loop that waits on a barrier after every load is bound by
memory latency, and the SoC's fabric apparently runs faster when the machine is busy;
the idle figure is the pessimistic one and is what the table shows.

### Translation overhead

| | native | Rosetta 2 | FEX tso_on |
|---|---|---|---|
| First launch of a fresh build, launch to first output (`START`) | 35 ms | 131 ms (ahead-of-time translation) | 4 ms |
| Later launch, launch to first output | 2 ms | 8 ms | 3 ms |
| Launch to first result (after crc32's setup and warm-up) | 50 ms | 66 ms | 70 ms |

The warm-up run of each kernel (which under FEX includes translating it) is within the
spread of the timed runs for every kernel (the script prints them): translating this
program costs far less than running it. FEX compiles the whole program in **79 blocks**
(4,894 guest instructions, 10,520 host instructions) with multiblock on, and 461 blocks
with it off (quick run, all kernels, TSO on); a run of one kernel alone compiles 13 to 30
blocks, the driver's included. The core exposes no translation counters through `fexmac` yet
(package F adds them); these counts come from the core's own `Disassemble=stats` output
in a local diagnostic build (see "How the code was examined").

## Where this port is far from Rosetta 2, and why

Without memory-ordering emulation FEX is at or ahead of Rosetta 2 on almost everything
(geometric mean 0.74; `rep_movs` 15 times faster, `matmul_f64` and `x87` faster). With
all three TSO switches on it is 1.9 times slower overall, and nearly all of that is TSO.
Rosetta 2 runs x86 code with the M-series hardware TSO mode, which macOS does not let an
ordinary process switch on; FEX has to emulate x86 ordering in software, with ordering
instructions or barriers on guest memory accesses. The worst kernels:

1. **`copy_loop`: 98x Rosetta (with `VECTORTSOENABLED=1`).** The guest loop is SSE2
   `movdqa` loads and stores. With vector TSO each 128-bit load is followed by
   `dmb ishld` and each store preceded by `dmb ish` (disassembly of the block at
   `run_copy_loop`):

   ```
   ldr q17, [x20]        ; movdqa xmm1, [r8+r10*8]
   dmb ishld
   eor v17.16b, v17.16b, v16.16b
   ...
   dmb ish
   str q17, [x20]        ; movdqa [r9+r10*8], xmm1
   ```

   Scalar accesses use the cheaper `ldapr`/`stlr` forms, but the M4 has no vector RCpc
   loads (`FEAT_LRCPC3` is absent: `hw.optional.arm.FEAT_LRCPC3` does not exist), so
   vectors get full barriers. Without vector TSO (`tso_scalar`) the kernel runs at 0.74x
   Rosetta. The same cost, smaller, is in every SSE kernel: `matmul_f32x4` 5.2x, `fft`
   4.5x, `nbody` 3.2x, `matmul_f64` 1.9x (scalar `movsd` is a vector access in FEX, so
   it gets the barriers too), all back to 0.5x to 1.3x with vector TSO off.

2. **`interp`: 2x Rosetta (scalar TSO).** Every guest load that is not to the stack
   becomes `ldapur`/`ldapr` and every store `stlr`, including the interpreter's loads of
   its instruction stream and its register array:

   ```
   ldapurb w10, [x4]          ; movzbl (%rax), %esi    (opcode)
   ldapurb w16, [x4, #1]      ; movzbl 0x1(%rax), %r12d
   ldapur  w20, [x4, #4]      ; movslq 0x4(%rax), %rcx
   ```

   With TSO off it runs at 0.95x Rosetta. Indirect branches are not the problem here:
   the jump-table `jmp *%rax` becomes an inline lookup in the core's L1 code cache
   (`ldp` from the thread's table, compare, `br`), six instructions on a hit. `sort`
   (1.22x) and `crc32` (1.15x) pay the same, less, because fewer of their accesses are
   stores.

3. **`rep_movs`: 1.17x Rosetta (with `MEMCPYSETTSOENABLED=1`), 18x slower than without
   it.** With the switch on, `rep movsb` becomes a loop of one `ldaprb` and one `stlrb` per
   byte, and `rep movsq` one `ldapr`/`stlr` per quadword; with it off, 64 bytes per
   iteration with `ldp q`/`stp q`. Rosetta 2 is slow here too (17x native).

4. **`calls` without multiblock: 5.8x Rosetta.** Each `call` and `ret` ends a block and
   goes through the dispatcher. With multiblock on (the measured default) it is 1.26x.

5. **Threads (`threads_lock` 1.5x, `threads_mp` 2.8x).** Both are dominated by cache-line
   traffic between cores and vary from run to run (minimum and median differ by up to
   2x). Native arm64 is itself slower than Rosetta 2 here, which is not explained. Not
   analysed further: numbers this noisy on a shared machine are not a basis for
   conclusions.

**x87** is slow everywhere (Rosetta 2 160 times native double arithmetic, FEX 115
times): both emulate 80-bit arithmetic in software. FEX is 1.4 times faster than Rosetta
2 at full precision, and 19 times faster than Rosetta with `X87REDUCEDPRECISION=1`, at
the cost of a different answer.

### Candidates (recorded, not done in this package)

- Vector TSO: drop a `dmb ishld` that is followed by a `dmb ish` with no memory access
  between them (`copy_loop`'s block has this pattern), and merge barriers between consecutive vector accesses with no intervening memory access where
  x86 ordering allows it; on a CPU with `FEAT_LRCPC3`, use RCpc vector loads.
- Treat provably thread-private memory (beyond the stack, which is already exempt) as
  non-TSO.
- `rep movs`/`rep stos` with `MEMCPYSETTSOENABLED`: x86 makes no promise about the
  order of the individual stores of one string instruction; copying in larger ordered
  units, or in plain wide stores bracketed by barriers, would keep the guarantee that
  matters (ordering against other instructions) at a fraction of the cost.
- Embedder guidance: for single-threaded guests, `TSOENABLED=0` is safe and is the
  fastest configuration (0.74x Rosetta); for multi-threaded guests, `VECTORTSOENABLED` is
  the switch with the largest cost, and FEX's defaults leave it off.
- Translation counters through `fexmac` (package F), so that blocks compiled can be
  reported without a diagnostic build.

## How the code was examined

`fexmac` refuses the core's diagnostic options (`DUMPIR`, `DISASSEMBLE` and the like are
not in its list), and the disassembler needs a build with `-DENABLE_VIXL_DISASSEMBLER=ON`.
For this analysis only, a separate build (`build-dis`, not committed, not part of any
test) added `DISASSEMBLE` to `Options.cpp` and ran `FEXCoreBench` with
`--option DISASSEMBLE=6` (blocks and statistics) and `SILENTLOG=0`; the core then writes
each block's host code to standard error. Two things found on the way:

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
  Rosetta 2 had been installed on this Mac the same morning. `Scripts/macos_bench.py`
  therefore signs the x86-64 build before running it (arm64 programs are always signed
  by the linker).
- The first launch of a new x86-64 program costs about 130 ms of ahead-of-time
  translation; later launches 8 ms.

## How to run it

```
ninja -j4 -C build FEXCoreBench
python3 Scripts/macos_bench.py --loader build/Bin/FEXCoreBench --out bench-out --reps 9
python3 Scripts/macos_bench.py --loader build/Bin/FEXCoreBench --out bench-quick --quick [--hardened]
```

The script builds the three programs into `--out`, runs native arm64, Rosetta 2 (skipped
if not installed) and each FEX configuration (`--configs`, `--only` to narrow), prints
the tables above and writes `results.json` with every individual time, the commands and
the machine's load. `--quick` (small sizes, checksums only; three FEX configurations) is
what the Mac workflow runs, as built and with `--hardened`; it asserts no timing.
`FEXCoreBench` alone:

```
FEXCoreBench [--option NAME=VALUE]... [--quick] [--reps N] [--only k1,k2] bench-out/bench-guest.macho
```
