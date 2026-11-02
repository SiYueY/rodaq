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
M4  Production Hardening
        ↓
M5  DDS Acquisition + SQLite3 Storage
```

Current milestone: **M0 — ACTIVE**

Only one milestone may be `ACTIVE` at a time.

---

## M0 — Foundation + Plugin ABI

**Status:** ACTIVE

**Current sub-phase:** **M0-B — PASS**. M0-A Review Closure and M0-B Stable C
Plugin ABI are implemented, mechanically tested, sanitizer-tested and reviewed.
M0-C Plugin Loading + Lifetime is next.

### Goal

Create the buildable project foundation and implement the Stable Plugin boundary before any real backend is coupled to Runtime.

### Prerequisites

- [x] V1 system design baseline exists.
- [x] Plugin System Freeze Candidate exists.
- [x] Acquisition / Storage Plugin contracts reconciled with Plugin System.
- [x] ROS2 / DDS / MCAP / SQLite3 mapping documents aligned to Stable ABI.
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

Production baseline:

- [x] oneTBB 2022.3.0.
- [x] spdlog 1.17.0.
- [x] fmt 12.1.0.
- [x] HdrHistogram_c 0.12.1.
- [x] yaml-cpp 0.9.0.
- [x] expected-lite 0.10.0 as the pinned Public C++ header dependency.
- [x] mimalloc 3.6.1 optional.

Development:

- [x] GoogleTest 1.18.0.
- [x] Google Benchmark 1.10.0.

#### Result / Error

- [x] Finalize `rodaq::Result<T>` as the direct expected-lite alias.
- [x] Force expected-lite's nonstd implementation independently of consumer C++ mode.
- [x] Keep expected-lite out of the Stable C ABI.
- [x] Implement/freeze error representation used by Public C++ APIs.

#### Stable ABI

- [x] Implement `include/rodaq/abi/abi.h`.
- [x] Implement `canonical.h`.
- [x] Implement `plugin.h`.
- [x] Implement `service.h`.
- [x] Implement `config.h`.
- [x] Implement `runtime.h`.
- [x] Implement `acquisition.h`.
- [x] Implement `storage.h`.

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
- [ ] wrong Boot ABI fixture.
- [ ] wrong family protocol fixture.
- [ ] too-small struct fixture.
- [ ] missing-symbol fixture.
- [ ] duplicate Plugin ID fixture.
- [ ] symbol provenance test.

#### Plugin infrastructure

- [ ] Implement `SharedLibrary`.
- [ ] Implement `PluginLibrary`.
- [ ] Implement `PluginCatalog`.
- [ ] Implement minimal `PluginManager`.
- [ ] Implement `AcquisitionInstance`.
- [ ] Implement `StorageInstance`.
- [ ] Implement `PluginLibraryLease`.
- [ ] Implement `PluginInstanceLease`.
- [ ] Implement Service Registry.
- [ ] Implement ConfigView.
- [ ] Implement Extension Query plumbing.
- [ ] Implement minimal C++17 Plugin SDK.
- [ ] Implement exception trampolines.
- [ ] Implement Plugin metadata generation.
- [ ] Implement Plugin authoring CMake helper.

#### Minimal Fake DSOs

- [ ] Fake Acquisition loads and validates.
- [ ] Fake Storage loads and validates.
- [ ] create/bind/configure/destroy lifecycle works.
- [ ] DSO unload occurs only after last lease.

### Deliverables

- [x] Clean buildable repository skeleton.
- [x] Pinned dependency graph.
- [x] Stable ABI headers.
- [x] ABI mechanical tests.
- [ ] Plugin loader/infrastructure.
- [ ] Service Registry / ConfigView.
- [ ] Minimal C++ Plugin SDK.
- [ ] Minimal Fake Acquisition / Storage DSOs.
- [ ] Initial ABI compatibility baseline.

### Verification

#### Acquisition ABI

- [x] `AdmissionOutcome` representable.
- [x] `RejectionReason` representable.
- [x] `PayloadDisposition` representable.
- [ ] Rejection reason does not imply ownership.
- [ ] ADOPT before transfer → CallerResponsible.
- [ ] ADOPT after transfer → RuntimeResponsible.
- [ ] RETAIN before acquisition → CallerResponsible.
- [ ] RETAIN after acquisition → RuntimeResponsible.
- [ ] RETAIN disposition does not transfer original native owner.
- [ ] Gate-pass/in-flight entry precedes irreversible AcceptanceSequence allocation.
- [ ] PreAdmissionRejected never consumes AcceptanceSequence.
- [ ] Clock registration same `(PluginInstance, native_id)` + same facts is idempotent.
- [ ] Clock same key + conflicting immutable facts → CONFLICT.
- [ ] Clock continuity reset requires new native-id generation evidence.
- [ ] Stream same `(PluginInstance, native_id)` re-registration is idempotent.
- [ ] identical native_id in different Plugin instances does not merge Clock/Stream identity.

#### Storage ABI

- [ ] submit non-OK means HandoffAccepted never established.
- [x] PayloadConsumed synchronous result representable.
- [x] PayloadPending representable.
- [x] PersistenceSucceeded synchronous result representable.
- [x] PersistenceFailed synchronous result representable.
- [x] PersistenceUnknown synchronous result representable.
- [x] PersistencePending representable.
- [ ] no same-stack reentrant callback; cross-thread completion race with submit return is reconciled correctly.
- [ ] async publication forbidden before irreversible `OK + matching PENDING` decision.
- [ ] early completion is buffered and cannot reclaim submit-call resources before return/reconcile.

#### Service / Config / lifetime

- [ ] required Service success/failure.
- [ ] optional Service absence.
- [ ] required `rodaq.acquisition.control.v1` present/compatible.
- [ ] required `rodaq.storage.control.v1` present/compatible.
- [ ] missing/incompatible required control extension prevents activation.
- [ ] `quiesce_plugin()` blocks all current/future Submit routing without destroying native release capability.
- [ ] normal Stop order is quiesce → drain/anchor release → destructive stop.
- [ ] abort KEEP_PAUSED never restores old routing.
- [ ] abort RESUME_OLD restores only Runtime-proven valid old routing.
- [ ] repeated terminal abort/commit on one transition → INVALID_STATE.
- [ ] Runtime lifecycle snapshot gates one recovery attempt and stale snapshot is rejected by later Runtime state checks.
- [ ] protocol/struct-size validation.
- [ ] caller-owned APPENDABLE output: old/new caller/callee compatibility.
- [ ] flags/reserved validation.
- [ ] ConfigView MAP/LIST/scalar tests.
- [ ] ConfigView exact-type conversion.
- [ ] ConfigView out-of-range index → NOT_FOUND.
- [ ] caller-owned `rodaq_error` remains readable after ABI return.
- [ ] Plugin descriptor / entrypoint object lifetime tied to PluginLibraryLease.
- [ ] Plugin exception firewall.
- [ ] raw noexcept violation subprocess death test.
- [ ] control-call timeout does not imply quiescence.
- [ ] stream-local control timeout isolates the target Stream by default.
- [ ] stream-local timeout escalates only when shared synchronization/native/memory-safety guarantees are lost.
- [ ] timed-out call remains in-flight; overlapping control mutation is forbidden.
- [ ] timed-out unquiesced Plugin instance/DSO is retained/quarantined.
- [ ] live instance prevents unload.
- [ ] live lease prevents unload.
- [ ] relevant ASan/UBSan suite clean.

### Exit Gate

M0 PASS only when the authoritative M0 Exit Gate in `rodaq_v1_system_design.md` is satisfied.

Evidence to record before marking PASS:

- [ ] build commands/results.
- [ ] ABI test results.
- [ ] Fake Plugin lifecycle results.
- [ ] Service/Config tests.
- [ ] lifetime tests.
- [ ] sanitizer results.
- [ ] no unresolved P0/P1 Plugin ABI conflict.

### Out of Scope

Do not implement in M0:

- full Admission/ResourceBudget pipeline,
- full PayloadPool,
- Dispatch/Completion Domain,
- ROS2 data acquisition,
- DDS data acquisition,
- MCAP writing,
- SQLite3 persistence,
- production performance tuning,
- hot reload.

### Immediate task order

1. [x] CMake/project skeleton.
2. [x] Third-party submodules/dependency CMake.
3. [x] `Result/Error`.
4. [x] ABI headers + ABI tests.
5. `SharedLibrary / PluginLibrary`.
6. Service Registry / ConfigView.
7. C++ Plugin SDK/trampolines.
8. Minimal Fake Plugin lifecycle fixtures.

---

## M1 — Runtime Core Primitives

**Status:** NOT STARTED

### Goal

Implement bounded, independently testable Runtime correctness primitives.

### Prerequisites

- [ ] M0 PASS.

### Scope

- [ ] Handles.
- [ ] bounded Registry.
- [ ] Structural Arena.
- [ ] ResourceBudget.
- [ ] all-or-nothing Reservation.
- [ ] Admission Gate / generation.
- [ ] Reject / interruptible Wait / deadline.
- [ ] Payload.
- [ ] PayloadView.
- [ ] PayloadPool.
- [ ] RuntimeRecord.
- [ ] RuntimeStats.
- [ ] LatencyHistogram.
- [ ] Logger facade.

### Deliverables

- [ ] Unit-tested Handle/Registry implementation.
- [ ] ResourceBudget/Reservation implementation.
- [ ] Payload ownership implementation.
- [ ] bounded PayloadPool.
- [ ] metrics/high-water primitives.
- [ ] stress/sanitizer targets.

### Verification

- [ ] resource exhaustion.
- [ ] partial reservation rollback.
- [ ] concurrent waiters.
- [ ] timeout.
- [ ] Stop wakeup.
- [ ] Failure wakeup.
- [ ] Gate generation change.
- [ ] Copy lifetime.
- [ ] Adopt transfer-point cases.
- [ ] Retain acquisition-point cases.
- [ ] exactly-once release.
- [ ] retained byte accounting.
- [ ] Payload object exhaustion.
- [ ] no unbounded allocator fallback.
- [ ] ASan clean.
- [ ] UBSan clean.
- [ ] TSan clean for required concurrency tests.

### Exit Gate

- [ ] All configured critical resources proven bounded.
- [ ] Reservation rollback proven non-failing and leak-free.
- [ ] Wait paths proven Stop/Failure interruptible.
- [ ] Payload ownership tests pass.
- [ ] Registry/Handle tests pass.
- [ ] required sanitizers clean.
- [ ] no backend-specific type in Runtime primitives.

### Out of Scope

Do not implement in M1:

- complete E2E Pipeline,
- Storage Completion Domain,
- real backend data path,
- release performance tuning.

---

## M2 — Fake End-to-End Runtime

**Status:** NOT STARTED

### Goal

Prove Runtime correctness end to end with Fake Plugins before introducing real middleware/storage complexity.

### Prerequisites

- [ ] M0 PASS.
- [ ] M1 PASS.

### Scope

#### Runtime Pipeline

- [ ] Binding validation.
- [ ] Ingress.
- [ ] Admission.
- [ ] Payload materialization.
- [ ] Gate Pass.
- [ ] AcceptanceSequence.
- [ ] ordered Accepted publication.
- [ ] Dispatch Queue.
- [ ] single logical Storage Dispatch Lane.
- [ ] Storage operation tracking.
- [ ] Completion Queue.
- [ ] Completion Domain.
- [ ] Stop.
- [ ] Failure cutoff.
- [ ] Quiescence.

#### Fake Acquisition

- [ ] single producer.
- [ ] multi producer.
- [ ] single Stream.
- [ ] multiple Streams.
- [ ] descriptor changes.
- [ ] Copy.
- [ ] Adopt.
- [ ] Retain.
- [ ] CallerResponsible.
- [ ] RuntimeResponsible.
- [ ] burst.
- [ ] high-rate.
- [ ] out-of-order producer timing.
- [ ] blocked Admission Wait.
- [ ] Stop while submitting.

#### Fake Storage

- [ ] submit non-OK before HandoffAccepted.
- [ ] synchronous PayloadConsumed.
- [ ] PayloadPending → async PayloadConsumed.
- [ ] synchronous PersistenceSucceeded.
- [ ] synchronous PersistenceFailed.
- [ ] synchronous PersistenceUnknown.
- [ ] PersistencePending → async Succeeded.
- [ ] PersistencePending → async Failed.
- [ ] PersistencePending → async Unknown.
- [ ] independent Payload/Persistence ordering.
- [ ] Storage stall.
- [ ] background fatal failure.
- [ ] flush.
- [ ] close.
- [ ] duplicate callback violation.
- [ ] missing callback scenario.
- [ ] same-stack reentrant callback violation.
- [ ] callback after close violation.

### Deliverables

- [ ] Complete Fake E2E pipeline.
- [ ] Runtime Stop/Failure convergence.
- [ ] Fake Plugin correctness suite.
- [ ] E2E stress suite.

### Verification

- [ ] Gate-pass Accepted cannot be dropped by ordinary pressure.
- [ ] same-Stream AcceptanceSequence order == Storage handoff order.
- [ ] no false global ordering across Streams.
- [ ] PAYLOAD_PENDING → exactly one `payload_consumed()`.
- [ ] synchronous PayloadConsumed → no payload callback.
- [ ] PERSISTENCE_PENDING → exactly one `persistence_completed()`.
- [ ] synchronous persistence terminal → no persistence callback.
- [ ] PayloadConsumed + PersistencePending releases Payload early.
- [ ] duplicate callback detected.
- [ ] same-stack reentrant callback detected; cross-thread race reconciled without false violation.
- [ ] post-close callback detected.
- [ ] Stop under multi-producer load converges.
- [ ] Stop with PayloadPending converges.
- [ ] Stop with PersistencePending converges.
- [ ] primary failure preserved.
- [ ] secondary cleanup errors separately reported.
- [ ] Plugin-origin lifetime prevents premature destroy/unload.

### Exit Gate

- [ ] Accepted no-drop invariant proven.
- [ ] per-Stream ordering proven.
- [ ] Runtime permits/counters converge after Stop.
- [ ] both Storage axes exactly-once rules proven.
- [ ] Payload early-release case proven.
- [ ] all completion contract violations detected.
- [ ] Stop/failure reaches quiescence.
- [ ] required ASan/UBSan/TSan stress suite clean.

### Out of Scope

Do not implement in M2:

- ROS2.
- DDS.
- MCAP.
- SQLite3.
- final AppImage release qualification.
- final performance tuning.

---

## M3 — ROS2 Acquisition + MCAP Storage Vertical Slice

**Status:** NOT STARTED

### Goal

Implement the first real Acquisition-to-Storage chain on top of the already validated Runtime.

### Prerequisites

- [ ] M0 PASS.
- [ ] M1 PASS.
- [ ] M2 PASS.
- [ ] ROS2 mapping reviewed against current ABI.
- [ ] MCAP mapping reviewed against current ABI and frozen mcap-cpp version.

### Scope

#### ROS2 Acquisition

- [ ] Plugin lifecycle.
- [ ] graph discovery.
- [ ] Source mapping.
- [ ] Clock mapping.
- [ ] Schema mapping.
- [ ] Stream mapping.
- [ ] StreamDescriptor publication.
- [ ] QoS / ConfigView.
- [ ] serialized payload acquisition.
- [ ] Copy baseline.
- [ ] Retain/loan only if concrete lifetime evidence justifies it.
- [ ] callback quiescence.
- [ ] failure reporting.

#### MCAP Storage

- [ ] Plugin lifecycle.
- [ ] Schema projection.
- [ ] Channel projection.
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

### MCAP

- [ ] Schema/Channel mapping.
- [ ] descriptor evolution.
- [ ] same-Stream ordering.
- [ ] payload exactness.
- [ ] metadata sidecar pairing.
- [ ] bounded metadata batch.
- [ ] flush.
- [ ] close.
- [ ] failure injection.

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
- [ ] AcceptanceSequence/timestamps preserved.
- [ ] ROS2 Stop quiescence passes.
- [ ] MCAP PayloadConsumed lifetime test passes.
- [ ] MCAP PersistencePending completion tests pass.
- [ ] sidecar/descriptor failure-recovery tests pass.
- [ ] initial throughput/CPU/RSS/latency baseline recorded.

### Out of Scope

Do not implement in M3:

- DDS.
- SQLite3.
- final production tuning.
- final release qualification.
- hot reload.

---

## M4 — Production Hardening

**Status:** NOT STARTED

### Goal

Make the proven M3 system measurable, bounded under stress, failure-hardened and release-qualified.

### Prerequisites

- [ ] M3 PASS.

### Scope

#### Correctness hardening

- [ ] ASan suite.
- [ ] UBSan suite.
- [ ] TSan suite.
- [ ] fuzz targets where appropriate.
- [ ] repeated Plugin load/unload.
- [ ] stress.
- [ ] soak.
- [ ] Stop/failure stress.

#### Performance

- [ ] 10-camera encoded profile.
- [ ] 10-camera raw RGB8 stress profile.
- [ ] mixed workload.
- [ ] PayloadPool tuning.
- [ ] Queue/dispatch tuning.
- [ ] Storage batching tuning.
- [ ] allocator A/B.
- [ ] copy/allocation accounting.
- [ ] latency histograms.

#### Release

- [ ] x86_64 AppImage.
- [ ] aarch64 AppImage.
- [ ] GLIBC gate.
- [ ] GLIBCXX gate.
- [ ] CXXABI gate.
- [ ] RPATH/RUNPATH audit.
- [ ] recursive DT_NEEDED closure.
- [ ] Plugin dependency closure.
- [ ] SONAME collision check.
- [ ] Ubuntu 22.04 clean-system test.
- [ ] Ubuntu 24.04 clean-system test.
- [ ] native ARM64 qualification.
- [ ] SBOM.
- [ ] THIRD_PARTY_NOTICES.
- [ ] checksums.
- [ ] release manifest.

### Deliverables

- [ ] benchmark suite.
- [ ] stress/soak suite.
- [ ] failure/crash matrix.
- [ ] performance baseline.
- [ ] qualified release artifacts.
- [ ] release metadata/licenses.

### Verification

- [ ] throughput.
- [ ] CPU.
- [ ] RSS.
- [ ] allocations/record.
- [ ] copies/record.
- [ ] P50/P95/P99/P99.9.
- [ ] Payload HWM.
- [ ] Dispatch HWM.
- [ ] Storage inflight HWM.
- [ ] Admission wait/timeout.
- [ ] disk slowdown/full/error.
- [ ] Stop at peak load.
- [ ] Plugin failure.
- [ ] Payload exhaustion.
- [ ] Stop during both Storage pending axes.
- [ ] primary/secondary error preservation.

### Exit Gate

- [ ] required sanitizer suites clean.
- [ ] stress/soak has no unbounded growth.
- [ ] bounded high-water behavior proven.
- [ ] 10-camera profiles recorded.
- [ ] mixed workload passes.
- [ ] crash/failure matrix passes.
- [ ] each performance optimization has before/after evidence.
- [ ] x86_64 AppImage qualified on Ubuntu22/24.
- [ ] aarch64 AppImage qualified on native ARM64.
- [ ] ABI/runtime symbol-version gates pass.
- [ ] recursive Plugin dependency closure passes.
- [ ] SBOM/license/release manifest complete.

### Out of Scope

DDS and SQLite3 are not required for M4 PASS.

---

## M5 — DDS Acquisition + SQLite3 Storage

**Status:** NOT STARTED

### Goal

Validate that the frozen architecture generalizes to a second Acquisition middleware and a second Storage backend without backend-specific exceptions in Runtime Core or Stable ABI.

### Prerequisites

- [ ] M4 PASS.
- [ ] DDS implementation/version reviewed.
- [ ] SQLite3 version/build policy reviewed.

### Scope

##### DDS Acquisition

- [ ] Plugin lifecycle.
- [ ] Participant/discovery/DataReader.
- [ ] Source mapping.
- [ ] Clock mapping.
- [ ] Schema/XTypes mapping.
- [ ] Stream mapping.
- [ ] descriptor evolution.
- [ ] source-native serialized / explicit reserialization path.
- [ ] Copy baseline.
- [ ] Retain/loan only if lifetime proof exists.
- [ ] listener/worker quiescence.
- [ ] failure/recovery.

##### SQLite3 Storage

- [ ] Plugin lifecycle.
- [ ] descriptor projection.
- [ ] prepared statements.
- [ ] bounded transaction batching.
- [ ] Payload binding lifetime.
- [ ] persistence outcomes.
- [ ] rollback.
- [ ] Unknown outcome.
- [ ] WAL/journal policy.
- [ ] finalization marker.
- [ ] recovery.

### Deliverables

- [ ] DDS Acquisition Plugin DSO.
- [ ] SQLite3 Storage Plugin DSO.
- [ ] Plugin manifests.
- [ ] DDS → Runtime → MCAP validation.
- [ ] ROS2 → Runtime → SQLite3 validation.
- [ ] DDS → Runtime → SQLite3 validation where useful.
- [ ] Canonical mapping symmetry tests.
- [ ] cross-backend compatibility tests.

### Verification

#### DDS

- [ ] multiple Writers per Stream.
- [ ] SourceSequence present/absent.
- [ ] reader-side timestamp.
- [ ] type evolution.
- [ ] loan lifetime if enabled.
- [ ] early callback before activation.
- [ ] Stop/quiescence.
- [ ] same-semantics recovery.

#### SQLite3

- [ ] descriptor FK readiness.
- [ ] unsigned sequence ordering projection.
- [ ] SQLITE_STATIC lifetime.
- [ ] SQLITE_TRANSIENT copied-memory accounting.
- [ ] transaction batching.
- [ ] COMMIT BUSY.
- [ ] rollback.
- [ ] PersistenceFailed.
- [ ] PersistenceUnknown.
- [ ] WAL/finalization cases.
- [ ] close with prepared statements.

Required initial direct-path mapping:

- [ ] `payload_state == PAYLOAD_CONSUMED`.
- [ ] `persistence_state == PERSISTENCE_PENDING`.
- [ ] COMMIT emits exactly one persistence completion.
- [ ] no payload callback for synchronously consumed Payload.

#### Canonical symmetry

- [ ] payload logical bytes equivalent across MCAP/SQLite projections.
- [ ] AcceptanceSequence preserved.
- [ ] descriptor interpretation preserved.
- [ ] timestamp semantics preserved.

### Exit Gate

- [ ] DDS integrates without Runtime Core semantic changes.
- [ ] SQLite3 integrates without Runtime Core semantic changes.
- [ ] no DDS type leaks to common Public API/ABI.
- [ ] no SQLite type leaks to common Public API/ABI.
- [ ] DDS Stop/quiescence passes.
- [ ] SQLite PayloadConsumed/PersistencePending lifetime test passes.
- [ ] SQLite Failed/Unknown outcome tests pass.
- [ ] Canonical mapping symmetry demonstrated.
- [ ] no backend-specific exception added to Stable Plugin ABI.
- [ ] V1 requirements satisfied across both Acquisition and both Storage backend families.

### Out of Scope

Not part of V1 milestone plan:

- SocketCAN implementation.
- additional Storage formats.
- Runtime hot reload.
- Plugin process sandboxing.
- remote/distributed Plugin execution.
- WASM Plugin runtime.

---

## Plan maintenance rules

### One active milestone

Exactly one milestone is `ACTIVE`.

Do not promote the next milestone until the current Exit Gate is satisfied.

### Evidence before PASS

A completed checkbox is not enough.

Before a milestone becomes PASS, `status.md` must summarize available evidence:

- build,
- tests,
- sanitizer,
- benchmark,
- release qualification,

as applicable.

### Progress terminology

Use precise state:

```text
implemented
compiled
unit-tested
integration-tested
sanitizer-tested
benchmark-tested
release-qualified
```

Do not use `done` when validation has not run.

### Design conflicts

If implementation requires changing a frozen contract:

1. stop changing the contract in code;
2. record the conflict;
3. review the relevant `docs/design/*.md`;
4. update the design deliberately if approved;
5. then continue implementation.

### Plan changes

- Update checkboxes only when repository state supports them.
- Keep this file executable and factual.
- Detailed design belongs in `docs/design/`.
- Do not create a new planning document unless the existing plan cannot represent the work cleanly.
