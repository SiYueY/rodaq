# RoDAQ M2 Fake Runtime Capacity Qualification

**Date:** 2026-11-05

**Build:** GCC 11.4.0, Release, CMake 3.22.1

**Platform:** Ubuntu 22.04.5, Linux 6.8.0, x86_64, 18 logical CPUs

**Storage:** Fake Storage, synchronous Payload/Persistence terminalization

This is M2 Fake Runtime evidence only. It is not MCAP, filesystem, NVMe,
compression, crash-recovery, or long-duration production evidence.

## Capacity Policy

The benchmark profiles use:

```text
stall tolerance: 0.25 s
safety factor:    2
capacity:         ceil(peak_records_per_second × 0.25 × 2)
```

The offered workload must converge to all expected Records within 110% of its
one-second source schedule. Every profile requires:

- accepted Records equals the offered schedule;
- all operations and Payload permits return to zero;
- no WaitTimeout, rejection or GateClosed count;
- high-water marks remain bounded;
- Stop converges.

CPU frequency scaling was enabled and benchmarks were not CPU-pinned. Latency
tails are therefore comparative engineering evidence, not a hard real-time
claim.

## Results

### Joint Telemetry

| Profile | Accepted | Accepted Bytes/s | Admission p99 / p99.9 | Persistence p99 / p99.9 | HWM Records / Bytes | Result |
|---|---:|---:|---:|---:|---:|---|
| Aggregated 1 kHz, 80,000 B | 1,000/s | 79.98 MB/s | 2.19 / 2.91 us | 87.42 / 101.06 us | 1 / 80,000 B | PASS |
| Per-joint 100 × 1 kHz, 800 B | 99,892/s | 79.91 MB/s | 19.38 / 62.62 us | 62.98 / 101.82 us | 55 / 44,000 B | PASS |

### Camera

| Profile | Accepted | Accepted Bytes/s | Admission p99 / p99.9 | Persistence p99 / p99.9 | HWM Records / Bytes | Result |
|---|---:|---:|---:|---:|---:|---|
| 10 × 720p30, 200 KiB encoded frame | 309.79/s | 63.44 MB/s | 16.54 / 27.34 us | 223.36 / 351.49 us | 6 / 1.23 MB | PASS |
| 10 × 720p30, raw RGB8 | 308.10/s | 851.83 MB/s | 11.52 / 12.77 us | 166.27 / 197.76 us | 4 / 11.06 MB | PASS |

### Mixed

| Profile | Accepted | Accepted Bytes/s | Admission p99 / p99.9 | Persistence p99 / p99.9 | HWM Records / Bytes | Result |
|---|---:|---:|---:|---:|---:|---|
| Encoded cameras + aggregated joints | 1,300.04/s | 141.45 MB/s | 4.73 / 16.20 us | 115.20 / 168.06 us | 5 / 1.02 MB | PASS |
| Raw cameras + per-joint joints | 99,890/s | 905.72 MB/s | 19.92 / 40.93 us | 988.67 / 1,694.72 us | 185 / 11.08 MB | PASS |

### Slot and Release Workers

| Profile | Result |
|---|---|
| 100k 16-byte Record slot terminalization | 404,859 Records/s; HWM 45,369 slots |
| 100k 1 KiB copy Record steady state | 275,323 Records/s; HWM 15,954 slots |
| Copy Payload class reserve/copy/release | 28.3 us average across 800 B / 80 KB / raw-frame classes |
| 1 KiB Payload reserve/release | 0.023 us |
| 64 KiB Payload reserve/copy/release | 1.14 us |
| Adopt release, 1 worker | 244,090 Records/s |
| Adopt release, 2 workers | 157,584 Records/s |
| Adopt release, 4 workers | 107,207 Records/s |

The measured single Release worker is sufficient and currently outperforms
additional workers for this workload. One worker remains the M2 default;
multiple workers remain configurable for later backend/evidence-driven tuning.

## Interpretation

The Release build meets the declared M2 Fake Runtime capacity for the standard
camera, joint and mixed workloads. Debug builds are intentionally slower and
are used for correctness/sanitizer qualification, not capacity claims.

M5 must re-establish storage production capacity with real MCAP serialization,
compression/chunking, filesystem behavior, durable writes, crash recovery and
long-duration soak.

## M2 Final Closure Fix comparison — fe4f938

Baseline: `413ba65`; final implementation: `fe4f938adef1680021201334dae0c434f67d4011`.
Both were built with GCC 11.4 Release/warnings-as-errors from the same pinned
dependencies on this Ubuntu 22.04 x86_64 host. Every existing benchmark ran
three repetitions; the table uses medians. The benchmark definitions and offered
schedules were unchanged. Single-producer Pipeline, multi-producer/multi-Stream
Joint and Camera, both Mixed profiles, slot terminalization and 1/2/4 release
workers all passed their existing admission/completion/Stop/resource gates.

| Profile | Baseline → final accepted Records/s | CPU core-equivalents | Persistence p99 us | HWM Records | Max RSS KiB |
|---|---:|---:|---:|---:|---:|
| BM_EncodedCamera10x720p30 | 310 → 310 | 0.32 → 0.33 | 134.01 → 225.41 | 5 → 7 | 38,976 → 38,920 |
| BM_RawCamera10x720p30 | 308 → 308 | 1.56 → 1.58 | 132.99 → 140.80 | 3 → 3 | 472,212 → 472,064 |
| BM_PerJoint100Streams1kHz | 99,944 → 99,977 | 1.94 → 1.87 | 65.86 → 48.13 | 22 → 26 | 94,000 → 94,292 |
| BM_AggregatedRobotState1kHz | 1,000 → 1,000 | 0.34 → 0.35 | 73.60 → 104.70 | 1 → 1 | 94,000 → 94,292 |
| run_encoded_aggregated | 1,300 → 1,300 | 0.49 → 0.48 | 106.75 → 99.71 | 5 → 7 | 78,580 → 78,320 |
| run_raw_per_joint | 100,270 → 100,272 | 3.28 → 3.39 | 232.70 → 483.84 | 45 → 118 | 583,816 → 583,460 |
| BM_AdoptReleaseWorkers/1 | 275,761 → 275,019 | 2.83 → 2.86 | 326.40 → 91.90 | 288 → 431 | 42,844 → 43,264 |
| BM_AdoptReleaseWorkers/2 | 222,663 → 237,258 | 3.28 → 3.29 | 1624.06 → 7704.57 | 437 → 2,067 | 44,148 → 47,976 |
| BM_AdoptReleaseWorkers/4 | 158,853 → 153,930 | 3.83 → 3.77 | 80347.13 → 52396.03 | 13,488 → 8,854 | 62,012 → 64,620 |
| BM_RuntimePipelineSmallRecordSteadyState | 392,114 → 373,124 | 2.27 → 2.26 | 37257.21 → 25018.37 | 15,984 → 11,688 | 92,656 → 92,996 |
| BM_StorageSlotTerminalization100k | 446,981 → 456,085 | 2.24 → 2.22 | 53346.30 → 46071.81 | 25,621 → 26,068 | 45,680 → 45,848 |

All declared rate profiles sustained their schedules with zero ordinary
rejections/WaitTimeouts and convergent Stop. Unpaced small-Record and slot
throughput, CPU, tails and high-water are shown in the table. An initial
comparison identified an unnecessary extra handoff mutex
introduced by the fix; the final implementation uses an atomic cutoff
authorization at the same boundary, avoiding that additional normal-path lock.

Tail latency and high-water variation remain visible in the table, especially
unpaced bursts and multiple release workers. CPU frequency scaling and host
scheduling were uncontrolled, so these are comparative Fake Runtime results,
not a new tail-latency SLA or proof of a uniform performance improvement.
One release worker remains the default; no benchmark policy or resource bound
was relaxed. Failure behavior is separately verified by the new lifecycle,
queued-cutoff, Retain and late-handoff/root-recovery regression tests.

## M2 Ownership Closure qualification — working tree based on 3adefb28

GCC 11.4 Release/warnings-as-errors, same Ubuntu 22.04 host and pinned production
dependencies. Eight existing benchmark executables each ran three repetitions:
Joint, Camera, Mixed, StorageSlot, ReleaseWorker, RuntimePipeline, PayloadClass
and PayloadPool. All completed without benchmark errors; scheduled profiles
passed their existing accepted-count, zero-rejection/WaitTimeout, bounded-resource
and Stop gates. No rate, capacity or completion criterion was relaxed.

The mixed Fake Acquisition configuration now explicitly supplies distinct
`camera-source` and `joint-source` IDs. This corrects the fixture's previous
cross-owner identity collision without changing its workload or production
Canonical identity handling. Measurements below are medians.

| Profile | Accepted Records/s | Persistence p99 us | HWM Records | Stop us |
|---|---:|---:|---:|---:|
| BM_EncodedCamera10x720p30 | 310 | 107.71 | 3 | 599 |
| BM_RawCamera10x720p30 | 308 | 134.66 | 3 | 407 |
| BM_PerJoint100Streams1kHz | 99,980 | 24.62 | 27 | 1,628 |
| BM_AggregatedRobotState1kHz | 1,000 | 54.27 | 1 | 370 |
| run_encoded_aggregated | 1,300 | 92.86 | 5 | 425 |
| run_raw_per_joint | 100,039 | 136.06 | 160 | 1,698 |
| BM_AdoptReleaseWorkers/1 | 275,063 | 413.69 | 410 | 1,042 |
| BM_AdoptReleaseWorkers/2 | 221,137 | 13991.93 | 3,183 | 1,070 |
| BM_AdoptReleaseWorkers/4 | 134,066 | 118358.01 | 18,778 | 1,210 |
| BM_RuntimePipelineSmallRecordSteadyState | 366,714 | 57704.45 | 21,459 | 1,199 |
| BM_StorageSlotTerminalization100k | 451,775 | 73269.25 | 38,743 | 1,030 |

Raw JSON and stderr were recorded under `/tmp/rodaq-ownership-capacity-*` on the
qualification host. These files are transient artifacts; this table is the
durable result. CPU frequency and scheduling remain uncontrolled. The visible
tail/HWM variation, particularly with multiple release workers and unpaced bursts,
is not evidence of a uniform regression or improvement. One release worker
remains the default; real ROS2/MCAP/filesystem tails remain M3–M5 qualification.

## C++ coding standards migration — working tree based on be448c8

All eight existing Release benchmark executables passed three repetitions on
the same GCC 11 / Ubuntu 22.04 host, using the existing resource, acceptance,
zero-rejection and Stop gates. Function renaming also changes the benchmark
registration labels; `bm_*` below corresponds to the previous `BM_*` profile.
No workload, capacity policy or benchmark gate was relaxed. Values are medians.

| Profile | Accepted Records/s | Persistence p99 us | HWM Records | Stop us |
|---|---:|---:|---:|---:|
| bm_encoded_camera10x720p30 | 310 | 125.18 | 5 | 1,145 |
| bm_raw_camera10x720p30 | 308 | 159.62 | 3 | 1,168 |
| bm_per_joint100_streams1k_hz | 99,978 | 174.21 | 101 | 2,142 |
| bm_aggregated_robot_state1k_hz | 1,000 | 101.25 | 1 | 954 |
| run_encoded_aggregated | 1,300 | 110.72 | 6 | 1,328 |
| run_raw_per_joint | 100,136 | 392.96 | 77 | 1,863 |
| bm_adopt_release_workers/1 | 280,176 | 916.99 | 722 | 997 |
| bm_adopt_release_workers/2 | 218,059 | 3346.43 | 828 | 1,043 |
| bm_adopt_release_workers/4 | 150,325 | 81395.71 | 13,366 | 941 |
| bm_runtime_pipeline_small_record_steady_state | 387,945 | 36438.01 | 15,257 | 1,032 |
| bm_storage_slot_terminalization100k | 454,423 | 45449.21 | 26,412 | 905 |

PayloadClass and PayloadPool benchmarks also completed without errors. CPU
frequency and host scheduling were not fixed, so latency/high-water variation
is comparative evidence rather than a new performance guarantee. Real
ROS2/MCAP/filesystem qualification remains future milestone work.


## 2026-11-05 declaration separation follow-up

After moving SDK and internal Runtime definitions out of headers, all eight
existing Release benchmark executables passed three repetitions. Selected median
accepted Records/s compared with the preceding naming-review run: per-joint
99,975 vs 99,978; raw mixed 100,211 vs 100,136; one release worker 291,931 vs
280,176; Runtime pipeline 397,750 vs 387,945; storage slots 454,000 vs 454,423.
The host frequency and scheduling were not controlled; these are capacity-gate
and comparative observations, not evidence of a guaranteed speed change.


## 2026-11-05 Status + Result migration qualification

All eight existing Release benchmark executables passed three repetitions with
unchanged workloads and capacity/acceptance/shutdown gates. Values below are
medians on the same GCC 11 / amd64 host. PayloadClass and PayloadPool also
completed without errors. Default Result failure construction has empty
diagnostics, avoiding a new per-operation diagnostic-string allocation.

| Profile | Accepted Records/s | Persistence p99 us | HWM Records | Stop us |
|---|---:|---:|---:|---:|
| bm_encoded_camera10x720p30 | 310 | 127.42 | 8 | 1,211 |
| bm_raw_camera10x720p30 | 308 | 184.19 | 5 | 1,255 |
| bm_per_joint100_streams1k_hz | 99,979 | 39.77 | 20 | 1,677 |
| bm_aggregated_robot_state1k_hz | 1,000 | 82.94 | 1 | 843 |
| run_encoded_aggregated | 1,300 | 101.76 | 5 | 1,176 |
| run_raw_per_joint | 100,249 | 331.26 | 61 | 2,484 |
| bm_adopt_release_workers/1 | 279,096 | 744.96 | 1,812 | 1,119 |
| bm_adopt_release_workers/2 | 241,707 | 29,818.88 | 8,164 | 946 |
| bm_adopt_release_workers/4 | 153,350 | 89,849.85 | 17,612 | 1,427 |
| bm_runtime_pipeline_small_record_steady_state | 418,186 | 44,171.26 | 20,632 | 982 |
| bm_storage_slot_terminalization100k | 453,695 | 46,989.31 | 28,302 | 866 |

## 2026-11-06 Core Structure Closure qualification

GCC 11.4.0 Release with warnings-as-errors ran all eight existing benchmark
executables for three repetitions on Ubuntu 22.04.5. Workloads, capacity policy,
acceptance gates, resource-return checks and Stop convergence checks were not
changed. All executables passed.

| Profile | Accepted Records/s | Accepted Bytes/s | Persistence p99 us | HWM Records | Stop us |
|---|---:|---:|---:|---:|---:|
| `bm_encoded_camera10x720p30` | 310.08 | 63.50 MB/s | 47.68 | 4 | 497.41 |
| `bm_raw_camera10x720p30` | 309.15 | 854.74 MB/s | 234.50 | 5 | 663.55 |
| `bm_per_joint100_streams1k_hz` | 99,967.12 | 79.97 MB/s | 3,352.58 | 462 | 2,422.78 |
| `bm_aggregated_robot_state1k_hz` | 999.80 | 79.98 MB/s | 89.60 | 1 | 382.46 |
| `run_encoded_aggregated` | 1,300.32 | 141.47 MB/s | 37.95 | 6 | 643.58 |
| `run_raw_per_joint` | 100,026.65 | 906.96 MB/s | 81.79 | 105 | 2,002.94 |
| `bm_daq_pipeline_small_record_steady_state` | 343,464.35 | 351.71 MB/s | 79,364.10 | 36,618 | 1,259.52 |
| `bm_storage_slot_terminalization100k` | 427,569.22 | 6.84 MB/s | 82,771.97 | 48,188 | 1,329.15 |

The PayloadClass and PayloadPool executables completed without benchmark errors.
CPU frequency scaling and scheduling were not fixed, so these remain Fake Runtime
capacity and comparative engineering evidence rather than a real-time SLA or
MCAP/filesystem/NVMe production claim.

Compared with the preceding declaration-separation run, per-joint acceptance
was approximately 99,979 vs 99,975 Records/s, raw mixed 100,249 vs 100,211,
and the small-record pipeline 418,186 vs 397,750. The host CPU frequency and
scheduling were not controlled; these observations establish existing gate
passes and do not establish a performance improvement guarantee. Real backend
and filesystem qualification remain outside this task.

## 2026-11-06 Result<void> operation migration qualification

All eight existing Release benchmark executables passed three repetitions with
unchanged workload and correctness/capacity gates. PayloadClass and PayloadPool
also completed without benchmark errors. Median observations on GCC 11 / amd64:

| Profile | Accepted Records/s | Persistence p99 us | HWM Records | Stop us |
|---|---:|---:|---:|---:|
| bm_encoded_camera10x720p30 | 310 | 123.39 | 8 | 561 |
| bm_raw_camera10x720p30 | 309 | 218.24 | 5 | 1,057 |
| bm_per_joint100_streams1k_hz | 99,979 | 56.48 | 40 | 2,234 |
| bm_aggregated_robot_state1k_hz | 1,000 | 83.58 | 1 | 792 |
| run_encoded_aggregated | 1,300 | 122.62 | 7 | 593 |
| run_raw_per_joint | 100,275 | 261.38 | 71 | 2,271 |
| bm_adopt_release_workers/1 | 287,734 | 177.79 | 558 | 974 |
| bm_adopt_release_workers/2 | 234,772 | 9,715.71 | 2,438 | 1,004 |
| bm_adopt_release_workers/4 | 150,367 | 84,672.51 | 15,653 | 998 |
| bm_runtime_pipeline_small_record_steady_state | 406,610 | 29,999.10 | 12,952 | 1,028 |
| bm_storage_slot_terminalization100k | 475,031 | 51,412.99 | 31,424 | 922 |

The preceding Status-returning migration results remain in the prior table.
The workload gates passed after the Result<void> migration; host frequency and
scheduling were not controlled, so these observations do not establish a speed
improvement or a real backend/filesystem performance guarantee. M3 remains
NOT STARTED. Hosted qualification for these changes remains PENDING.

Compared with the preceding Status-returning run, median per-joint acceptance
was 99,979 vs 99,979 Records/s, raw mixed
100,275 vs 100,249, and the small-record
pipeline 406,610 vs 418,186. These are
uncontrolled host observations with unchanged pass thresholds.

## 2026-11-06 Core structure and naming migration qualification

The Release benchmark suite was run for three repetitions after the DAQ SLO
naming and domain source split. All profiles retained their accepted-count,
zero-rejection/WaitTimeout, bounded high-water and Stop-convergence gates.
Median observations on the same GCC 11 / amd64 class of host:

| Profile | Accepted Records/s | Accepted MB/s | Persistence p99 / p99.9 us | HWM Records | Stop us |
|---|---:|---:|---:|---:|---:|
| bm_encoded_camera10x720p30 | 310 | 63.5 | 185.5 / 198.3 | 10 | 926.2 |
| bm_raw_camera10x720p30 | 309 | 854.9 | 161.4 / 261.4 | 4 | 716.8 |
| bm_per_joint100_streams1k_hz | 99,970 | 80.0 | 177.0 / 885.2 | 105 | 2,073.6 |
| bm_aggregated_robot_state1k_hz | 1,000 | 80.0 | 109.4 / 132.4 | 1 | 409.6 |
| run_encoded_aggregated | 1,300 | 141.4 | 129.3 / 163.2 | 4 | 641.5 |
| run_raw_per_joint | 100,182 | 908.4 | 39.7 / 465.9 | 98 | 2,034.7 |

Compared with the preceding Result<void> qualification medians, per-joint
acceptance is 99,970 vs 99,979 Records/s, raw-camera bandwidth 854.9 vs
851.8 MB/s, and raw mixed bandwidth 908.4 vs 905.7 MB/s. The migration therefore
shows no unexplained throughput regression. Host CPU frequency and scheduling
remain uncontrolled; these results are comparative capacity evidence, not a
real backend/filesystem performance guarantee. M3 remains NOT STARTED.
