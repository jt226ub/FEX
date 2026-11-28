# macOS port: plan to 1.0

"1.0" here means the translator core is a dependable library on Apple silicon Macs: proven
on real hardware, usable through a small stable interface, with its known gaps closed or
written down as out of scope. It does not mean any game runs. Running games needs a caller
(such as AnyPS5) and a graphics path, which are separate work.

This definition was proposed by the session that built the port. The owner may change it.

## State at the start of this plan (27 November 2026)

Branch `macos/runtime-fixes`. On GitHub's hosted Mac, which is a virtual machine: the
smoke test and multi-thread test pass, and 2,253 of 2,291 instruction tests pass in four
modes with no unexpected failures. Nothing has run on a physical Mac. See
`docs/macOS-port-status.md`.

## Exit criteria for 1.0

Every item needs evidence from a physical Apple silicon Mac, recorded in the status
document.

| # | Criterion | How it is shown |
|---|---|---|
| 1 | All existing tests pass on real hardware, as built and signed for the hardened runtime with only the JIT entitlement | Mac test job or handoff steps, zero unexpected failures |
| 2 | Write protection is proven to be enforced and respected | `JITWriteProbe` shows enforcement; the branch without the fixes fails; the fixed branch passes |
| 3 | A C embedding interface exists and the test tools use it | Header, library, tests; no tool reaches into core C++ types for thread setup or fault handling |
| 4 | Fault handling is complete for an embedder | Tests for: misaligned atomics, scratch-buffer restart, self-modifying code by page protection, stopping a thread from another thread, a guest fault delivered with correct guest state |
| 5 | x86 memory ordering holds between threads when emulation is on | A multi-thread ordering test that fails with emulation off and passes with it on |
| 6 | Cached translations load from disk | Second run of the instruction tests compiles nothing and gives identical results; corrupt and mismatched caches are rejected |
| 7 | A real guest program runs | A freestanding x86-64 benchmark with no system calls beyond exit runs to the correct result |
| 8 | Speed is measured | Benchmark numbers recorded for this port and, where the same guest code can run under it, for Rosetta 2 |
| 9 | The port tracks upstream | Merged with current upstream FEX with the full suite passing; the sync script documented |
| 10 | Tests run on real hardware automatically | A self-hosted runner job, on a private repository only |
| 11 | Documents are current and every expected failure is explained | Status document reviewed; `Known_Failures_macOS` has no "believed" entries left unverified |

## Out of scope for 1.0

- 32-bit guest code. macOS reserves the first 4GB of every 64-bit process.
- FEX's Linux process emulation, FEXServer and thunks.
- Integration with AnyPS5 or any other caller.
- Folding the port into the umbrella repository. That happens after 1.0, as a subtree.
- iOS and Android.
- Anything submitted to FEX-Emu/FEX.

## Work packages

Each is sized for one worker. "Needs" lists packages that must land first.

| ID | Package | Needs | Criteria |
|---|---|---|---|
| A | **Hardware round.** Run every existing test on the physical Mac, as built and hardened. Fix whatever real hardware exposes in executable-memory handling. | none | 1, 2 |
| B | **Verify expected failures.** Read each of the 16 "believed" entries in `Known_Failures_macOS`, confirm or correct the stated cause, and identify the unlisted eighteenth failure. Where a test can be made to run by a better relocation, do that. | A | 11 |
| C | **Embedding interface.** Implement the C interface sketched in `docs/macOS-embedding-interface.md` as a library; move thread setup, descriptor tables, fault handlers and invalidation behind it; port the three test tools to it. | A | 3 |
| D | **Fault handling.** Add to the library: self-modifying code detection by page protection, stopping a thread through the interrupt fault page, and guest fault delivery with reconstructed guest state. A test for each. | C | 4 |
| E | **Memory ordering test.** Write a multi-thread test of x86 ordering guarantees (store buffering and message passing patterns) that distinguishes emulation on from off, and fix what it finds. | A | 5 |
| F | **Code cache.** Implement loading by copy into `MAP_JIT` memory as designed in `docs/macOS-code-cache-design.md`, with the keying and rejection rules, and the three tests listed there. | A | 6 |
| G | **Guest program and benchmark.** Build a freestanding x86-64 benchmark, run it through the interface, record speed here and under Rosetta 2. | C | 7, 8 |
| H | **Upstream sync.** Run `Scripts/macos_sync_upstream.sh`, resolve conflicts keeping the macOS changes, and get the full suite passing on the result. | A | 9 |
| I | **Hardware test job.** Make the self-hosted runner workflow in the umbrella repository reliable and extend it to every new test. | A | 10 |
| J | **Guard page and scratch-buffer review.** The shared code buffer has no guard page on macOS and the fault handler infers the scratch buffer's guard range. Decide the correct macOS design for both and test the edges. | A | 4 |

Order: A first and alone. Then B, C, E, F, H, I, J can run side by side. D and G follow C.

## Rules for every package

- Work is integrated on `macos/v1`, created from `macos/runtime-fixes` when this plan
  starts. Branch each package from it as `macos/wp-<id>-<short-name>`. One package, one
  branch. Do not commit to `main`.
- Use the method macOS provides. Keep a workaround only when none exists, with the reason
  in a comment.
- Every behaviour change comes with a test that runs on Apple silicon, and the Linux
  core-only build must still pass.
- Update `docs/macOS-port-status.md` in the same change.
- Nothing goes to FEX-Emu/FEX. Any pull request names `jt226ub/FEX` as its base.
- Keep upstream's licence and copyright notices.

## Result (27 November 2026)

All ten work packages are on `macos/v1` and, from the merge for the owner's review, on
`main`. The last full check ran on `macos/v1` at `3b12faa03` (the commits after it change
documents only): the physical-Mac job at full level, every step that ran passed
(umbrella repository `jt226ub/Apple-Silicon-Translation-Runtime`, run 46083556911, Mac
mini `Mac16,10`, Apple M4, macOS 27.0.1): the default-toolchain leg ran every step but the
memory-ordering soaks (soak level only); the Xcode 26.5 leg ran a reduced set (build, linkage, smoke, thread, `fexmac_c_test`, `fexmac_hook_test`, mutex, ordering on/off/scalar, instruction tests in four modes, the home check) and skipped the probe, low-memory, coverage, 1,000-start, self-modifying-code, fault-differential, code cache, two-pass cache, benchmark, benchmark-mutation, sync-script and upscaler steps (corrected in round 2, 28 November
2026: this said "with both toolchains, every step PASS"). Soak level: never run in the job (round 2 check, 28 November 2026). The hosted-Mac job (run 46083525991) and the
Linux arm64 job (run 46083526078) green. The orchestrator's log with each package's
evidence is the umbrella's `docs/testing/v1-progress.md`; the details are in
`docs/macOS-port-status.md`. Nothing in this section is a new measurement.

| # | Criterion | State | Evidence (where recorded) | Limits |
|---|---|---|---|---|
| 1 | All existing tests pass on real hardware, hardened, JIT entitlement only | met | Physical-Mac job, full level (run 46083556911; every step that ran passed; the Xcode 26.5 leg ran smoke, thread, the two `fexmac` tests, mutex, ordering and the instruction tests, not the other port tests; corrected in round 2): smoke, thread and every port test pass on the default toolchain; 2,296 instruction tests, 2,265 pass, 15 expected failures (checked stops), 6 runner skips, 0 unexpected, in each of four modes, as built and hardened (status document, "Tests"; progress log, rows A and B) | The 15 expected failures and the 16 tests not run (6 runner skips, 6 on upstream's lists, 4 that cannot be assembled above 4GB) are each explained (status document, "Every test that does not pass"); 32-bit guest tests are out of scope |
| 2 | Write protection is enforced and respected | met | `JITWriteProbe --expect-enforced` passes on the M4, as built and hardened (umbrella `docs/testing/round-1-report.md` step 1; the job's probe step); the branch without the runtime fixes dies with SIGBUS at its first write to code memory and the fixed branch passes (round 1 report, step 2); package A's fix for the 24 ordering-mode hangs, reverted, brings them back (progress log, row A) | The hosted Mac does not enforce write protection; only the physical Mac shows this |
| 3 | A C embedding interface exists and the test tools use it | met | `fexmac` (`Source/Embedding/`, `include/fexmac.h`, version 2.0) as static library, prelinked archive and dylib; the smoke, thread, ordering, benchmark and instruction-test tools use it for thread setup and fault handling; `fexmac_c_test` 80 subtests and `fexmac_hook_test` 24 pass on the M4 and the hosted Mac (runs above; progress log, row C; `docs/macOS-embedding-interface.md`) | The instruction-test runner still takes some harness work from the core (documented in the embedding document); the dylib under the hardened runtime needs the embedder and the dylib signed with one real Team ID |
| 4 | Fault handling is complete for an embedder | met with documented limits (the independent reviewer's verdict) | Misaligned atomics, scratch-buffer restart, self-modifying code by write protection, stops from another thread, guest faults with exact state: `fexmac_c_test`, `fexmac_hook_test`, the SMC instruction tests with `--smc mtrack` (3 of 3 per mode), `Scripts/macos_fault_diff.py` (1,472 cases x 4 modes x 4 ways, 2 known differences, 0 failures), as built and hardened on the M4 (run 46083556911, default-toolchain leg: the Xcode 26.5 leg runs the two `fexmac` tests but not the SMC tests or the fault differential, corrected in round 2; status document, "Fault handling for an embedder"; progress log, rows D and J) | From the progress log's row D: a 32-byte store whose lower page faults has written its upper half; `fxsave` across a page; 4K protections that differ within a 16K host page are refused unless the embedder opts in, and are then not enforced; writes through aliased mappings are not detected; no SMC detection with no memory registered. Cost: block-entry stop check, `rep` loops and rotates are slower (status document's performance table). A real guard-mapping failure is untested (row J) |
| 5 | x86 memory ordering holds between threads with emulation on | met, with one documented known gap for the owner to judge | `FEXCoreOrderingTest` on the M4: with all ordering settings on, 0 forbidden outcomes in 100 million tries per litmus test and the 17 counting tests exact; with emulation off the forbidden outcomes appear in every litmus test but LB (never seen on the M4), most by the million; on, off and scalar modes are hard requirements in the physical-Mac job (status document, "x86 memory ordering between threads"; progress log, row E). *Corrected in round 2 (28 November 2026):* the "0 forbidden outcomes" did not cover x87 stores: the tests of the time had no x87 store litmus test, and x87 stores to memory bypassed the ordering emulation in 1.0; round 2's x87 litmus tests show thousands of forbidden outcomes on the 1.0 code with `--tso on` and 0 after the fix, which is on `macos/wp-r2-ordering-speed`, not on `macos/v1` (status document, "Known defects found after 1.0 (round 2)") | **Known gap:** the misaligned back-patch is cross-modifying code the architecture does not guarantee: at most one access per thread per patched instruction can miss its barrier, once; never observed; kept as upstream's design; the owner is to judge it against this criterion. Split locked operations have five stated limits, none seen in the tests |
| 6 | Cached translations load from disk | met | Instruction tests twice with the cache, all four modes, as built and hardened: the warm pass compiles no block in any passing test and every result is identical; corrupt, mismatched and untrusted caches are rejected (`Scripts/macos_code_cache_tests.py`, 107 checks; about 20,000 fuzzed and re-sealed caches over the reviews with no crash, hang or wrong result) (run 46083556911, default-toolchain leg only: the Xcode 26.5 leg skips the cache steps, corrected in round 2; status document, "On-disk code cache"; progress log, row F) | A cache directory is trusted only when private to the user (same user only, by design); the cache's Linux build and out-of-space paths are unverified (progress log, row F) |
| 7 | A real guest program runs | met | The freestanding x86-64 benchmark (`unittests/macOS/Bench/`) through `FEXCoreBench`: the literal mode runs 16 kernels with exit as its only system call and every checksum right; 18 kernels in the timed mode match Rosetta 2 and native arm64 bit for bit; 58 mutations of the kernels are all caught (status document, "Guest program and benchmark"; `docs/macOS-benchmarks.md`; progress log, row G) | The timed mode uses the benchmark's own private ABI (write, clock, threads), not only exit; with ordering emulation off the two thread kernels may be wrong, as stated |
| 8 | Speed is measured | met; figures labelled PROVISIONAL | Every figure is generated by `Scripts/macos_bench.py` from `docs/macOS-benchmarks-results.json` and checked by `--check-docs`. Measured on the M4 at `3b12faa03` with the machine near idle (other processes averaged a third of a core): the headline, all ordering emulation on against Rosetta 2's hardware ordering, is 2.07 times Rosetta 2's time (geometric mean of 15 single-threaded kernels, the committed results), and 2.08 in the other of the two quietest of three runs (commit message of `0d7359b7e`; generated blocks in `docs/macOS-benchmarks.md` and the status document) | The script's strict load gate saw a two-second background spike above its peak limit, so it labelled every run PROVISIONAL. Whether to accept these figures or re-measure on a quieter machine with `--final` is the owner's decision. Most of the gap is the software emulation of x86 ordering, mostly the barriers on vector accesses |
| 9 | The port tracks upstream | met | Upstream FEX-Emu/FEX `main` at `a92baf073` merged with `Scripts/macos_sync_upstream.sh` (a real merge, no conflicts), the full suite passing afterwards on the M4, as built and hardened (progress log: default toolchain; `docs/macOS-upstream-sync.md`: both toolchains); the script is documented in `docs/macOS-upstream-sync.md` and its offline tests pass in both Mac jobs (31 then; 32 with the test added for the merge into `main`, run locally) (progress log, row H) | Upstream has moved on since `a92baf073` (its `main` was at `d663eb8ed` when this was written); the next sync is not done. Upstream's workflows carry a port change (a repository guard) to expect in sync conflicts |
| 10 | Tests run on real hardware automatically | met, with the runner started by hand and the security caveats | `physical_mac.yml` in the private umbrella repository, self-hosted runner `m4-mini` registered to that repository only, never to this public fork; at full level every step that ran passed, including the SMC and fault-differential steps on the default-toolchain leg; the Xcode 26.5 leg runs a reduced set without them (runs 46078132203 and 46083556911; corrected in round 2: this said "green at full level with both toolchains"; umbrella `docs/testing/physical-mac-job-report.md`; progress log, row I) | Job code runs as the owner's macOS account; the runner is started by hand with a scrubbed environment, for supervised runs only, and stopped afterwards; a dedicated non-admin runner user is recommended and is the owner's to set up; there is no nightly schedule until then. Soak level: never run in the job (round 2 check, 28 November 2026). (umbrella `docs/testing/physical-mac-runner.md`) |
| 11 | Documents are current and every expected failure is explained | met | `unittests/ASM/Known_Failures_macOS` has no "believed" entry: each of its 15 entries states the exact outcome (stop reason, signal, trap, address) and any other failure of a listed test fails the run; each is explained in the status document (progress log, row B). The status document was reviewed end to end for this merge (its "Document review" section says what was checked) | What is still open is listed in one place: "Open items" in `docs/macOS-port-status.md` |

Work packages:

| ID | Package | State | Commit on `macos/v1` |
|---|---|---|---|
| A | Hardware round | done | `146b99ad4` |
| B | Verify expected failures | done | `389a9a18b` |
| C | Embedding interface | done | `98a72c187` |
| D | Fault handling | done (criterion 4 met with documented limits) | `01cec85d9`, merged in `3b12faa03` |
| E | Memory ordering test | done (one known gap for the owner) | `1e405d7f0` |
| F | Code cache | done | `811ec1f9e` |
| G | Guest program and benchmark | done; benchmark figures PROVISIONAL | `bbf5f27c1`; figures at `0d7359b7e` |
| H | Upstream sync | done | `004e2e907` |
| I | Hardware test job | done (runner started by hand) | `3b12faa03`; umbrella `8ce96b1`, `69fc460` |
| J | Guard page and scratch-buffer review | done | `cd77b9b28` |

The decisions left to the owner: the criterion 5 known gap; whether to accept the
provisional benchmark figures or re-measure; the dedicated runner user and whether to
enable a nightly schedule after that; the repository settings listed in the status
document.
