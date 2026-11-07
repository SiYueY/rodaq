# RoDAQ V1 Development Plan

This file is the cross-agent execution plan.

The canonical milestone boundaries and Exit Gates are defined in:

```text
docs/design/rodaq_v1_system_design.md
    Chapter 6 — Development and Verification Roadmap
```

This file tracks execution state. It must use the same milestone names and may not redefine weaker acceptance criteria.

## Milestone overview

```text
M0  Foundation + Plugin ABI
        ↓
M1  Runtime Core Primitives
        ↓
M2  Fake End-to-End Runtime
        ↓
M3  ROS2 Acquisition + MCAP Storage Vertical Slice
        ↓
M4  FastDDS Acquisition + MCAP Interoperability
        ↓
M5  Production Hardening + Release Qualification
```

Current milestone: **M2 ACTIVE — Status/Result/Logging infrastructure
refactor; M3 NOT STARTED**

Only one milestone may be `ACTIVE` at a time.

### Non-plugin architecture execution rule

The current non-plugin design baseline is `docs/design/architecture.md`,
`runtime.md`, `admission.md`, `canonical_profile.md` and
`recording_service.md`. Implementation contracts distinguish:

- `Core Runtime`: activation-local Canonical, Admission, Payload, Storage
  coordination, bounded progress and safety; no Task Ledger, Retention,
  historical Artifact Catalog or process restart policy in `ActivationState`.
- `BoundedRecording`: a service that can stay online and execute finite
  recording tasks; each Activation produces a separate Artifact/Scope, and
  Start/Stop gaps are explicit.
- `ContinuousRecording`: uninterrupted rolling with cross-segment
  lineage and bounded retention. This is **not supported** by the present
  V1 ABI/Artifact contract and is a future design gate, not an implicit
  M3/M5 completion claim.

Execution order follows `recording_service.md` `7.3: pre-M3 Runtime
mutation coordination; M3/M4 real-chain evidence; M5 production-service
implementation and qualification **if** the release claims industrial
unattended `BoundedRecording`. Core-library/finite-task releases may stay
narrower, but must explicitly state the limitation and must not claim 7x24
production Recording Service or `ContinuousRecording`. Unchecked tasks
below remain NOT STARTED until implementation and evidence are recorded.

---

## Status, Result & Logging Infrastructure Refactor

**Status:** ACTIVE — implementation and targeted verification complete; full
Qualification matrix pending.

This source-breaking pre-release refactor temporarily reopens M2. Stable C ABI
struct layouts, offsets, scalar values and protocol versions remain unchanged;
the ABI severity macro source names change from `RODAQ_LOG_*` to
`RODAQ_ABI_LOG_*` and require all consumers to rebuild.

- [x] Replace expected-lite with tl::expected 1.3.1 at
  `1770e3559f2f6ea4a5fb4f577ad22aeb30fbd8e4`.
- [x] Install `tl/expected.hpp` and license; remove expected-lite target,
  installed header, selection tests and SBOM entry.
- [x] Adapt rocomm `Result<T>` source directly and make Status header-only with
  rocomm-style `Ok()/code()` plus RoDAQ `domain()/error_code()` extensions.
- [x] Apply strong Status/Result abort contracts, no-value Result, rvalue
  dangling-access deletion, no implicit Result boolean conversion and compile
  restrictions for array/cv/`Result<Status>`.
- [x] Add public `logging.hpp` / `logging_config.hpp`, Running-only
  `DAQ::logger()`, weak Logger handles and Activation-owned LoggingContext.
- [x] Implement Sync/AsyncBounded, record/byte bounds, all three overflow
  policies, Console/File/RotatingFile/DailyFile sinks, level/pattern/time-zone
  settings, periodic/level flush, backtrace, formatting isolation, reentrancy
  drop, statistics and bounded quarantine.
- [x] Freeze `RODAQ_LOG_*` public macros and rename ABI severity constants to
  `RODAQ_ABI_LOG_*` without compatibility aliases.
- [x] Make `fmt::fmt` and `spdlog::spdlog` public dependencies of `rodaq::daq`;
  install exact package closure and resolve it with `find_dependency()`.
- [x] Keep `rodaq::sdk` independent of DAQ, fmt, spdlog and YAML.
- [x] Add targeted Status/Result, compile-fail, logging backend/context/logger,
  DAQ lifecycle, Plugin severity mapping, ABI/install and symbol checks.
- [x] Adapt Result directly from rocomm and migrate Status to its header-only
  `Ok()/code()` API with RoDAQ diagnostic extensions.
- [x] GCC 11.4 Debug with warnings-as-errors: **108/108 CTest PASS** before the
  final exception-rollback hardening; affected logging tests rerun **2/2 PASS**.
- [ ] Run the complete final Qualification matrix and record evidence.
- [ ] Restore `M2 = PASS / Core FROZEN` only after that evidence is complete.

---

## Core Implementation Structure & Naming Closure (pre-M3)

**Status:** PASS — local Debug, Release, sanitizer, Ubuntu 24.04,
installation, ABI, package and capacity qualification complete. Hosted CI
rerun remains pending. M3 is NOT STARTED.

This is a breaking source-layout and naming closure before M3. It does not add
backend functionality or change Runtime data-path semantics.

- [x] Public API moves to `rodaq::DAQ`, `DAQConfig`, `DAQState`, `DAQStats`,
  `DAQLatencyStats` and `DAQLifecycleSnapshot`.
- [x] Public C++ lifecycle and config views no longer expose Stable C ABI types.
- [x] C++/YAML configuration uses `acquisition_id`; the frozen Storage ABI keeps
  `rodaq_storage_open_request.session_id`.
- [x] Internal C++ namespace defaults to `rodaq`; Plugin subsystem implementation
  uses `rodaq::plugin`; no `detail`, `internal` or `impl` namespace is used.
- [x] `DAQ::Impl` is one private type split across `daq.cpp`, `lifecycle.cpp` and
  `recovery.cpp`.
- [x] `ActivationState` composes independently owned `CanonicalState`,
  `BindingState`, `AdmissionState` and `ActivationMetrics`.
- [x] `CanonicalState` owns Canonical registration/lookup/availability state;
  `BindingState` owns Binding lifecycle/publication; `AdmissionState` owns Gate,
  submission/in-flight accounting and sequence allocation.
- [x] `AcceptedRecord`, `TimestampState` and `AcceptanceSequenceAllocator` have
  separate Record, Canonical and Admission ownership.
- [x] Activation uses `ActivationPhase`; immutable `DAQConfig` is separate from
  negotiated `ActivationLimits`.
- [x] Config validation lives in `config/validation.*`.
- [x] `InterfaceRef`, `ServiceRegistryRef`, `ConfigNodeRef` and Canonical
  `*_info` source names are frozen without aliases.
- [x] Plugin service adapters are split by Common, Canonical, Acquisition and
  Storage protocol; registration remains in `activation_services.cpp`.
- [x] The thin activation Logging wrapper is removed.
- [x] Source and test trees no longer contain `runtime/`, `sdk/`, `failure/`,
  `resource/` or `observability/` implementation directories.
- [x] ABI source identifiers, macro names and protocol identifiers are
  version-free; numeric `protocol_version` negotiation remains.
- [x] Shared DAQ target is `librodaq.so` / `rodaq::daq`; implementation SDK is
  `rodaq::sdk`; Plugin Host implementation remains private.
- [x] Installed DAQ, Plugin SDK and combined consumers pass.
- [x] `RoDAQTargets` exports only `rodaq::daq` and `rodaq::sdk`.
- [x] Public `DAQ`, `Status`, Config model and YAML symbols are exported; internal
  implementation symbols remain hidden.
- [x] Canonical, Binding, Admission, Activation, Metrics, Logging and descriptor
  ownership tests are present.
- [x] Automated source path/name residue check is in CTest.
- [x] Default Debug warnings-as-errors: **105/105 CTest PASS**.
- [x] Release with tests and benchmarks enabled: **105/105 CTest PASS**.
- [x] GCC 11 ASan+LSan and UBSan full CTest: **105/105 PASS** in both builds.
- [x] Ubuntu 24.04 full CTest: **105/105 PASS**.
- [x] Clang 14 TSan selected concurrency suites: **16/16 PASS** with ASLR
  disabled for the container.
- [x] Steady-state and capacity benchmarks rerun with recorded comparison.
- [x] clang-format and clang-tidy project checks PASS.
- [x] Dependency inventory/SBOM and exported-symbol audits.

Completion restores M2 as the current completed milestone and leaves M3 NOT
STARTED until a separate M3 task is authorized.

---

## Pre-M3 Non-plugin Runtime Contract Gate

**Status:** NOT STARTED / pending implementation and validation.
**Owner:** Runtime / Storage coordination (not a concrete Plugin).
**Source:** `docs/design/runtime.md` `8.2.1 and `16.34; `recording_service.md` `7.3.

This is an entry gate within the existing M2 → M3 transition, **not**
a separate active milestone or a new M2-wide rearchitecture.

- [ ] Finish current M2 Status/Result/Logging requalification and restore
  `M2 = PASS / Core FROZEN` before M3 begins.
- [ ] Audit all Runtime → Storage mutation entry points:
  `ensure_descriptor_ready`, `submit`, `flush` and `close`, including
  descriptor reconciliation, dispatch, lifecycle, failure and quarantine.
- [ ] Enforce the default **non-overlap of unauthorized Storage mutation
  calls**, or explicitly prove concrete Storage Contract concurrency
  capabilities. Separate internally serial workers are not proof of
  one logically serialized instance.
- [ ] Preserve descriptor-ready-before-binding/first-record,
  same-Stream accepted handoff order and early completion
  reconciliation. Avoid inline-callback deadlocks.
- [ ] Test slow, blocked and late-returning mutations; timeout must fail
  closed, retain no-future-access anchors and never start an unsafe
  second mutation against the timed-out first call.
- [ ] Add Fake Storage overlap detection and interleaving tests:
  dynamic descriptor evolution while older Streams submit, early/late
  completions, Stop, failure cutoff, flush/close and quarantine.
- [ ] Record a focused gate report plus affected Runtime/ABI tests.
  If a public contract change is necessary, revise the owning
  designs and decisions before implementing it.

**Exit Gate:** No unproven cross-thread Storage mutation overlap, no
readiness/handoff/order regression, and timeout/failure maintains safe
resource ownership. Follow D-066 for incremental vs final qualification.

---

## M0 — Foundation + Plugin ABI

**Status:** PASS

**Current sub-phase:** **M0 Final Contract Correction — PASS**. Stable ABI core
prefix is frozen. Native arm64 qualification is deferred by D-034.

### Goal

Create the buildable project foundation and implement the Stable Plugin boundary before any real backend is coupled to Runtime.

### Prerequisites

- [x] V1 system design baseline exists.
- [x] Plugin System Freeze Candidate exists.
- [x] Acquisition / Storage Plugin contracts reconciled with Plugin System.
- [x] ROS2 / FastDDS / MCAP required V1 mappings aligned to Stable ABI; SQLite3 retained as future architecture-validation mapping.
- [x] R1 — Canonical ABI Contract Reconciliation PASS.
- [x] R2 — Control & Lifecycle Contract Reconciliation PASS.
- [x] R3 — ABI Mechanics Contract Reconciliation PASS.
- [x] Pre-implementation P1 engineering/document reconciliation completed.
- [x] Second independent review FR-01..FR-12 repaired.
- [x] Consolidated post-repair design review PASS — no remaining known P0/P1 static design blocker to M0.
- [x] Shared agent context exists:
  - [x] `AGENTS.md`
  - [x] `docs/development/status.md`
  - [x] `docs/development/decisions.md`
  - [x] `docs/development/plan.md`

### Scope

#### Project / CMake

- [x] Create top-level `CMakeLists.txt`.
- [x] Create `include/`.
- [x] Create `src/`.
- [x] Create `apps/`.
- [x] Create `tests/`.
- [x] Create `benchmarks/`.
- [x] Create `cmake/`.
- [x] Establish the header-install skeleton and package/export extension point;
  linkable package export remains deferred.
- [x] Add warning configuration.
- [x] Add sanitizer options.
- [x] Integrate CTest.

#### Third-party baseline

- [x] Add approved Git submodules under `third_party/`.
- [x] Pin each submodule by commit.
- [x] Implement `cmake/dependencies.cmake`.
- [x] Verify no normal build-time network fetching.
- [x] Verify the Stable C ABI exposes no third-party types.
- [x] Verify the Public C++ API exposes no third-party types except the
  D-025-frozen `rodaq::Result<T>` alias.

Required production dependency pins:

- [x] spdlog 1.17.0.
- [x] fmt 12.1.0.
- [x] HdrHistogram_c 0.12.1.
- [x] yaml-cpp 0.9.0.
- [x] tl::expected 1.3.1 as the pinned private Result implementation dependency.

Evaluated but not selected for V1:

- [x] oneTBB 2022.3.0 evaluated; removed from the V1 baseline.
- [x] mimalloc 3.6.1 evaluated; removed from the V1 baseline.

Development:

- [x] GoogleTest 1.18.0.
- [x] Google Benchmark 1.10.0.

Dependency lifecycle is tracked separately:

```text
PINNED
BUILT
LINKED
USED
QUALIFIED
```

Current required-dependency states:

```text
tl_expected      PINNED + BUILT + LINKED + USED + QUALIFIED
HdrHistogram_c   PINNED + BUILT + LINKED + USED + QUALIFIED
spdlog           PINNED + BUILT + LINKED + USED + QUALIFIED (public DAQ CMake dependency)
fmt              PINNED + BUILT + LINKED + USED + QUALIFIED (public DAQ CMake dependency)
yaml-cpp         PINNED + BUILT + LINKED + USED + QUALIFIED (ConfigLoader)
oneTBB           EVALUATED + NOT-SELECTED; removed from V1 baseline
mimalloc         EVALUATED + NOT-SELECTED; removed from V1 baseline
GoogleTest       BUILT + LINKED + USED + QUALIFIED (test only)
Google Benchmark BUILT + LINKED + USED + QUALIFIED (benchmark only)
```

#### Result / Error

- [x] Finalize `rodaq::Result<T>` as a RoDAQ wrapper over private tl::expected storage.
- [x] Pin and mechanically verify tl::expected 1.3.1.
- [x] Keep tl::expected out of the Stable C ABI.
- [x] Implement/freeze error representation used by Public C++ APIs.

#### Stable ABI

- [x] Finalize the Plugin ABI header namespace/path.
- [x] Rename the common ABI primitive header to `common.h`.
- [x] Implement `include/rodaq/plugin/abi/common.h`.
- [x] Implement `canonical.h`.
- [x] Implement `plugin.h`.
- [x] Implement `service.h`.
- [x] Implement `config.h`.
- [x] Implement `runtime.h`.
- [x] Implement `acquisition.h`.
- [x] Implement `storage.h`.
- [x] Re-run full M0-B mechanical qualification after the ABI path convergence.
- [x] Finalize `query_interface()` terminology and required control interface IDs.
- [x] Move `rodaq_storage_operation` to Storage ABI ownership.
- [x] Unify Storage completion, systemic failure and Descriptor readiness diagnostics.

#### ABI tests

- [x] C compilation tests for every ABI header.
- [x] C++17 compilation tests for every ABI header.
- [x] `sizeof` sentinels.
- [x] `alignof` sentinels.
- [x] `offsetof` sentinels.
- [x] All 30 MIN_STRUCT_SIZE tests.
- [x] Tail-field gating tests.
- [x] Scalar and protocol value tests.
- [x] Public ABI header isolation test.
- [x] Installed C/C++/Result consumer tests.
- [x] export-symbol allowlist test with good family fixtures.
- [x] wrong Boot ABI fixture.
- [x] wrong family protocol fixture.
- [x] too-small struct fixture.
- [x] missing-symbol fixture.
- [x] duplicate Plugin ID fixture.
- [x] symbol provenance test.

#### Plugin infrastructure

- [x] Implement `SharedLibrary`.
- [x] Implement `PluginLibrary`.
- [x] Implement `PluginCatalog`.
- [x] Implement minimal `PluginManager`.
- [x] Implement `AcquisitionPluginInstance`.
- [x] Implement `StoragePluginInstance`.
- [x] Implement `PluginLibraryLease`.
- [x] Implement `PluginInstanceLease`.
- [x] Reject Acquisition/Storage instance creation through the wrong family lease.
- [x] Implement Service Registry.
- [x] Add `rodaq.runtime.projection` service ABI/plumbing for typed descriptor projections.
- [x] Add `rodaq.runtime.clock_correlation` read-only service ABI/plumbing for activation replay-time anchor.
- [x] Implement ConfigView.
- [x] Implement Plugin Interface Query plumbing.
- [x] Implement minimal C++17 Plugin SDK.
- [x] Implement exception trampolines.
- [x] Implement Plugin metadata generation.
- [x] Implement Plugin authoring CMake helper.

#### Minimal Fake DSOs

- [x] Fake Acquisition loads and validates.
- [x] Fake Storage loads and validates.
- [x] create/bind/configure/destroy lifecycle works for the M0-D service integration scope.
- [x] create/destroy lifetime works for the M0-C fixture scope.
- [x] DSO unload occurs only after last lease.
- [x] Catalog removal preserves live instance and library-lease lifetime.

### Deliverables

- [x] Clean buildable repository skeleton.
- [x] Pinned dependency graph.
- [x] M0-B core/family Stable ABI headers.
- [x] M0-D additive Runtime service ABI headers for projection and clock-correlation services.
- [x] ABI mechanical tests.
- [x] Plugin loader/infrastructure for the M0-C boundary.
- [x] Service Registry / ConfigView.
- [x] Minimal C++ Plugin SDK.
- [x] Minimal Fake Acquisition / Storage DSOs.
- [x] Initial ABI compatibility baseline.

### Verification

#### Acquisition ABI

- M0 qualifies ABI representability and interface semantics. Actual ownership
  transfer, Binding routing and acceptance sequencing behavior is assigned to
  M1/M2.
- [x] `AdmissionOutcome` representable.
- [x] `RejectionReason` representable.
- [x] `PayloadDisposition` representable.
- [x] Rejection reason is a separate ABI field and does not encode ownership.
- [x] M1: ADOPT before/after transfer ownership behavior.
- [x] M1: RETAIN acquisition-point and original-native-owner behavior.
- [x] M2: Gate-pass/AcceptanceSequence ordering and PreAdmissionRejected behavior.
- [x] M1: Clock/Stream native registration idempotence, conflict and generation behavior.
- [x] M1: identical native_id in different Plugin instances remains isolated.

#### Storage ABI

- M0 qualifies ABI representability. Handoff reconciliation and asynchronous
  completion behavior is assigned to M1/M2.
- [x] M2: submit non-OK means HandoffAccepted never established.
- [x] PayloadConsumed synchronous result representable.
- [x] PayloadPending representable.
- [x] PersistenceSucceeded synchronous result representable.
- [x] PersistenceFailed synchronous result representable.
- [x] PersistenceUnknown synchronous result representable.
- [x] PersistencePending representable.
- [x] Asynchronous/systemic Storage diagnostics use complete `rodaq_error` values.
- [x] Descriptor readiness FAILED carries complete diagnostics.
- [x] M2: no same-stack reentrant callback; cross-thread completion race reconciliation.
- [x] M2: async publication and early-completion buffering rules.

#### Service / Config / lifetime

- [x] required Service success/failure.
- [x] optional Service absence.
- [x] required `rodaq.acquisition.control` present/compatible.
- [x] required `rodaq.storage.control` present/compatible.
- [x] missing/incompatible required control interface prevents activation.
- [x] M2: `quiesce_plugin()` blocks current/future Submit routing.
- [x] M2: normal Stop quiesce → drain → destructive stop behavior.
- [x] M2: Binding abort KEEP_PAUSED / RESUME_OLD and terminal transition state.
- [x] M1/M2: Runtime lifecycle snapshot recovery gating and stale-state rejection.
- [x] protocol/struct-size validation.
- [x] caller-owned APPENDABLE output: old/new caller/callee compatibility.
- [x] flags/reserved validation.
- [x] ConfigView MAP/LIST/scalar tests.
- [x] ConfigView exact-type conversion.
- [x] ConfigView out-of-range index → NOT_FOUND.
- [x] caller-owned `rodaq_error` remains readable after ABI return.
- [x] Plugin descriptor / entrypoint object lifetime tied to PluginLibraryLease.
- [x] Plugin exception firewall.
- [x] raw noexcept violation subprocess death test.
- [x] M2: control timeout/quiescence, stream isolation, overlapping mutation and quarantine behavior.
- [x] live instance prevents unload.
- [x] live lease prevents unload.
- [x] relevant ASan/UBSan suite clean.

#### M0 Closure Repair

- [x] All callable Plugin SDK entrypoints audited for required pointers, views,
  counts, flags and reserved fields.
- [x] Storage Open / Submit structural validation added before Plugin C++
  implementation entry.
- [x] Acquisition control and Runtime Service request/input validation added.
- [x] All public Runtime Service wrappers are default-invalid safe.
- [x] `MonotonicTimeNs()` returns typed `InvalidState`.
- [x] Frozen V1 Storage output definitions are independent of current ABI
  headers.
- [x] Storage / Projection / Clock APPENDABLE capacity matrices and canaries
  pass.
- [x] Existing current-host / frozen-Plugin fixtures remain passing.
- [x] CMake and Runtime SemVer validators share the same accepted/rejected
  boundary corpus.
- [x] Storage Submit / Capabilities / Descriptor readiness 3x3 exception matrix
  passes through actual ABI entrypoints.
- [x] Ubuntu 22.04 normal / ASan / UBSan and Ubuntu 24.04 normal amd64
  qualification each pass 79/79.

### Exit Gate

M0 PASS only when the authoritative M0 Exit Gate in `rodaq_v1_system_design.md` is satisfied.

Evidence to record before marking PASS:

- [x] build commands/results.
- [x] ABI test results.
- [x] Fake Plugin lifecycle results.
- [x] Service/Config tests.
- [x] lifetime tests.
- [x] sanitizer results.
- [x] no unresolved P0/P1 Plugin ABI conflict.

### Out of Scope

Do not implement in M0:

- full Admission/ResourceBudget pipeline,
- full PayloadPool,
- Dispatch/Completion Domain,
- ROS2 data acquisition,
- FastDDS data acquisition,
- MCAP writing,
- SQLite3 persistence,
- production performance tuning,
- hot reload.

### Immediate task order

1. [x] CMake/project skeleton.
2. [x] Third-party submodules/dependency CMake.
3. [x] `Result/Error`.
4. [x] ABI headers + ABI tests.
5. [x] `SharedLibrary / PluginLibrary`.
6. [x] Service Registry / ConfigView and M0-D Runtime services.
7. [x] C++ Plugin SDK/trampolines.
8. [x] Minimal Fake Plugin lifecycle fixtures.

---

## M1 — Runtime Core Primitives

**Status:** PASS

### Goal

Implement bounded, independently testable Runtime correctness primitives.

### Prerequisites

- [x] M0 PASS.

### Scope

- [x] Handles.
- [x] bounded Registry.
- [x] Structural Arena.
- [x] ResourceBudget.
- [x] all-or-nothing Reservation.
- [x] Admission Gate / generation.
- [x] Reject / interruptible Wait / deadline.
- [x] Payload.
- [x] PayloadView.
- [x] PayloadPool.
- [x] AcceptedRecord.
- [x] StatsAccumulator.
- [x] LatencyHistogram.
- [x] Logger facade.

### Deliverables

- [x] Unit-tested Handle/Registry implementation.
- [x] ResourceBudget/Reservation implementation.
- [x] Payload ownership implementation.
- [x] bounded PayloadPool.
- [x] metrics/high-water primitives.
- [x] stress/sanitizer targets.

### Verification

- [x] resource exhaustion.
- [x] partial reservation rollback.
- [x] concurrent waiters.
- [x] timeout.
- [x] Stop wakeup.
- [x] Failure wakeup.
- [x] Gate generation change.
- [x] Copy lifetime.
- [x] Adopt transfer-point cases.
- [x] Retain acquisition-point cases.
- [x] exactly-once release.
- [x] retained byte accounting.
- [x] Payload object exhaustion.
- [x] no unbounded allocator fallback.
- [x] ASan clean.
- [x] UBSan clean.
- [x] TSan clean for required concurrency tests.

M1 ResourceBudget covers the configured `records` and logical `bytes`
dimensions required by these primitives. Dispatch, completion and Storage
in-flight dimensions are added with their owning M2 mechanisms.

### Exit Gate

- [x] All configured critical resources proven bounded.
- [x] Reservation rollback proven non-failing and leak-free.
- [x] Wait paths proven Stop/Failure interruptible.
- [x] Payload ownership tests pass.
- [x] Registry/Handle tests pass.
- [x] required sanitizers clean.
- [x] no backend-specific type in Runtime primitives.

### Out of Scope

Do not implement in M1:

- complete E2E Pipeline,
- Storage Completion Domain,
- real backend data path,
- release performance tuning.

---

## M2 — Fake End-to-End Runtime

**Status:** PASS — coding standards local qualification; new hosted CI pending

**Current sub-phase:** **C++ coding standards review — COMPLETE / local PASS**.
M2 Ownership Closure remains COMPLETE / PASS.
Previous M2 Final Closure Fix remains COMPLETE / PASS.

- [x] FR-01 automatic failure convergence and dispatch cutoff.
- [x] FR-02 dynamic fatal Descriptor readiness escalation.
- [x] QF-01 eventual Quarantine Join with TSan thread-leak reporting.
- [x] Full local qualification matrix on implementation `fe4f938`; native
  arm64 DEFERRED by D-034. Subsequent hosted qualification on `3adefb28` passed;
  see the latest Ownership Closure evidence below.

Evidence: final closure appendix in `review_report.md` and the comparison in
`capacity_qualification.md`. M3 starts only as a separate development task.

### Goal

Prove Runtime correctness end to end with Fake Plugins before introducing real middleware/storage complexity.

The completed V1 Runtime Foundation Closure extends that goal with the standard
camera/joint capacity profiles, dependency lifecycle evidence, bounded logging,
configuration frontend and reference-driven implementation audits required
before M3.

### Prerequisites

- [x] M0 PASS.
- [x] M1 PASS.

### Scope

#### Runtime Pipeline

- [x] Binding validation.
- [x] Ingress.
- [x] Admission.
- [x] Payload materialization.
- [x] Gate Pass.
- [x] AcceptanceSequence.
- [x] ordered Accepted publication.
- [x] Dispatch Queue.
- [x] single logical Storage Dispatch Lane.
- [x] Storage operation tracking.
- [x] Completion Queue.
- [x] Completion Domain.
- [x] Stop.
- [x] Failure cutoff.
- [x] Quiescence.

#### M2 Closure Fix

- [x] Descriptor publication returns before Projection-dependent readiness.
- [x] Projection registration retries Descriptor readiness without Plugin-under-lock.
- [x] Activation-local bounded and deduplicated Descriptor reconciliation worker.
- [x] Initial Descriptor reconciliation completes before the Admission Gate opens.
- [x] Dynamic Descriptor Binding publication revalidates Runtime state and
  transition identity.
- [x] Ingress timestamp capture precedes Admission Wait and Payload materialization.
- [x] Nested readiness/Binding/commit failures are not duplicated into secondary history.
- [x] Storage handoff Watchdog identifies each Submit by a monotonic call generation.
- [x] Registry insert release-publishes a constructed slot and stable readers acquire-observe it.
- [x] TimestampKeySet publication rejects duplicate `(RoleIdentifier, ClockId)` keys.
- [x] Projection registration defers reconciliation outside the Acquisition Plugin-local lock.
- [x] Acquisition Binding prepare/commit/abort callbacks are deadline-supervised.
- [x] Runtime-owned activation ingress Clock and ClockCorrelation service.
- [x] Mandatory ingress timestamp separated from transport timestamps.
- [x] Completion bookkeeping capacity included in admitted-work capacity.
- [x] Descriptor readiness and Storage handoff progress deadlines.
- [x] Multi-Acquisition startup rollback and partial-start cleanup.
- [x] Bounded, owned and reaped quarantine workers and activation roots, with a
  documented 32-root process-wide cap covering healthy and quarantined activations.
- [x] Start failure primary/secondary history preservation.

#### Standard Workload Closure

- [x] Multi-size, independently budgeted PayloadPool classes.
- [x] Per-class Payload object/byte high-water metrics.
- [x] Dispatch/operation/completion/class capacities derived from peak rate,
  tolerated stall and recorded safety factor.
- [x] Production encoded-camera payload profile and stream scheduling benchmark.
- [x] Production aggregated and stress per-joint telemetry benchmark definitions.
- [x] Rate-controlled exclusive joint producer mode.
- [x] 10-camera × 30 FPS × 720P encoded generated-capacity benchmark.
- [x] Raw RGB8 camera stress benchmark.
- [x] Production encoded-camera + aggregated joint mixed benchmark.
- [x] Stress raw-camera + per-joint mixed benchmark.
- [x] Production aggregated joint 1k records/s Fake capacity PASS.
- [x] Stress per-joint 100k records/s Fake capacity PASS.
- [x] Production/stress mixed profiles bounded high-water PASS.
- [x] Reference-driven implementation audit records for ResourceBudget, Arena,
  PayloadPool, Registry, Lifecycle and Plugin loading.

#### Shared Runtime Foundation Closure

- [x] Runtime-owned bounded LoggingContext with async queue, drop policy,
  metrics, flush/shutdown and spdlog/fmt backend.
- [x] Route Plugin common log and diagnostic calls through LoggingContext.
- [x] YAML ConfigLoader built on yaml-cpp, producing RoDAQ-owned ConfigTree.
- [x] Remove unselected mimalloc build/use options from the V1 baseline.

#### Fake Acquisition

- [x] single producer.
- [x] multi producer.
- [x] single Stream.
- [x] multiple Streams.
- [x] descriptor changes.
- [x] Copy.
- [x] Adopt.
- [x] Retain.
- [x] CallerResponsible.
- [x] RuntimeResponsible.
- [x] burst.
- [x] high-rate.
- [x] out-of-order producer timing.
- [x] blocked Admission Wait.
- [x] Stop while submitting.

#### Fake Storage

- [x] submit non-OK before HandoffAccepted.
- [x] synchronous PayloadConsumed.
- [x] PayloadPending → async PayloadConsumed.
- [x] synchronous PersistenceSucceeded.
- [x] synchronous PersistenceFailed.
- [x] synchronous PersistenceUnknown.
- [x] PersistencePending → async Succeeded.
- [x] PersistencePending → async Failed.
- [x] PersistencePending → async Unknown.
- [x] independent Payload/Persistence ordering.
- [x] Storage stall.
- [x] background fatal failure.
- [x] flush.
- [x] close.
- [x] duplicate callback violation.
- [x] missing callback scenario.
- [x] same-stack reentrant callback violation.
- [x] callback after close violation.

### Deliverables

- [x] Complete Fake E2E pipeline.
- [x] Runtime Stop/Failure convergence.
- [x] Fake Plugin correctness suite.
- [x] E2E stress suite.

### Verification

- [x] Gate-pass Accepted cannot be dropped by ordinary pressure.
- [x] same-Stream AcceptanceSequence order == Storage handoff order.
- [x] no false global ordering across Streams.
- [x] PAYLOAD_PENDING → exactly one `payload_consumed()`.
- [x] synchronous PayloadConsumed → no payload callback.
- [x] PERSISTENCE_PENDING → exactly one `persistence_completed()`.
- [x] synchronous persistence terminal → no persistence callback.
- [x] PayloadConsumed + PersistencePending releases Payload early.
- [x] duplicate callback detected.
- [x] same-stack reentrant callback detected; cross-thread race reconciled without false violation.
- [x] post-close callback detected.
- [x] Stop under multi-producer load converges.
- [x] Stop with PayloadPending converges.
- [x] Stop with PersistencePending converges.
- [x] primary failure preserved.
- [x] secondary cleanup errors separately reported.
- [x] nested readiness/Binding/commit failure is not duplicated in secondary history.
- [x] Plugin-origin lifetime prevents premature destroy/unload.
- [x] ingress timestamp is captured before Admission Wait and large-payload Copy.
- [x] Descriptor handle returns before delayed initial readiness completes; Start
  still waits for initial reconciliation before opening Gate.
- [x] dynamic Descriptor publication waits for the Binding transition before the
  Fake Acquisition resumes that Stream.
- [x] Stop during Descriptor readiness does not publish a switch Binding.
- [x] Descriptor reconciliation request queue is bounded to `max_stream_descriptors`.
- [x] process-wide activation-root exhaustion is reported as `ResourceExhausted`.
- [x] continuous handoff remains Running beyond multiple timeout periods when every
  individual Submit meets its own deadline.
- [x] unpublished/fabricated Registry handles cannot observe partially constructed slots.
- [x] duplicate TimestampKey registration fails with `InvalidArgument`.
- [x] Projection-dependent recovery succeeds when the Acquisition Plugin holds a
  local mutex during registration.
- [x] Payload size classes select the smallest accommodating block without
  crossing capacity domains.
- [x] Production aggregated joint profile sustains >= 1,000 accepted records/s.
- [x] Stress per-joint profile sustains >= 100,000 accepted records/s.
- [x] Camera and mixed profiles sustain offered schedules without steady-state
  Admission WaitTimeout or unbounded high-water growth.

### Exit Gate

- [x] Accepted no-drop invariant proven.
- [x] per-Stream ordering proven.
- [x] Runtime permits/counters converge after Stop.
- [x] both Storage axes exactly-once rules proven.
- [x] Payload early-release case proven.
- [x] all completion contract violations detected.
- [x] Stop/failure reaches quiescence.
- [x] 32 activation roots bound all healthy and quarantined activations.
- [x] Standard camera + joint generated workload passes its declared
  throughput, completion, Stop and bounded-resource gates.
- [x] LoggingContext bounded queue/flush/shutdown and Plugin logging service
  integration pass ASan/UBSan/TSan and lifecycle tests.
- [x] YAML ConfigLoader produces ConfigTree/ConfigView without exposing
  YAML::Node across Runtime or Plugin boundaries.
- [x] required ASan/UBSan/TSan stress suite clean.

### Out of Scope

Do not implement in M2:

- ROS2.
- FastDDS.
- MCAP.
- SQLite3 (future).
- final AppImage release qualification.
- final real-backend and filesystem performance tuning.

---

## M3 — ROS2 Acquisition + MCAP Storage Vertical Slice

**Status:** NOT STARTED

### Goal

Implement the first real Acquisition-to-Storage chain on top of the already validated Runtime.

### Prerequisites

- [x] M0 PASS.
- [x] M1 PASS.
- [ ] Current M2 Status/Result/Logging refactor requalified;
  `M2 = PASS / Core FROZEN` restored. Historical M2 evidence alone
  does not qualify the active revision.
- [ ] Pre-M3 Non-plugin Runtime Contract Gate above PASS, with
  cross-thread Storage mutation evidence.
- [ ] ROS2 mapping reviewed against current ABI.
- [ ] MCAP mapping reviewed against current ABI and a directly pinned RoDAQ mcap-cpp version/commit (no ROS2 mcap_vendor runtime dependency).

### Scope

#### Non-plugin Runtime / Storage integration

- [ ] Rerun `runtime.md` `16.34 interleaving tests before first real
  Storage integration and retain them as M3 regression coverage.
- [ ] Prove real MCAP descriptor readiness, dynamic descriptor
  evolution, `submit`, final `flush` and `close` follow the verified
  mutation coordination contract under overlapping source traffic.
- [ ] Measure actual Handoff stalls, per-class Camera/Joint
  offered/accepted/rejected counts and P95/P99/P99.9; distinguish
  single-Lane head-of-line blocking from aggregate throughput.
- [ ] Capture actual Backend finalization, disk consumption,
  metadata/index memory growth and failure behavior as inputs to
  the future Recording Service. Do not introduce Task Ledger or
  file-retention semantics into Runtime/Plugin ABI.

#### ROS2 Acquisition

- [ ] Plugin lifecycle.
- [ ] graph discovery.
- [ ] Source mapping.
- [ ] Clock mapping.
- [ ] Schema mapping.
- [ ] Stream mapping.
- [ ] StreamDescriptor publication.
- [ ] QoS / ConfigView.
- [ ] `rodaq.projection.ros2` generation and registration.
- [ ] serialized payload acquisition.
- [ ] Copy baseline.
- [ ] Retain/loan only if concrete lifetime evidence justifies it.
- [ ] callback quiescence.
- [ ] failure reporting.

#### MCAP Storage

- [ ] Plugin lifecycle.
- [ ] `Header.profile = ros2`.
- [ ] Ros2Projection → Schema projection.
- [ ] Ros2Projection → Channel / offered QoS projection.
- [ ] Record projection.
- [ ] checked sink.
- [ ] bounded metadata batching.
- [ ] RecordMetadata sidecar.
- [ ] payload lifetime proof.
- [ ] persistence batch completion.
- [ ] Artifact finalization.
- [ ] required crash/recovery behavior.

### Deliverables

- [ ] ROS2 Acquisition Plugin DSO.
- [ ] MCAP Storage Plugin DSO.
- [ ] Plugin manifests.
- [ ] E2E executable/integration test.
- [ ] round-trip validator.
- [ ] initial performance baseline.
- [ ] real-MCAP Storage mutation interleaving/order and tail-latency evidence.
- [ ] Backend Artifact finalization / disk-failure evidence usable by
  a later Recording Service, including a list of facts that are **not**
  yet exposed through supported DAQ/validator interfaces.
- [ ] development AppImage smoke artifact.

### Verification

### ROS2

- [ ] single publisher.
- [ ] multiple publishers same Stream.
- [ ] publisher disappear/reappear.
- [ ] schema/type identity.
- [ ] source sequence sentinel handling.
- [ ] timestamp mapping.
- [ ] Copy exact bytes.
- [ ] normal Accepted SubmitResult.
- [ ] GateClosed rejection result.
- [ ] ResourceUnavailable rejection result.
- [ ] WaitTimeout result.
- [ ] Stop while callbacks active.

### MCAP / interoperability

- [ ] Schema/Channel mapping.
- [ ] descriptor evolution.
- [ ] same-Stream ordering.
- [ ] payload exactness.
- [ ] metadata sidecar pairing.
- [ ] bounded metadata batch.
- [ ] flush.
- [ ] close.
- [ ] failure injection.
- [ ] Distinguish successful Backend finalization,
  `PersistenceSucceeded` and unproven crash durability;
  no `NormalComplete` after short write, ENOSPC or close failure.
- [ ] Dynamic descriptor reconciliation and concurrent submit/Stop
  interleaving without unproven Storage mutation overlap.
- [ ] Real MCAP writer memory/index/sidecar and disk-use trends under
  sustained camera + joint mixed workload.
- [ ] `ros2 bag info` qualification.
- [ ] `ros2 bag play` + subscriber verification.
- [ ] PlotJuggler direct-open + field decode qualification.
- [ ] Foxglove direct-open + field decode qualification.
- [ ] representative String/Imu/Image/PointCloud2/JointState/TF/custom-message fixtures.

Required initial submit mapping:

- [ ] `payload_state == PAYLOAD_CONSUMED`.
- [ ] `persistence_state == PERSISTENCE_PENDING`.
- [ ] no `payload_consumed()` callback.
- [ ] batch commit produces exactly one persistence completion.

### Exit Gate

- [ ] ROS2 → Runtime → MCAP functional path passes.
- [ ] no ROS/RMW type leaks into Runtime Core public contracts.
- [ ] no MCAP type leaks into Runtime Core public contracts.
- [ ] Canonical mapping validated.
- [ ] logical payload round-trip exact.
- [ ] ROS2-profile MCAP with complete ros2msg/ros2idl + cdr projection.
- [ ] ros2 bag play / PlotJuggler / Foxglove qualification passes for the M3 corpus.
- [ ] AcceptanceSequence/timestamps preserved.
- [ ] ROS2 Stop quiescence passes.
- [ ] MCAP PayloadConsumed lifetime test passes.
- [ ] MCAP PersistencePending completion tests pass.
- [ ] sidecar/descriptor failure-recovery tests pass.
- [ ] initial throughput/CPU/RSS/latency baseline recorded.
- [ ] Fake and real Backend cross-thread Storage mutation tests PASS;
  no readiness or same-Stream handoff regression.
- [ ] Short mixed-load MCAP evidence explicitly records limits:
  no per-Stream hard real-time QoS, no assured crash durability,
  and no claim of uninterrupted recording.

### Out of Scope

Do not implement in M3:

- FastDDS Acquisition (implemented in M4).
- SQLite3 Storage implementation.
- final production tuning.
- final release qualification.
- Recording Service production implementation, durable Task Ledger,
  Artifact Catalog/Retention and automatic process recovery
  (M5 `BoundedRecording` workstream).
- `ContinuousRecording`, seamless rolling and cross-Scope continuity
  (future design gate).
- hot reload.

---

## M4 — FastDDS Acquisition + MCAP Interoperability

**Status:** NOT STARTED

### Goal

Implement the second V1 Acquisition path directly on Fast DDS / Fast CDR, without a ROS2 runtime dependency on the recording host, and prove that ROS2 Acquisition and FastDDS Acquisition converge to the same ROS2-compatible MCAP contract.

### Prerequisites

- [ ] M3 PASS.
- [ ] Fast DDS / Fast CDR versions frozen for implementation.
- [ ] DDS-Pipe / DDS Record & Replay source-reference audit completed.

### Scope

#### FastDDS Acquisition

- [ ] Plugin lifecycle and clean non-ROS2 deployment.
- [ ] DomainParticipant / endpoint discovery / DataReader lifecycle.
- [ ] `rt/*` ROS2 topic normalization.
- [ ] TypeInformation / CompleteTypeObject / DynamicType resolution.
- [ ] exact ROS2 type-name reconstruction.
- [ ] dependency-complete ros2idl/ros2msg reconstruction.
- [ ] serialized CDR data path without mandatory per-record DynamicData decode/re-encode.
- [ ] Writer GUID SourceId mapping.
- [ ] publication/reception timestamp mapping.
- [ ] DataWriter QoS → deterministic `offered_qos_profiles`.
- [ ] `rodaq.projection.ros2` generation.
- [ ] bounded pending-type/discovery state.
- [ ] listener/worker quiescence and failure/recovery.

#### Cross-Acquisition Equivalence

- [ ] record the same ROS2 publisher corpus through ROS2 Acquisition and FastDDS Acquisition.
- [ ] compare normalized topic/type/schema semantics.
- [ ] compare payload bytes.
- [ ] compare QoS projection.
- [ ] compare timestamp semantics/tolerance.
- [ ] compare playback-visible behavior.

#### Interoperability

- [ ] FastDDS → MCAP `ros2 bag info`.
- [ ] FastDDS → MCAP `ros2 bag play` + subscriber verification.
- [ ] FastDDS → MCAP PlotJuggler qualification.
- [ ] FastDDS → MCAP Foxglove qualification.
- [ ] RoDAQ Canonical round-trip.

### Deliverables

- [ ] FastDDS Acquisition Plugin DSO.
- [ ] Plugin manifest and dependency closure.
- [ ] DDS-Pipe / DDS Record & Replay source-reference audit notes/evidence.
- [ ] cross-acquisition equivalence suite.
- [ ] FastDDS interoperability golden corpus.
- [ ] clean non-ROS2 recording-host smoke package.

### Verification

- [ ] `rmw_fastrtps_cpp` external ROS2 baseline passes.
- [ ] dynamic publisher add/remove/restart.
- [ ] complete type dependency discovery.
- [ ] multiple publishers on one topic.
- [ ] BEST_EFFORT / RELIABLE / TRANSIENT_LOCAL QoS cases.
- [ ] 720P image payload path.
- [ ] no mandatory hot-path deserialize/re-serialize.
- [ ] FastDDS Stop/quiescence under active traffic.
- [ ] no ROS2 libraries in FastDDS recording-host transitive runtime closure.
- [ ] ROS2/FastDDS cross-acquisition equivalence runs in separate process profiles by default.
- [ ] Classify native DDS/RMW losses, Plugin-local staging losses and
  Runtime PreAdmissionRejected separately where source evidence exists;
  report `Unknown` rather than 0 lost where no reliable proof exists.
- [ ] Preserve each Activation/Scope time identity and qualify
  cross-source timestamp uncertainty; do not assert inter-Activation
  continuity without explicitly proven Clock correlation.
- [ ] simultaneous ROS2 + FastDDS activation is rejected unless Fast DDS/Fast CDR SONAME/ABI/symbol coexistence qualification explicitly passes.

### Exit Gate

- [ ] FastDDS Acquisition integrates without ROS2 runtime dependency.
- [ ] no Fast DDS / Fast CDR type leaks into Runtime Core public contracts.
- [ ] complete ROS2 schema and Ros2Projection generated for qualification corpus.
- [ ] FastDDS → Runtime → MCAP passes ros2 bag play / PlotJuggler / Foxglove.
- [ ] ROS2 vs FastDDS cross-acquisition equivalence suite passes.
- [ ] clean non-ROS2 recording-host dependency audit passes.

### Out of Scope

- Generic DDS recording.
- ROS2 services/actions recording.
- DDS routing/forwarding/replay.
- SQLite3 Storage implementation.
- final production tuning/release qualification.

---

## M5 — Production Hardening + Release Qualification

**Status:** NOT STARTED

### Goal

Harden the completed ROS2 + FastDDS + MCAP system for bounded production behavior and release qualification without weakening correctness or interoperability.

### Prerequisites

- [ ] M4 PASS.

### Scope

- [ ] ASan / UBSan / TSan regression suites.
- [ ] repeated load/unload and Stop/failure stress.
- [ ] long soak and MCAP failure injection.
- [ ] 10-camera × 30 FPS × 1280×720 encoded workload.
- [ ] raw-image stress workload.
- [ ] joint telemetry workload:
  - [ ] production: 100 joints × 100 parameters per joint × 1 kHz aggregated
    RobotState Record.
  - [ ] stress: 100 joints × 100 parameters per joint × 1 kHz per-joint Records.
- [ ] mixed 10-camera + joint-telemetry workload.
- [ ] PayloadPool / queue / batching benchmark-driven tuning.
- [ ] CPU / RSS / allocation / copy-count / tail-latency baselines.
- [ ] ros2 bag play / PlotJuggler / Foxglove interoperability regression.
- [ ] x86_64 / aarch64 AppImage qualification.
- [ ] Ubuntu 22.04 / 24.04 clean-system qualification.
- [ ] GLIBC / GLIBCXX / CXXABI / RPATH / DT_NEEDED gates.
- [ ] FastDDS-only deployment without ROS2 runtime.
- [ ] plugin dependency collision audit covers ROS2 `rmw_fastrtps_*` versus RoDAQ-pinned Fast DDS/Fast CDR.
- [ ] SBOM / licenses / checksums / release manifest.

#### BoundedRecording Production Service (non-plugin workstream)

**Status:** NOT STARTED. Complete this workstream if V1 is advertised
as an industrial unattended recording product. A core-library or
finite-task distribution may defer this workstream **only with an
explicitly narrower Release Scope** and no production-service claim.
Reference: `docs/design/recording_service.md` ``1–7.

**Task lifecycle / durable facts**
- [ ] Define the deployment Profile before implementation: finite task
  duration, peak source rates, allowed loss/Unknown, RPO/RTO,
  headroom, restart budget, clock uncertainty and health SLO.
- [ ] Implement `TaskId` / `ActivationRef` / `ArtifactRef` with
  generation-aware idempotent Start/Stop, one Writer per target,
  conflict handling and explicit inter-Activation recording gaps.
- [ ] Implement bounded, versioned Durable Task Ledger + replay,
  checksum/torn-tail protection, atomic publication, directory
  ownership and duplicate/late-command reconciliation.
- [ ] Separate Task/Artifact facts from Canonical identity;
  no Task recovery may rewrite AcceptanceSequence, Scope or
  unverified `NormalComplete`.

**Disk / Artifact management**
- [ ] Implement Disk Admission for worst-case finite duration,
  peak bytes, already-reserved space, filesystem quotas,
  MCAP writer metadata/index and finalization headroom.
- [ ] Implement disk `Healthy → Pressure → Critical` thresholds
  with hysteresis, ETA-to-full, bounded stop and explicit ENOSPC.
- [ ] Implement Artifact Catalog and read-only validation of
  `Reserved/Writing/Finalizing/VerifiedComplete/Partial/Unknown`;
  only Backend/Validator evidence establishes completeness.
- [ ] Implement idempotent/audited Retention for independently
  closed, verified, authorized Artifacts with no active Reader
  lease. Never delete current/Partial/unknown files to
  manufacture headroom.

**Recovery / supervision / health**
- [ ] On startup, obtain exclusive ownership, replay ledger,
  scan artifacts read-only, classify partial/corrupt/unknown,
  and start any replacement recording as a **new** Activation.
- [ ] Provide external process supervisor (e.g. systemd):
  progress-based health, restart backoff and budget,
  circuit-breaker and no-double-writer fencing.
- [ ] Export bounded Task + Runtime + Source Quality + Storage
  snapshots. Separate native loss, Plugin staging loss, pre-Gate
  rejects, Accepted and PersistenceFailed/Unknown; missing
  source proof must appear as `Unknown`, never 0 lost.
- [ ] Provide minimal task-control CLI/local interface with
  OperationId/generation idempotency and bounded readers;
  remote/API access, if exposed, requires auth/authz,
  audit, secure transport and request/bandwidth limits.
- [ ] Choose and validate actual crash-durability mode with
  measurable RPO, independent of `PersistenceSucceeded`,
  `NormalComplete` and Task Ledger persistence; explicitly
  document unsupported guarantees.

**Industrial qualification**
- [ ] Fault-inject SIGKILL/OOM, power-loss simulation,
  torn Ledger, ENOSPC/quota, short write, interrupted close,
  blocked Plugin, restart loops, Retention failure and
  concurrent Reader leases.
- [ ] Prove actual Camera+Joint MCAP resource trends:
  RSS, FD, thread, writer index/sidecar/log memory, file count,
  disk utilization and tail latency across repeated tasks.
- [ ] Retain the 30–60 min M5 mixed-load as a short performance
  check; propose ≥72 h stability and ≥168 h/maintenance-period
  unattended run for the production Profile. Freeze the actual
  duration by project SLO/risk assessment; these are design
  recommendations, not universal standards.
- [ ] Record Task/Artifact/Recovery traces, P95/P99/P99.9,
  accepted/rejected/source-loss evidence, measured RTO/RPO
  and uncovered intervals in a reproducible report.

### Deliverables

- [ ] benchmark and soak reports.
- [ ] crash/failure matrix evidence.
- [ ] interoperability regression corpus/results.
- [ ] qualified release artifacts for required architectures.
- [ ] SBOM / THIRD_PARTY_NOTICES / release manifest.
- [ ] `BoundedRecording` service implementation plus durable
  ledger/catalog schema, replay and lifecycle tests **if production
  service is an advertised Release Profile**.
- [ ] Disk Admission/Retention, conservative recovery, supervisor
  health and fault-injection evidence **if this Profile is in scope**.
- [ ] Explicit Release Scope declaration (core-library/finite-task
  or qualified unattended `BoundedRecording`); `ContinuousRecording`
  must be marked unsupported until a separate future gate.

### Verification

- [ ] bounded high-water marks under camera and mixed workloads.
- [ ] no long-run resource growth.
- [ ] failure paths preserve deterministic quiescence and Artifact state.
- [ ] performance optimizations have before/after evidence.
- [ ] ROS2 and FastDDS interoperability remains green after tuning.
- [ ] Validate Task/Artifact failure state from actual Backend
  and Ledger evidence; distinguish `NormalComplete` from measured
  crash durability.
- [ ] If `BoundedRecording` is advertised: repeated finite tasks
  across process restart, writer fencing, catalog/retention
  safety, disk headroom and Partial Artifact recovery matrix PASS.
- [ ] If `BoundedRecording` is advertised: long soak per frozen
  deployment Profile, supervisor restart limits, upstream loss/
  Unknown accounting and bounded writer memory/disk/FD trends PASS.

### Exit Gate

- [ ] required sanitizer and long-soak suites clean.
- [ ] bounded-resource behavior proven.
- [ ] camera/mixed-workload baselines recorded.
- [ ] crash/failure matrix passes.
- [ ] ROS2 + FastDDS MCAP interoperability qualification passes.
- [ ] x86_64 and aarch64 release qualification passes.
- [ ] FastDDS recording-host package has no ROS2 runtime dependency.
- [ ] SBOM/license/release manifest complete.
- [ ] Product scope documented: **production `BoundedRecording`
  claim only after the required non-plugin Service/Disk/Ledger/
  Recovery/Health gates PASS**. Otherwise explicitly label
  release core-library/finite-task only.
- [ ] Storage mutation interleaving tests (Descriptor readiness,
  submit, flush, close) remain PASS after tuning; historical
  M3 evidence is not enough if coordination implementation changed.
- [ ] No unsupported claim of seamless rolling, full-chain zero
  loss, crash durability or same-Scope automatic recovery.

### Out of Scope

- SQLite3 implementation.
- SocketCAN implementation.
- Generic DDS implementation.
- additional Storage formats.
- runtime hot reload / process sandboxing.
- `ContinuousRecording`: seamless rolling, cross-Segment lineage,
  same-Scope automatic recovery and active-Artifact retention.
- full remote management UI/export server unless separately
  selected; production security constraints still apply to
  any externally exposed interface.

### Future Design Gate — ContinuousRecording (not a V1 milestone)

**Status:** NOT STARTED / outside M5 Exit Gate. This is a
dependency-controlled future extension, not permission to
modify the Storage Plugin or add a second Runtime now.

- [ ] Freeze Segment identity and Task/Artifact/Scope relation:
  atomic old/new Segment boundary, ordered Record assignment,
  Descriptor/Projection sidecars and independently valid
  Segment finalization.
- [ ] Design required Storage/Runtime ABI capabilities for
  bounded rolling while preserving same-Stream AcceptanceSequence
  and Payload/Persistence lifetime; evaluate measured alternatives.
- [ ] Freeze cross-Segment Manifest, Retention and crash matrix:
  insufficient disk, next open/old close failure, reader lease,
  Partial recovery and lineage across process restart.
- [ ] Qualify MCAP Segment consumption with ros2 bag play,
  PlotJuggler, Foxglove and Task-level reader composition,
  without rewriting Canonical IDs.
- [ ] Run separate long-duration rolling/durability/recovery
  qualification and explicitly authorize the new release
  Profile before claiming uninterrupted recording.

---

## 2026-11-05 M2 Ownership Closure — COMPLETE / local PASS

- [x] Source owner retention, same-owner interning and cross-owner Conflict.
- [x] Caller ownership for Source/Stream mutation and optional Source Submit.
- [x] Owner-scoped failure propagation and explicit Source recovery.
- [x] Descriptor transport Clock ownership, including ingress Clock exclusion.
- [x] Fixed-capacity Quarantine publication with shared 64-entry budget and
  preserved claim/Join/destruction ordering; 32 activation roots unchanged.
- [x] Stable-ABI double-Plugin regressions, synchronized recovery, shared-budget
  exhaustion/reuse and isolated allocation-failure regression.
- [x] Normal/ASan+LSan/UBSan/Release/Ubuntu 24: 94/94 each; Clang 14 TSan:
  10/10 with thread-leak checks; Runtime-only Release and Release capacity PASS.
- [x] Design, decisions, review evidence and capacity report updated; public
  headers/Stable ABI unchanged; formatter and diff checks PASS.

Prior baseline: local `fe4f938` PASS; hosted `3adefb28`
[RoDAQ Qualification #41968952350](https://github.com/SiYueY/rodaq/actions/runs/41968952350) SUCCESS.
Local qualification was performed before commit/push; subsequent hosted
qualification of `be448c8` succeeded at [RoDAQ Qualification #41975605291](https://github.com/SiYueY/rodaq/actions/runs/41975605291).
Native arm64 remains D-034 DEFERRED.
The generic M2 Runtime is frozen for this milestone. M3 remains NOT STARTED;
start ROS2 Acquisition + MCAP Storage only as a separate development task.

## 2026-11-05 C++ coding standards review — COMPLETE / local PASS

- [x] Review applicable C++17 coding guidelines; record automated diagnostics,
  intentional exceptions and scoped ownership/lifetime/concurrency review.
- [x] Migrate project-owned functions, C++ SDK authoring hooks, Runtime
  Application Candidate, tests, benchmarks and install consumers to snake_case.
- [x] Revise D-033 source-name freeze via D-056; document required source/binary
  migration. Stable C Plugin ABI and frozen V1 binary fixtures remain unchanged.
- [x] Clang-format 14 and seven clang-tidy 14 checks pass; CI check added for
  translation units, C/C++ public headers and install consumers.
- [x] Normal, ASan/LSan, UBSan, Release and Ubuntu 24: 94/94 each; Clang 14
  TSan: 10/10; Runtime-only Release and Release capacity PASS.
- [x] Design, decision, review and capacity documents updated; native arm64
  remains D-034 DEFERRED. M3 remains NOT STARTED.

This is local qualification of the uncommitted working tree. New hosted
qualification for these changes remains PENDING until committed, pushed and run.


## 2026-11-05 C++ declaration separation

- [x] Move SDK and applicable internal Runtime function definitions into source
  files, retaining template, constant evaluation, Plugin export and necessary
  public inline linking cases in headers.
- [x] Recheck installed SDK / Stable C ABI compatibility, full local functional
  and sanitizer matrices, Runtime-only build and automated C++ style checks.
- [ ] Obtain hosted qualification for the current uncommitted changes. Full
  Clang TSan target build additionally exposes an existing unused-variable
  warning in a null-descriptor test fixture; CI's specified TSan subset passes.

M3 remains NOT STARTED. The earlier M2 local qualification and hosted runs are
historical evidence and do not qualify the latest uncommitted changes.


## 2026-11-05 Error native accessor rename

- [x] Rename the C++ `Error` native accessor and member; update callers and
  preserve the Stable C ABI field name.
- [x] Run Debug full build/CTest and C++ style checks.
- [ ] Run hosted qualification after committing and pushing these changes.


## 2026-11-05 Result failure construction

- [x] Expose `rodaq::make_error`, migrate project callers and SDK macros, and
  confine tl::expected names to the Result implementation and contract tests.
- [x] Validate error copy/move, void Result, installed consumers, full Debug
  CTest and automated C++ style checks; Stable C ABI remains unchanged.
- [ ] Obtain hosted qualification for the current working-tree changes.


## 2026-11-05 Direct error construction

- [x] Add direct `make_error` overloads and migrate applicable call sites while
  preserving forwarding of existing Error objects and the Stable C ABI.
- [x] Validate direct and propagated errors, full Debug build/CTest and C++ style.
- [ ] Obtain hosted qualification for current working-tree changes.


## 2026-11-05 C++ status terminology convergence

- [x] Replace C++ ErrorCode with StatusCode including Ok; migrate Error accessor,
  ABI mappings, project code, tests, SDK consumers and current design contracts.
- [x] Normalize Ok / unknown codes on Error construction without losing supplied
  diagnostics; preserve Stable C ABI and its existing diagnostic behavior.
- [x] Verify Debug, Release, ASan/LSan and UBSan 94/94 each; Clang 14 TSan
  subset 10/10; Runtime-only Release and C++ style checks PASS.
- [ ] Complete hosted qualification of the current changes after commit/push.

C++ sources must migrate and rebuild; M3 remains NOT STARTED.


## Status + Result migration — COMPLETE / local PASS

- [x] Replace public Error / Result<void> with Status and a RoDAQ Result<T>
  wrapper; remove make_error, legacy result accessors and compatibility aliases.
- [x] Migrate Runtime, SDK virtual functions/trampolines, tests, benchmark and
  installed consumers; use Status directly for primary failure observation.
- [x] Migrate C status types, fields and constants, independent ABI fixtures and
  layout tests. Preserve domain ownership, admission and shutdown behavior.
- [x] Validate OK/UnknownError normalization, immutable result failure state,
  copy/move, all C/C++ status mappings and forbidden Result instantiations.
- [x] Debug, Release, ASan/LSan, UBSan, Ubuntu 24: 96/96 each; Clang 14 TSan:
  10/10 with thread-leak detection; Runtime-only and capacity gates PASS.
- [x] Full C++ style checks, current design/decision updates and final diff review.
- [ ] Obtain new hosted qualification after commit/push. Older CI is historical.

No compatibility layer is retained. All C/C++ consumers and plugins must
migrate and rebuild. M3 remains NOT STARTED; native arm64 remains D-034 DEFERRED.

## 2026-11-06 Direct Status failure storage

- [x] Remove optional<Status> and migrate public/internal failure observation,
  deferred diagnostics, tests, install consumer and current contracts.
- [x] Verify Debug/ASan+LSan/UBSan 96/96 each, Clang 14 TSan 10/10,
  installed DAQ consumer and full automated C++ style checks.
- [ ] Obtain new hosted qualification after commit/push. Release capacity,
  Runtime-only and Ubuntu 24 were not rerun for this follow-up.

No compatibility layer is retained. M3 remains NOT STARTED.

## 2026-11-06 Result<void> operation migration — COMPLETE / local PASS

- [x] Add the out-of-line Result<void> specialization with private expected<void,
  Status> storage, default success, immutable diagnostics and OK-as-failure
  normalization. Preserve non-void Result defaults and prohibit Result<Status>.
- [x] Migrate every no-value operation, callback, SDK hook, test, benchmark and
  consumer; retain Status for diagnostics and failure observations only.
- [x] Replace the void compile-fail fixture with positive unit/header/install
  checks and reject implicit void-result-to-value failure loss.
- [x] Debug/Release/ASan+LSan/UBSan/Ubuntu 24: 95/95 each; Clang 14 TSan:
  10/10; Runtime-only and eight Release capacity executables (three repetitions)
  PASS. Full formatting/static checks and final diff review PASS.
- [x] Update current designs, D-058, status, review and capacity evidence.
- [ ] Obtain new hosted qualification after commit/push; previous CI is historical.

M3 remains NOT STARTED; native arm64 remains D-034 DEFERRED. No compatibility
aliases or old operation signatures are retained.
