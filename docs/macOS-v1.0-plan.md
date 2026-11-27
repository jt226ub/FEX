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

- Branch from `macos/runtime-fixes` as `macos/wp-<id>-<short-name>`. One package, one
  branch. Do not commit to `main`.
- Use the method macOS provides. Keep a workaround only when none exists, with the reason
  in a comment.
- Every behaviour change comes with a test that runs on Apple silicon, and the Linux
  core-only build must still pass.
- Update `docs/macOS-port-status.md` in the same change.
- Nothing goes to FEX-Emu/FEX. Any pull request names `jt226ub/FEX` as its base.
- Keep upstream's licence and copyright notices.
