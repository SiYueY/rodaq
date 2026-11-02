# RoDAQ Development Status

**Updated:** 2026-11-02
**Branch:** `develop`
**Base HEAD:** `b180a5cc0b3e` — `docs: expand RoDAQ design and coding guidelines`

This file records factual current repository state. Keep it concise and update it after meaningful implementation or validation work.

## Current milestone

**M0 — Foundation + Plugin ABI**

Status: **ACTIVE**

The design phase is sufficiently mature to begin implementation. The first implementation target is the common foundation and Plugin ABI/infrastructure, not ROS 2 or MCAP integration directly.

## Current repository state

Implementation skeleton has not been created yet.

Currently missing:

- top-level `CMakeLists.txt`
- `include/`
- `src/`
- `apps/`
- `tests/`
- `benchmarks/`
- `cmake/`

`third_party/` exists but currently contains no pinned submodules.

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

## Not started

- CMake/project skeleton.
- Third-party submodules.
- `rodaq::Result<T>` implementation/backend decision in code.
- C ABI headers.
- ABI layout/symbol tests.
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

This is a static design-readiness result only. No production code/build/sanitizer/backend round-trip has been executed yet. ROS2/MCAP executable qualification remains an M3 gate, and DDS/SQLite3 qualification remains an M5 gate.

One early implementation decision should be resolved before `Result` spreads through the codebase:

- use/adapt the already-reviewed RoDAQ-style frozen `Result<T, E>` implementation, or
- use `expected-lite` only as a private backend behind `rodaq::Result<T>`.

Do not expose `nonstd::expected` in Public API.

## Immediate next actions

1. Establish top-level CMake/project skeleton.
2. Add and pin approved third-party submodules.
3. Finalize `rodaq::Result<T>` implementation choice.
4. Implement the initial C ABI headers.
5. Add ABI layout and symbol tests immediately.
6. Implement SharedLibrary / PluginLibrary.
7. Implement minimal Service Registry and ConfigView.
8. Build Fake Acquisition / Storage loading/lifecycle tests.

## Validation state

No implementation build or runtime tests have been run yet because production code has not been created.

The next status update must distinguish:

- implemented,
- compiled,
- unit-tested,
- integration-tested,
- sanitizer-tested,

instead of using "done" for unvalidated work.
