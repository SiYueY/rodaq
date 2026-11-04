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
