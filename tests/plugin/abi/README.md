# ABI binary qualification tools

`run_counter_matrix.py`, `run_domain_matrix.py` and
`build_runtime_consumer.py` establish independent installed-SDK builds. CTest
orders their fixture dependencies. Domain minor 1 is a **synthetic append-only
qualification version**, not a published RoDAQ Interface version. These probes
do not qualify ROS 2, MCAP, filesystem commit or production workloads.

After building both compiler profiles for the same architecture, freeze their
already-tested binaries into a new, unique directory:

```sh
python3 tests/plugin/abi/freeze_binary_bundle.py \
  --identity FOUNDATION-BASELINE-ID \
  --output build/abi-baselines/FOUNDATION-BASELINE-ID \
  --profile gcc11=build/gcc11 --profile clang14=build/clang14
python3 tests/plugin/abi/replay_binary_bundle.py \
  --bundle build/abi-baselines/FOUNDATION-BASELINE-ID \
  --log build/abi-baselines/replay.log
```

Freeze refuses an existing destination. It preserves compiled layout manifests,
compiler/platform provenance, installed public and fixture headers, C/C++
providers, C consumers, the C++ runtime consumer and its SDK shared library.
Run this on the matching architecture (or explicitly identified emulation).

Replay invokes no compiler, CMake or installation step. It verifies every file
before and after all 84 directions: both compiler pairs, old/new Counter and
typed Interface consumers, plus the installed C++ SDK runtime. Package trees
remain readonly. To test a future SDK against these preserved providers, supply
both `--runtime-consumer /path/to/current/client` and
`--runtime-library-dir /path/to/current/sdk/lib`. Retain the original baseline;
never regenerate its providers during compatibility regression.

Archive the directory with `tar --zstd` and record the archive SHA-256 alongside
it in the stage evidence. Local build trees and expiring CI artifacts alone are
not a durable published ABI baseline. Freeze/replay output proves binary
compatibility only; authoritative stage acceptance remains in Notion 00.
