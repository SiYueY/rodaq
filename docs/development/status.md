# RoDAQ Development Status

**Updated:** 2026-11-02
**Branch:** `develop`
**Base HEAD:** `404da89e76cd31142aaa293df79da090a79a508f` — `docs: freeze RoDAQ V1 design baseline`

This file records factual current repository state. Keep it concise and update it after meaningful implementation or validation work.

## Current milestone

**M0 — Foundation + Plugin ABI**

Status: **ACTIVE**

The design phase is sufficiently mature to begin implementation. The first implementation target is the common foundation and Plugin ABI/infrastructure, not ROS 2 or MCAP integration directly.

## Current repository state

M0-A Review Closure and M0-B Stable C Plugin ABI are complete and validated.
The repository now contains:

- a C++17 CMake project foundation,
- warning, sanitizer, CTest, dependency and install support,
- Public `rodaq::Error` and `rodaq::Result<T>` primitives,
- eight Stable C ABI headers under `include/rodaq/abi/`,
- C11/C++17 ABI compilation, layout, minimum-prefix and symbol qualification,
- pinned dependency submodules with no normal-build network access,
- header-only install validation for `rodaq::Error` and `rodaq::Result<T>`.

`third_party/` contains all nine approved pinned submodules. No Plugin Loader,
linkable RoDAQ package target or production shared library has been introduced.

## Design baseline

Current system baseline:

- `docs/design/rodaq_v1_system_design.md` — V1 Detailed System Design / Implementation Baseline.
- `docs/design/rodaq_plugin_system.md` — V1 Plugin System Detailed Design / Freeze Candidate.
- `docs/design/architecture.md` — v0.5.
- `docs/design/runtime.md` — v0.5.
- `docs/design/canonical_data_model.md` — v0.9.
- `docs/design/canonical_profile.md` — v0.5.
- `docs/design/admission.md` — v0.5.
- `docs/design/payload.md` — v0.5.
- `docs/design/acquisition_plugin.md` — v0.6, Plugin ABI Reconciliation Candidate.
- `docs/design/acquisition_plugin/ros2.md` — v0.4, Plugin ABI Mapping Review Candidate.
- `docs/design/acquisition_plugin/dds.md` — v0.4, Plugin ABI Mapping Review Candidate.
- `docs/design/storage_plugin.md` — v0.4, Plugin ABI Reconciliation Candidate.
- `docs/design/storage_plugin/mcap.md` — v0.5, Plugin ABI Mapping Review Candidate.
- `docs/design/storage_plugin/sqlite3.md` — v0.4, Plugin ABI Mapping Review Candidate.

## V1 implementation scope

Design in V1:

- ROS 2 Acquisition.
- DDS Acquisition.
- MCAP Storage.
- SQLite3 Storage.

Implement first:

- ROS 2 Acquisition.
- MCAP Storage.

The common Runtime and Plugin infrastructure must be validated with Fake Plugins before the first real backend vertical slice.

## Completed

- Canonical data model and V1 profile have undergone multiple design reviews.
- Runtime, Admission, Payload, Acquisition Plugin and Storage Plugin design documents exist.
- Non-compressed V1 system design baseline has been produced.
- Detailed Plugin System design has been produced.
- V1 development stages have been unified into one authoritative M0–M5 milestone model; the system blueprint defines the Gate semantics and `docs/development/plan.md` tracks execution state.
- Plugin System / generic Acquisition / generic Storage / ROS2 / DDS / MCAP / SQLite3 documents have been reconciled to the same stable ABI semantics before M0 implementation.
- Plugin architecture direction has converged on:
  - stable C ABI,
  - C++17 authoring SDK,
  - family-specific Acquisition / Storage protocols,
  - Boot ABI marker,
  - Service Registry,
  - Extension Query,
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
  - `abi.h`, `canonical.h`, `service.h`, `config.h`, `runtime.h`,
    `acquisition.h`, `storage.h` and `plugin.h`,
  - required Service and control-extension ID constants,
  - all 30 V1 APPENDABLE minimum-prefix constants,
  - FROZEN layout sentinels,
  - C11 and C++17 independent header compilation,
  - hidden-visibility Acquisition and Storage symbol fixtures,
  - installed C, C++ ABI and Result consumer qualification.

## Not started

- SharedLibrary / PluginLibrary / PluginManager.
- Service Registry implementation.
- ConfigView implementation.
- Minimal C++ Plugin SDK.
- Runtime ResourceBudget / Admission implementation.
- Payload / PayloadPool implementation.
- Dispatch / Completion implementation.
- Fake Acquisition / Storage.
- ROS 2 Acquisition implementation.
- MCAP Storage implementation.
- DDS Acquisition implementation.
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
required Acquisition / Storage control extensions
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

The Stable C ABI is implemented and mechanically qualified. No Plugin Loader,
Runtime, Plugin lifecycle or backend round-trip has been executed. ROS2/MCAP
executable qualification remains an M3 gate, and DDS/SQLite3 qualification
remains an M5 gate.

The Result policy is now frozen by D-025:

- `rodaq::Result<T>` directly aliases `nonstd::expected<T, Error>`.
- expected-lite 0.10.0 is a pinned Public C++ header dependency.
- V1 always uses expected-lite's nonstd implementation.
- expected-lite never enters the Stable C Plugin ABI.

## Immediate next actions

1. Begin M0-C — Plugin Loading + Lifetime.
2. Implement SharedLibrary / PluginLibrary / PluginManager.
3. Implement actual Boot ABI, protocol, symbol and duplicate-ID rejection.
4. Implement PluginLibraryLease / PluginInstanceLease.
5. Implement minimal Service Registry and ConfigView.
6. Build Fake Acquisition / Storage loading/lifecycle tests.

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
  `include/rodaq/abi/` tree passed forbidden-type isolation checks.

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
