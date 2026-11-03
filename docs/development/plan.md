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

Current milestone: **M0 — ACTIVE**

Only one milestone may be `ACTIVE` at a time.

---

## M0 — Foundation + Plugin ABI

**Status:** PASS

**Current sub-phase:** **M0-E + Closure Fix — PASS**. Stable ABI core prefix is
frozen. Native arm64 qualification is deferred by D-034.

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
- [x] Add `rodaq.runtime.projection.v1` service ABI/plumbing for typed descriptor projections.
- [x] Add `rodaq.runtime.clock_correlation.v1` read-only service ABI/plumbing for activation replay-time anchor.
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
- [ ] M1: ADOPT before/after transfer ownership behavior.
- [ ] M1: RETAIN acquisition-point and original-native-owner behavior.
- [ ] M2: Gate-pass/AcceptanceSequence ordering and PreAdmissionRejected behavior.
- [ ] M1: Clock/Stream native registration idempotence, conflict and generation behavior.
- [ ] M1: identical native_id in different Plugin instances remains isolated.

#### Storage ABI

- M0 qualifies ABI representability. Handoff reconciliation and asynchronous
  completion behavior is assigned to M1/M2.
- [ ] M2: submit non-OK means HandoffAccepted never established.
- [x] PayloadConsumed synchronous result representable.
- [x] PayloadPending representable.
- [x] PersistenceSucceeded synchronous result representable.
- [x] PersistenceFailed synchronous result representable.
- [x] PersistenceUnknown synchronous result representable.
- [x] PersistencePending representable.
- [x] Asynchronous/systemic Storage diagnostics use complete `rodaq_error` values.
- [x] Descriptor readiness FAILED carries complete diagnostics.
- [ ] M2: no same-stack reentrant callback; cross-thread completion race reconciliation.
- [ ] M2: async publication and early-completion buffering rules.

#### Service / Config / lifetime

- [x] required Service success/failure.
- [x] optional Service absence.
- [x] required `rodaq.acquisition.control.v1` present/compatible.
- [x] required `rodaq.storage.control.v1` present/compatible.
- [x] missing/incompatible required control interface prevents activation.
- [ ] M2: `quiesce_plugin()` blocks current/future Submit routing.
- [ ] M2: normal Stop quiesce → drain → destructive stop behavior.
- [ ] M2: Binding abort KEEP_PAUSED / RESUME_OLD and terminal transition state.
- [ ] M1/M2: Runtime lifecycle snapshot recovery gating and stale-state rejection.
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
- [ ] M2: control timeout/quiescence, stream isolation, overlapping mutation and quarantine behavior.
- [x] live instance prevents unload.
- [x] live lease prevents unload.
- [x] relevant ASan/UBSan suite clean.

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
- FastDDS.
- MCAP.
- SQLite3 (future).
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
- [ ] MCAP mapping reviewed against current ABI and a directly pinned RoDAQ mcap-cpp version/commit (no ROS2 mcap_vendor runtime dependency).

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
- [ ] `rodaq.projection.ros2.v1` generation and registration.
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

### Out of Scope

Do not implement in M3:

- FastDDS Acquisition (implemented in M4).
- SQLite3 Storage implementation.
- final production tuning.
- final release qualification.
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
- [ ] `rodaq.projection.ros2.v1` generation.
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
- [ ] mixed small/large-message workload.
- [ ] PayloadPool / queue / batching benchmark-driven tuning.
- [ ] CPU / RSS / allocation / copy-count / tail-latency baselines.
- [ ] ros2 bag play / PlotJuggler / Foxglove interoperability regression.
- [ ] x86_64 / aarch64 AppImage qualification.
- [ ] Ubuntu 22.04 / 24.04 clean-system qualification.
- [ ] GLIBC / GLIBCXX / CXXABI / RPATH / DT_NEEDED gates.
- [ ] FastDDS-only deployment without ROS2 runtime.
- [ ] plugin dependency collision audit covers ROS2 `rmw_fastrtps_*` versus RoDAQ-pinned Fast DDS/Fast CDR.
- [ ] SBOM / licenses / checksums / release manifest.

### Deliverables

- [ ] benchmark and soak reports.
- [ ] crash/failure matrix evidence.
- [ ] interoperability regression corpus/results.
- [ ] qualified release artifacts for required architectures.
- [ ] SBOM / THIRD_PARTY_NOTICES / release manifest.

### Verification

- [ ] bounded high-water marks under camera and mixed workloads.
- [ ] no long-run resource growth.
- [ ] failure paths preserve deterministic quiescence and Artifact state.
- [ ] performance optimizations have before/after evidence.
- [ ] ROS2 and FastDDS interoperability remains green after tuning.

### Exit Gate

- [ ] required sanitizer and long-soak suites clean.
- [ ] bounded-resource behavior proven.
- [ ] camera/mixed-workload baselines recorded.
- [ ] crash/failure matrix passes.
- [ ] ROS2 + FastDDS MCAP interoperability qualification passes.
- [ ] x86_64 and aarch64 release qualification passes.
- [ ] FastDDS recording-host package has no ROS2 runtime dependency.
- [ ] SBOM/license/release manifest complete.

### Out of Scope

- SQLite3 implementation.
- SocketCAN implementation.
- Generic DDS implementation.
- additional Storage formats.
- runtime hot reload / process sandboxing.
