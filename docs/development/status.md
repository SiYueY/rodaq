# RoDAQ Development Status

**Updated:** 2026-11-03
**Branch:** `develop`
**Pre-integration checkpoint:** `9996e2b` — `runtime: implement projection and clock correlation services`

This file records factual current repository state. Keep it concise and update it after meaningful implementation or validation work.

## Current milestone

**M0 — Foundation + Plugin ABI**

Status: **CLOSURE REVIEW**

Current sub-phase: **M0 Closure Fix**. ABI freeze and M0 PASS are pending
caller-owned APPENDABLE, SDK firewall, manifest parity, bidirectional
compatibility and final platform qualification.

The design phase is sufficiently mature to begin implementation. The first implementation target is the common foundation and Plugin ABI/infrastructure, not ROS 2 or MCAP integration directly.

The V1 backend scope has now converged to ROS2 Acquisition + FastDDS Acquisition + MCAP Storage. ROS2 and FastDDS are required alternative process profiles by default; same-process co-activation remains unsupported unless Fast DDS/Fast CDR dependency coexistence qualification passes. SQLite3, Generic DDS and SocketCAN remain future architecture-validation targets.

## Current repository state

M0-A Review Closure, M0-B Stable C Plugin ABI, M0-C Plugin Loading + Lifetime
and M0-D Runtime Services + ConfigView are complete and validated. The
repository now contains:

- a C++17 CMake project foundation,
- warning, sanitizer, CTest, dependency and install support,
- Public `rodaq::Error` and `rodaq::Result<T>` primitives,
- eight Stable C ABI headers under `include/rodaq/plugin/abi/`, with
  `common.h` as the shared primitive header,
- C11/C++17 ABI compilation, layout, minimum-prefix and symbol qualification,
- a private Linux Plugin loader with Boot ABI, family, descriptor and provenance validation,
- private DSO and instance lease ownership with bounded normal unload behavior,
- catalog/manager orchestration and explicit quarantine retention,
- pinned dependency submodules with no normal-build network access,
- header-only install validation for `rodaq::Error` and `rodaq::Result<T>`.

`third_party/` contains all nine approved pinned submodules. M0-D adds bounded
Service Registry and ConfigView implementations, activation-lifetime
InteropProjection and ClockCorrelation services, and Fake Plugin
`bind/configure` integration. No C++ Plugin SDK, linkable installed RoDAQ
package target or production shared library has been introduced.

## Design baseline

Current system baseline:

- `docs/design/rodaq_v1_system_design.md` — V1 Detailed System Design / Implementation Baseline.
- `docs/design/rodaq_plugin_system.md` — V1 Plugin System Detailed Design / Freeze Candidate.
- `docs/design/architecture.md` — v0.6.
- `docs/design/runtime.md` — v0.6.
- `docs/design/canonical_data_model.md` — v0.9.
- `docs/design/canonical_profile.md` — v0.6.
- `docs/design/admission.md` — v0.5.
- `docs/design/payload.md` — v0.5.
- `docs/design/acquisition_plugin.md` — v0.7, Plugin ABI Reconciliation Candidate.
- `docs/design/acquisition_plugin/ros2.md` — v0.5, Plugin ABI Mapping Review Candidate.
- `docs/design/acquisition_plugin/fastdds.md` — v0.5, V1 Implementation Mapping Baseline.
- `docs/design/storage_plugin.md` — v0.5, Plugin ABI Reconciliation Candidate.
- `docs/design/storage_plugin/mcap.md` — v0.6, Plugin ABI Mapping Review Candidate.
- `docs/design/storage_plugin/sqlite3.md` — v0.5, Future Storage Mapping / Architecture Validation.

## V1 implementation scope

V1 required implementation:

- ROS2 Acquisition.
- FastDDS Acquisition.
- MCAP Storage.

V1 artifact requirement:

- ROS2-profile MCAP consumable by `ros2 bag play`, PlotJuggler and Foxglove, while retaining RoDAQ full-fidelity sidecars.

Architecture/design targets retained for later implementation include Generic DDS, SocketCAN and SQLite3; they are not V1 release gates.

The common Runtime and Plugin infrastructure must be validated with Fake Plugins before the first real backend vertical slice.

## Completed

- Canonical data model and V1 profile have undergone multiple design reviews.
- Runtime, Admission, Payload, Acquisition Plugin and Storage Plugin design documents exist.
- Non-compressed V1 system design baseline has been produced.
- Detailed Plugin System design has been produced.
- V1 development stages have been unified into one authoritative M0–M5 milestone model; the system blueprint defines the Gate semantics and `docs/development/plan.md` tracks execution state.
- Plugin System / generic Acquisition / generic Storage / ROS2 / FastDDS / MCAP documents have been reconciled to the same stable ABI semantics; SQLite3 remains a future architecture-validation mapping.
- Plugin architecture direction has converged on:
  - stable C ABI,
  - C++17 authoring SDK,
  - family-specific Acquisition / Storage protocols,
  - Boot ABI marker,
  - Service Registry,
  - Plugin Interface Query,
  - Runtime-owned ConfigView,
  - explicit lifecycle/thread/ownership contracts,
  - Acquisition `AdmissionOutcome + RejectionReason + PayloadDisposition`,
  - independent Storage Payload/Persistence completion axes,
  - RAII DSO lifetime,
  - ABI CI.
- Shared repository context mechanism established with:
  - `AGENTS.md`,
  - `docs/development/status.md`,
  - `docs/development/plan.md`,
  - `docs/development/decisions.md`.
- M0-A project foundation implemented:
  - CMake 3.22 / C++17 baseline,
  - warnings and ASan / UBSan options,
  - CTest integration,
  - all nine approved dependencies pinned as Git submodules,
  - expected-lite 0.10.0 installed as the explicit Public C++ header dependency,
  - `rodaq::Error` with the V1 C++ error taxonomy,
  - `rodaq::Result<T>` as the D-025 direct nonstd expected-lite alias,
  - independent Public Header compile tests and unit tests,
  - compile-only external consumer smoke using an installed header tree.
- M0-A Review Closure implemented:
  - removed the unauthorized `rodaq::Unexpected` alias,
  - mechanically enforced expected-lite 0.10.0 and nonstd selection,
  - added automated wrong-version and std-selection compile-fail tests,
  - aligned `BUILD_TESTING` / `RODAQ_BUILD_TESTS`,
  - wired `RODAQ_BUILD_BENCHMARKS` to the benchmarks subtree,
  - made Git submodule gitlinks the sole dependency commit authority.
- M0-B Stable C Plugin ABI implemented:
  - `common.h`, `canonical.h`, `service.h`, `config.h`, `runtime.h`,
    `acquisition.h`, `storage.h` and `plugin.h`,
  - required Service and control-interface ID constants,
  - `query_interface()` family slots for required interfaces and optional extensions,
  - complete `rodaq_error` diagnostics for Storage completion and failure callbacks,
  - Descriptor readiness results carrying inline failure diagnostics,
  - `rodaq_storage_operation` owned by the Storage ABI,
  - all 30 V1 APPENDABLE minimum-prefix constants,
  - FROZEN layout sentinels,
  - C11 and C++17 independent header compilation,
  - hidden-visibility Acquisition and Storage symbol fixtures,
  - installed C, C++ ABI and Result consumer qualification.
- M0-C Plugin Loading + Lifetime implemented:
  - `SharedLibrary` with `RTLD_NOW | RTLD_LOCAL`, canonical paths and RAII unload,
  - Boot ABI and `dlinfo` / `dladdr` symbol provenance validation,
  - mutually exclusive Acquisition / Storage family detection,
  - family minimum-prefix and required-slot validation,
  - Plugin ID, UTF-8 and complete SemVer 2.0 descriptor validation,
  - `PluginLibraryLease` and `PluginInstanceLease` lifetime separation,
  - `AcquisitionPluginInstance` / `StoragePluginInstance` create and exactly-once destroy,
  - `PluginCatalog` path merge and duplicate-ID conflict handling,
  - minimal `PluginManager` explicit-path orchestration,
  - process-lifetime quarantine without unsafe destroy or unload,
  - `AcquisitionPluginInstance` / `StoragePluginInstance` private owner handles,
  - direct private state types without a `detail` namespace,
  - `plugin_error` status conversion helpers without an `abi_` prefix,
  - isolated DSO fixture matrix and subprocess lifecycle verification.
- M0-D Runtime Services + ConfigView implemented:
  - additive projection and clock-correlation Stable Runtime Service ABI,
  - C11/C++17 header, minimum-prefix, tail-gating, scalar and installed-header
    qualification,
  - bounded freeze-once Service Registry with explicit query and error semantics,
  - immutable ConfigTree/ConfigView with exact scalar types and stable MAP order,
  - activation-lifetime InteropProjectionRegistry with immutable copied bytes,
    idempotent same-byte registration and finite count/byte bounds,
  - activation-scoped ClockCorrelation with deterministic anchor capture and
    checked ingress-to-Unix projection,
  - private instance-level `bind/configure` ABI dispatch with caller-owned error
    conversion,
  - Fake Acquisition/Storage service binding, config consumption, projection
    exchange and clock-anchor query through real DSOs.

M0-B post-PASS structural convergence:

```text
include/rodaq/abi/
    → include/rodaq/plugin/abi/

include/rodaq/abi/abi.h
    → include/rodaq/plugin/abi/common.h
```

The path-only step did not change binary ABI semantics. The old public include
path and compatibility headers were removed because RoDAQ has no released
consumer for that path. The following pre-freeze convergence then applied the
intentional ABI changes.

M0-B pre-freeze ABI convergence:

- family `get_extension` slots became `query_interface`;
- control extension IDs became control interface IDs with unchanged strings;
- `rodaq_storage_operation` moved from Canonical to Storage ownership;
- Storage async/systemic failures now carry complete `rodaq_error` diagnostics;
- Descriptor readiness failures now use inline `rodaq_error`, producing the
  converged `336`-byte result layout;
- D-017 terminology and D-029 Storage-diagnostics policy are recorded.

Structural convergence validation:

- GCC 11 Debug, ASan, UBSan and combined ASan+UBSan each passed 65/65 CTest
  entries.
- All eight C11/C++17 standalone header tests passed under the new path,
  including FROZEN layout, 30 minimum-prefix constants, scalar/protocol values
  and symbol allowlists.
- Install smoke verified all eight headers under
  `<prefix>/include/rodaq/plugin/abi/` and confirmed the legacy
  `<prefix>/include/rodaq/abi/` directory is absent.
- Active source/tests/build references contain no legacy include path, old
  guard or old header test name; the only retained old-path text is the
  migration record in this status file and D-028.
- Catalog-removal regressions verify live instance and live library lease
  lifetime independent of catalog ownership.
- CMake matrix verified `BUILD_TESTING=OFF` and `RODAQ_BUILD_TESTS=OFF` each
  produce zero RoDAQ tests; the enabled matrix produces all 65 tests.
- Network audit recorded no network syscalls during a fresh configure/build.
- Production ABI/code contains no `get_extension`, `CONTROL_EXTENSION_ID` or
  `required control extension` terminology; `rodaq_storage_operation` is
  defined only by `storage.h`.
- clang-format, `git diff --check`, whitespace and recursive submodule audits
  passed.

## Not started

- Minimal C++ Plugin SDK.
- Plugin exception trampolines and complete Fake lifecycle qualification (M0-E).
- Runtime ResourceBudget / Admission implementation.
- Payload / PayloadPool implementation.
- Dispatch / Completion implementation.
- Full Fake Acquisition / Storage runtime data path.
- ROS 2 Acquisition implementation.
- MCAP Storage implementation.
- FastDDS Acquisition implementation.
- SQLite3 Storage implementation.

## Current design readiness

A second independent cross-document review found additional contract conflicts after the earlier PASS claim. Findings FR-01 through FR-12 were repaired, then the full contract set was re-reviewed across Canonical, Runtime, Admission, Payload, Plugin ABI, Acquisition/Storage, backend mappings, lifecycle, logging and milestone gates. The post-repair consolidated static design review now passes.

Current reconciliation state:

```text
R1 — Canonical ABI Contract Reconciliation
    COMPLETE

R2 — Control & Lifecycle Contract Reconciliation
    COMPLETE

R3 — ABI Mechanics Contract Reconciliation
    COMPLETE

Second independent review FR-01..FR-12 repair
    COMPLETE

Consolidated post-repair design review
    PASS
```

Closed areas include:

```text
Canonical Record identity / Source / Clock / Timestamp projection
required Acquisition / Storage control interfaces
instance-wide non-destructive Acquisition quiesce before destructive Stop
Binding abort KEEP_PAUSED / RESUME_OLD disposition
Runtime lifecycle snapshot for recovery gating
Gate-pass-before-AcceptanceSequence commit order
Storage HandoffCommit / return / reconcile timing
Storage close disposition
operation-scoped control timeout / quarantine semantics
Clock / Stream Plugin-instance registration-key semantics
caller-owned ABI diagnostics
appendable input/output compatibility and normative minimum-prefix rules
cross-thread Storage completion race handling
physical dispatch-capacity reservation
MCAP long-run index/memory boundedness policy
RoDAQ-native logging design
three-level Semantic / ABI / Backend qualification freeze model
```

No remaining known P0/P1 static design blocker prevents M0 Foundation + Plugin ABI implementation. M0 may proceed from the repaired contract set.

The Stable C ABI and private Plugin loading/lifetime boundary are implemented and
mechanically qualified. No Service Registry, ConfigView, Plugin SDK, Runtime data
plane or backend round-trip has been executed. ROS2/MCAP executable
qualification remains an M3 gate; FastDDS/MCAP interoperability is the M4 gate;
production hardening/release qualification is M5. SQLite3 is now a future design
validation target rather than a V1 release gate.

The Result policy is now frozen by D-025:

- `rodaq::Result<T>` directly aliases `nonstd::expected<T, Error>`.
- expected-lite 0.10.0 is a pinned Public C++ header dependency.
- V1 always uses expected-lite's nonstd implementation.
- expected-lite never enters the Stable C Plugin ABI.

## Immediate next actions

1. Begin M0-E — C++ Plugin SDK + exception trampolines.
2. Implement the C++17 Plugin SDK and complete exception firewall.
3. Implement Plugin Interface Query plumbing.
4. Add Plugin metadata generation and authoring CMake helpers.
5. Build complete Fake Acquisition / Storage lifecycle tests for M0-E.

## Validation state

```text
M0-A Foundation
    IMPLEMENTED
    COMPILED
    UNIT TESTED
    ASAN TESTED
    UBSAN TESTED
    INSTALL HEADER COMPILE SMOKE TESTED
    PASS

M0-A Review Closure
    IMPLEMENTED
    NEGATIVE COMPILE TESTED
    CMAKE OPTION MATRIX TESTED
    INSTALL HEADER TESTED
    REVIEWED
    PASS

M0-B
    IMPLEMENTED
    C COMPILE TESTED
    C++17 COMPILE TESTED
    ABI LAYOUT TESTED
    MINIMUM-PREFIX TESTED
    SYMBOL TESTED
    INSTALL HEADER TESTED
    ASAN TESTED
    UBSAN TESTED
    REVIEWED
    PASS

M0-C
    IMPLEMENTED
    COMPILED
    UNIT TESTED
    INTEGRATION TESTED
    DSO LIFETIME TESTED
    QUARANTINE TESTED
    ASAN TESTED
    UBSAN TESTED
    REVIEWED
    PASS

M0-D Runtime Services + ConfigView
    IMPLEMENTED
    ABI C/C++ COMPILE TESTED
    SERVICE/CONFIG TESTED
    PROJECTION/CLOCK TESTED
    FAKE PLUGIN INTEGRATION TESTED
    ASAN TESTED
    UBSAN TESTED
    PASS
```

Validation performed on GCC 11.4.0, CMake 3.22.1, clang-format 14.0.0 and
amd64:

- GCC Debug with `RODAQ_WARNINGS_AS_ERRORS=ON`: build passed; CTest
  `rodaq.compile.error_header`, `rodaq.compile.result_header`, `rodaq.error` and
  `rodaq.result` all passed.
- ASan Debug: build passed; all four CTest entries passed under
  `setarch x86_64 -R`. The ASLR wrapper is required by this Linux 6.8 host to
  avoid a GCC 11 ASan runtime startup failure; the tests also passed directly
  when process address randomization did not collide with the sanitizer shadow
  mapping.
- UBSan Debug with non-recovering undefined behavior: build passed and all four
  CTest entries passed.
- Combined ASan + UBSan Debug with warnings-as-errors: build passed and all
  four CTest entries passed under the same ASLR wrapper.
- `RODAQ_BUILD_TESTS=OFF`, `RODAQ_BUILD_BENCHMARKS=ON` and
  `RODAQ_USE_MIMALLOC=ON`: configure and build passed.
- M0-A install smoke: the installed header tree contained the two Public C++
  headers, expected-lite header and expected-lite license. An external C++17
  consumer compiled with warnings-as-errors using only the installed include
  directory and no RoDAQ source-tree include path. M0-B later extended this tree
  with all eight ABI headers and C/C++ consumers.
- Result selection conflict qualification: configuring expected-lite for
  `std::expected` failed compilation as required.
- Network audit: `strace` recorded no network syscalls during a fresh configure
  and build; source audit found no `FetchContent`, `ExternalProject_Add`,
  configure-time clone or download command.
- Submodule audit: all nine approved submodules and the recursive dependency
  below HdrHistogram_c report clean worktrees at their pinned commits.
- Formatting and scope audit: clang-format 14 dry-run passed for all new C/C++
  files, `git diff --check` passed, whitespace audit passed, and the
  `include/rodaq/plugin/abi/` tree passed forbidden-type isolation checks.

M0-A Review Closure and M0-B validation:

- GCC 11 Debug with warnings-as-errors: 34/34 CTest entries passed.
- C11 and C++17: all eight ABI headers passed independent compilation.
- FROZEN layout: exact `sizeof`, `alignof` and `offsetof` checks passed in both
  C11 and C++17.
- APPENDABLE layout: all 30 minimum-prefix constants passed exact numeric and
  field-offset checks; tail-field gating passed for every minimum field.
- Contract domains: all protocol versions, status/log/diagnostic/lifecycle,
  Admission, payload ownership, control and Storage dual-axis scalar values
  passed.
- Result gates: wrong expected-lite version and conflicting std-selection tests
  failed compilation with their required diagnostics.
- Header isolation: ABI headers contain no STL, third-party or backend types;
  Public C++ headers expose only the D-025 `Result<T>` third-party alias.
- Symbols: hidden-visibility fixtures export exactly the Boot ABI marker and
  their family entrypoint with unmangled C names.
- Installed tree: C ABI, C++ ABI and Result consumers compiled using only the
  installed include tree; expected-lite license was present.
- CMake option matrix: `BUILD_TESTING=OFF` and `RODAQ_BUILD_TESTS=OFF` each
  produced zero test targets; the enabled matrix produced all 34 tests.
- Benchmarks/mimalloc options: benchmarks subtree plus Google Benchmark and
  optional mimalloc configured and built successfully.
- ASan, UBSan and combined ASan+UBSan: 34/34 CTest entries passed in each build;
  ASan runs used `setarch x86_64 -R` for the documented host workaround.

M0-C validation:

- `cmake --build build/debug --parallel 2` followed by
  `ctest --test-dir build/debug --output-on-failure --parallel 2`: 65/65 CTest
  entries passed with GCC 11 Debug and warnings-as-errors.
- `cmake --build build/asan --parallel 2`, `cmake --build build/ubsan --parallel 2`
  and `cmake --build build/asan-ubsan --parallel 2`: all builds passed. Their
  CTest runs each passed 65/65; ASan and combined builds used
  `setarch "$(uname -m)" -R ctest ...` for the documented host workaround.
- Loader rejection matrix passed for missing/wrong Boot ABI, missing/dual/
  mismatched family, wrong protocol, too-small family, null required slot,
  malformed descriptors, invalid ID/SemVer/UTF-8, duplicate Plugin ID and
  dependency-provided symbols.
- `dlinfo` load-base versus `dladdr` defining-object checks rejected both marker
  and family-entrypoint provenance violations.
- Test-only lifecycle event logs verified load/unload/create/destroy ordering,
  exactly-once destroy, same-path catalog merge and final-lease unload.
- Quarantine subprocess verified no subsequent instance creation, no destroy and
  no DSO unload before process exit.

M0-D validation:

- GCC 11.4.0 Debug with warnings-as-errors: all 70 CTest entries passed.
- ASan Debug with warnings-as-errors: all 70 CTest entries passed directly.
- UBSan Debug with non-recovering undefined behavior and warnings-as-errors:
  all 70 CTest entries passed.
- The Projection multi-reader/serialized-writer test passed under TSan using
  `setarch "$(uname -m)" -R`; unwrapped execution hit the documented host
  `unexpected memory mapping` runtime limitation.
- `BUILD_TESTING=OFF` and `RODAQ_BUILD_TESTS=OFF` each configured and built
  with zero RoDAQ tests.
- Fake Acquisition/Storage DSO integration verified create → bind →
  configure → destroy ordering, required-service absence, wrong protocol,
  too-small service table and null required service slot failures.
- Fake integration verified Runtime-owned ConfigView consumption, projection
  producer/consumer byte exchange and stable activation clock-anchor lookup.
- Installed-header smoke passed with the additive projection and
  clock-correlation ABI.
- Failed load/create paths passed under ASan without leaked DSO handles or
  plugin-code use after unload.
- `clang-format --dry-run --Werror` over `src/plugin/` and `tests/plugin/`,
  `git diff --check`, and the whitespace scan all passed without findings.
- `git submodule status --recursive` reported all approved pins, and the
  recursive per-submodule worktree check reported no modifications.
- No `FetchContent`, `ExternalProject_Add`, clone, curl, wget or download command
  was found in the build, source or test trees. `cmake/` and `third_party/`
  have no worktree changes; the intentional public ABI convergence is recorded
  above.
- Naming audit found no forbidden placeholder namespaces, former family owner
  class names, ABI-prefixed error helper, or legacy status conversion references.
