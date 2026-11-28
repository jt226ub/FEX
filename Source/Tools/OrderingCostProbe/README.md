# OrderingCostProbe

Times, natively on the host, the arm64 instruction sequences that x86 memory-ordering
emulation is made of: plain, acquire/release and barrier forms of 16-byte vector loads
and stores, scalar acquire/release accesses, barriers alone and between other work,
store-to-load forwarding when access widths change, 4K and 16K copies done each way, and
the exact loops the JIT emits today for `rep movsb`/`rep movsq` with string ordering on and
off. Standalone, macOS only, not linked against FEX. Round 2, task E1.

```
clang -O2 -mcpu=apple-m1 -o OrderingCostProbe Source/Tools/OrderingCostProbe/Main.c
./OrderingCostProbe --qos both            # performance cores, then efficiency cores
./OrderingCostProbe --cpumap              # which CPU numbers belong to which core type
./OrderingCostProbe --list                # the kernels
```

Every timed loop is inline assembly; check the sequences with `otool -tv OrderingCostProbe`.
The sequences said to be "today's" follow `FEXCore/Source/Interface/Core/JIT/MemoryOps.cpp`
(`LoadMemTSO`, `StoreMemTSO`, `EmitRepStringInPlace`) on a host with FEAT_LRCPC2.

Columns: `ns min`/`ns med` are wall time per operation (minimum and median of the
samples); `cyc med` is the median of (ns x the clock frequency measured by a calibration
loop of dependent adds just before that sample), so it does not depend on how far the core
has ramped; `ns/B` is per byte copied or accessed; `cyc/base` compares a row with the row
it is measured against (named in the source); `GHz` is the calibration range over the
row's samples and `CPUs` the CPU numbers the samples started and ended on
(`pthread_cpu_number_np`).

Core type. macOS has no affinity interface. `--qos interactive` runs the kernels in a
user-interactive thread (performance cores preferred), `--qos background` in a
background-QoS thread, `darwinbg` adds `setpriority(PRIO_DARWIN_THREAD, 0, PRIO_DARWIN_BG)`.
On the M4 Mac mini (macOS 27.0.1) CPUs 0 to 5 are the efficiency cores (at most 2.89 GHz
by the calibration loop) and 6 to 9 the performance cores (up to about 4.4 GHz); check
with `--cpumap` and with `sysctl hw.perflevel0 hw.perflevel1`. A background thread ran on
the performance cores whenever the thread that started it waited in `pthread_join`
(priority is lent to the joined thread); the tool therefore waits by polling. Running the
whole process with `taskpolicy -b` also keeps it on the efficiency cores.

Timings depend on the machine's load; the tool prints the load average at start and end.
