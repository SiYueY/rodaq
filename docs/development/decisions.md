# RoDAQ Durable Development Decisions

This file is an index of durable decisions that future agents should not rediscover from chat history.

Detailed rationale remains in `docs/design/`. Keep entries short. Add a new entry only when forgetting the choice could materially change architecture, implementation, compatibility, or validation.

---

## D-001 — C++17 and platform baseline

**Status:** Accepted

RoDAQ V1 uses C++17 with GCC 11.x as the reference compiler.

Release targets:

- Ubuntu 22.04 / 24.04.
- amd64 / arm64.

Release artifacts are built against the Ubuntu 22.04 compatibility baseline.

**Reference:** `docs/design/rodaq_v1_system_design.md`, coding guidelines.

---

## D-002 — Runtime is the sole Acquisition/Storage mediator

**Status:** Accepted

Acquisition Plugins and Storage Plugins do not call each other directly.

The data path is:

```text
Native Source
  -> Acquisition Plugin
  -> RoDAQ Runtime
  -> Storage Plugin
  -> Persistent Output
```

Runtime owns Admission, Payload lifetime, ordering, dispatch, completion and lifecycle coordination.

**Reference:** `docs/design/architecture.md`, `runtime.md`.

---

## D-003 — Canonical identity is independent from backend representation

**Status:** Accepted

Canonical Model is the semantic authority.

Record identity is based on:

```text
(StreamId, AcceptanceSequence)
```

`StreamId` is logical identity; `StreamDescriptorId` is the immutable interpretation contract.

Changes to StreamName, Schema, PayloadEncoding or TimestampLayout create a new StreamDescriptor while keeping the same logical Stream identity.

**Reference:** `canonical_data_model.md`, `canonical_profile.md`, system baseline.

---

## D-004 — V1 Plugin binary boundary uses stable C ABI

**Status:** Accepted

No C++ virtual class, STL type, RTTI object or third-party type crosses the Plugin binary boundary.

A C++17 Plugin SDK is provided only as an authoring facade over the stable C ABI.

**Reference:** `docs/design/rodaq_plugin_system.md`.

---

## D-005 — Acquisition and Storage use family-specific protocols

**Status:** Accepted

Do not use one generic `rodaq_plugin_api` plus `plugin_kind`.

Use separate Acquisition and Storage entrypoints/protocols.

**Reference:** `docs/design/rodaq_plugin_system.md`.

---

## D-006 — Plugin extensibility is directional

**Status:** Accepted

Plugin -> Runtime extensibility uses the minimal Service Registry.

Runtime -> Plugin optional capability discovery uses Plugin Extension Query.

New optional capability should not expand the core family protocol unless it is truly part of every implementation's base contract.

**Reference:** `docs/design/rodaq_plugin_system.md`.

---

## D-007 — One DSO represents one Plugin implementation and one family in V1

**Status:** Accepted

A DSO may create multiple instances, but V1 does not support multiple independent Plugin implementations or mixed Acquisition/Storage families in one DSO.

**Reference:** `docs/design/rodaq_plugin_system.md`.

---

## D-008 — Dynamic Plugin loading does not imply hot reload

**Status:** Accepted

Plugins may be selected and loaded dynamically before Running.

Plugin topology is frozen while Running.

V1 does not implement live DSO replacement, Stream migration, or in-place hot reload.

**Reference:** `docs/design/rodaq_plugin_system.md`.

---

## D-009 — Storage submit uses independent Payload / Persistence completion axes

**Status:** Accepted

Storage `submit()` first establishes Handoff responsibility:

```text
return non-OK
    -> HandoffAccepted never established
    -> no retained Runtime Payload access after return
    -> no normal completion callback for the operation

return OK
    -> HandoffAccepted
```

After HandoffAccepted, two axes are tracked independently:

```text
Payload:
    Pending / Consumed

Persistence:
    Pending / Succeeded / Failed / Unknown
```

A Pending Payload axis terminalizes through exactly one `payload_consumed()` notification. A Pending Persistence axis terminalizes through exactly one `persistence_completed()` notification.

If an axis is already terminal in the synchronous `submit()` result, no callback is sent for that axis.

Same-stack synchronous reentrant callbacks during `submit()` are forbidden. A genuine cross-thread completion may race with `submit()` return; Runtime must reconcile it with the returned result.

This separation allows large Payload backing to be released while transaction/batch persistence is still pending.

**Reference:** `docs/design/rodaq_plugin_system.md`, `docs/design/storage_plugin.md`.

---

## D-010 — Accepted records are not ordinary queue-drop candidates

**Status:** Accepted

Before Gate Pass, Admission reserves all resources needed for eventual handoff or rolls back completely.

After Gate Pass, ordinary queue/resource pressure must not silently drop an Accepted Record.

Queue capacity is transport; ResourceBudget/Admission owns correctness.

**Reference:** `docs/design/admission.md`, system baseline.

---

## D-011 — Payload V1 is contiguous and immutable

**Status:** Accepted

V1 physical Payload is one contiguous immutable byte range.

Ingress ownership modes are:

- Copy.
- Adopt.
- Retain.

Externally retained memory counts against Runtime Payload budgets.

Payload consumption and Storage durability are separate concepts.

**Reference:** `docs/design/payload.md`, Plugin System design.

---

## D-012 — One active acquisition path per Stream

**Status:** Accepted

A Canonical Stream has at most one active acquisition path / owning Acquisition Plugin / current Binding at a time in V1.

Redundant or merged producers require an explicit future design.

**Reference:** `architecture.md`, `runtime.md`, system baseline.

---

## D-013 — Third-party code is pinned as Git submodules

**Status:** Accepted

Production third-party sources live under `third_party/` as pinned Git submodules.

Normal builds must not fetch dependencies from the network.

Third-party code is not edited directly; local changes require a patch set or maintained fork.

**Reference:** `docs/design/rodaq_v1_system_design.md`.

---

## D-014 — Dependency baseline

**Status:** Accepted for V1 implementation baseline

Production baseline:

- oneTBB 2022.3.0 — shared, `TBB::tbb` only.
- spdlog 1.17.0 — static, external fmt.
- fmt 12.1.0 — static.
- HdrHistogram_c 0.12.1 — core-only static.
- yaml-cpp 0.9.0 — static + PIC.
- expected-lite 0.10.0 — private implementation candidate only; must not leak through Public API.
- mimalloc 3.6.1 — optional explicit allocator backend.

Development:

- GoogleTest 1.18.0.
- Google Benchmark 1.10.0.

The `rodaq::Result<T>` implementation choice remains an early M0 code decision; this does not authorize exposing expected-lite types publicly.

**Reference:** system baseline.

---

## D-015 — V1 uses one authoritative M0–M5 development milestone model

**Status:** Accepted

The only V1 development-stage numbering is:

```text
M0 — Foundation + Plugin ABI
M1 — Runtime Core Primitives
M2 — Fake End-to-End Runtime
M3 — ROS2 Acquisition + MCAP Storage Vertical Slice
M4 — Production Hardening
M5 — DDS Acquisition + SQLite3 Storage
```

Do not maintain a second `Phase 1..N` numbering system.

Each milestone must define and satisfy:

```text
Goal
Prerequisites
Scope
Deliverables
Verification
Exit Gate
Out of Scope
```

Fake Plugins and the Fake End-to-End Runtime must pass before the first real ROS2 + MCAP vertical slice. Do not start by coupling Runtime implementation directly to ROS2 or MCAP.

`docs/design/rodaq_v1_system_design.md` Chapter 6 is the authoritative milestone/Gate specification. `docs/development/plan.md` is the execution-state checklist and must use the same milestone names.

**Reference:** `docs/design/rodaq_v1_system_design.md`, `docs/development/plan.md`.

---


---

## D-016 — Acquisition submit separates Admission outcome from Payload responsibility

**Status:** Accepted

Acquisition submit carries three orthogonal domain dimensions:

```text
AdmissionOutcome
RejectionReason
PayloadDisposition
```

`rodaq_status` is reserved for ABI/API call failure and is not used as the normal Admission result.

For ADOPT and RETAIN, calling `submit()` does not itself transfer responsibility. Runtime must first validate the input and reserve the required Payload object/byte capacity, then cross the corresponding Adopt transfer / Retain acquisition point.

Before that point:

```text
PayloadDisposition = CallerResponsible
```

After that point:

```text
PayloadDisposition = RuntimeResponsible
```

For RETAIN, the disposition applies only to the submission-specific independent lifetime anchor; it does not transfer ownership of the Plugin's original native object.

RejectionReason must never be used to infer Payload responsibility.

**Reference:** `docs/design/payload.md`, `docs/design/admission.md`, `docs/design/rodaq_plugin_system.md`, `docs/design/acquisition_plugin.md`.


---

## D-017 — V1 required control extensions are part of family conformance

**Status:** Accepted

V1 Acquisition and Storage keep small core family vtables, but correctness-critical control protocols are required extensions:

```text
rodaq.acquisition.control.v1
rodaq.storage.control.v1
```

Acquisition control carries Binding-switch and two-level quiescence coordination. `quiesce_stream()` is stream-local; `quiesce_plugin()` is the required instance-wide **non-destructive** barrier used before Runtime drain. Destructive native shutdown remains in core `stop()` and is invoked only after Runtime/Storage drain and Plugin-origin Payload anchors no longer require native release capability. Binding abort carries explicit `KEEP_PAUSED / RESUME_OLD` disposition.

Storage control carries capability negotiation and Descriptor readiness. Missing required control extension is protocol incompatibility, not optional feature absence.

**Reference:** `docs/design/rodaq_plugin_system.md`, `docs/design/acquisition_plugin.md`, `docs/design/storage_plugin.md`.

---

## D-018 — ABI diagnostics are caller-owned and self-contained

**Status:** Accepted

`rodaq_error` uses fixed inline caller-owned storage. No callee-owned diagnostic string pointer may escape an ABI call.

Caller-owned APPENDABLE outputs use `struct_size` as entry capacity and return initialized-prefix size. APPENDABLE structs are not embedded or passed by value; nested by-value ABI objects must be FROZEN.

**Reference:** `docs/design/rodaq_plugin_system.md`.

---

## D-019 — Storage completion permits cross-thread race with submit return

**Status:** Accepted

V1 forbids same-stack synchronous reentrant completion, but does not require an async worker callback to occur strictly after the caller observes `submit()` return.

Runtime creates operation tracking before calling Storage `submit()`, records racing completion events, then reconciles them with the returned synchronous result.

A Plugin may publish an operation to an async worker only after irrevocably committing to return `OK` with the corresponding axis `PENDING`.

That irreversible Plugin-internal decision is named `HandoffCommit`. It establishes the provisional async publication/lease point but does not itself establish ABI-visible `HandoffAccepted`; `submit() return == OK` does that. Runtime reconciliation merges any early worker completion. Early completion never permits Runtime to reclaim Payload/operation state before the current submit call and reconcile phase can no longer access it.

**Reference:** `docs/design/rodaq_plugin_system.md`, `docs/design/storage_plugin.md`.

---

## D-020 — Dispatch reservation must reserve physical enqueue capacity

**Status:** Accepted

A dispatch permit is valid only if post-Gate enqueue no longer depends on a failure-capable ordinary allocation.

The V1 correctness baseline uses a preallocated/fixed-capacity dispatch transport. oneTBB queues remain an implementation/benchmark candidate only if instrumentation proves reservation-to-enqueue requires no new failure-capable allocation.

**Reference:** `docs/design/admission.md`, `docs/design/runtime.md`, `docs/design/rodaq_v1_system_design.md`.

---

## D-021 — V1 freeze has three distinct levels

**Status:** Accepted

```text
SEMANTIC-FROZEN
ABI-IMPLEMENTATION-FROZEN
BACKEND-CONFORMANCE-QUALIFIED
```

Static ROS2/DDS/MCAP/SQLite3 mapping reviews are required for Semantic Freeze before ABI implementation. Executable backend round-trip/conformance qualification occurs later in M3/M5 and does not block M0 foundation/ABI implementation.

**Reference:** `docs/design/canonical_profile.md`, `docs/design/rodaq_v1_system_design.md`.

---

## D-022 — Gate pass precedes irreversible AcceptanceSequence allocation

**Status:** Accepted

Admission uses one per-Stream logical commit domain:

```text
confirm sequence capacity + final eligibility
→ Gate pass / in-flight entry
→ assign irreversible AcceptanceSequence
→ ordered Accepted publication
```

PreAdmissionRejected never consumes AcceptanceSequence. A Gate-passed attempt may consume a sequence and then terminalize as abnormal AdmissionRejected if the commit path becomes untrustworthy; the sequence is never rolled back or reused.

**Reference:** `docs/design/admission.md`, `docs/design/rodaq_v1_system_design.md`, `docs/design/rodaq_plugin_system.md`.

---

## D-023 — Clock and Stream native registration keys are Plugin-instance scoped

**Status:** Accepted

Clock and Stream registration keys are `(PluginInstance, native_id bytes)`. Same-key re-registration within one activation is idempotent when immutable facts agree; Clock conflicting immutable facts return Conflict. Clock continuity reset/restart requires new native-id generation evidence. Different Plugin instances are never automatically merged by matching native bytes.

SourceId remains different: it is already canonicalized by the Acquisition Mapping and is interned Scope-wide by exact Canonical SourceId bytes.

**Reference:** `docs/design/rodaq_plugin_system.md`, `docs/design/acquisition_plugin.md`.

---

## D-024 — Runtime lifecycle snapshot gates recovery attempts but is not a capability token

**Status:** Accepted

`rodaq.runtime.common.v1::get_lifecycle_snapshot()` is a bounded, thread-safe Runtime-facing query. A recovery attempt may start only from a snapshot reporting `RUNNING` and `normal_acquisition_allowed=true`.

The snapshot can become stale immediately; subsequent Runtime registration/report/submit calls remain authoritative. Plugins do not continuously poll it.

**Reference:** `docs/design/rodaq_plugin_system.md`, `docs/design/acquisition_plugin.md`.

## Adding a decision

Use the next `D-NNN` identifier.

Each entry should contain:

- status,
- decision,
- short reason only if needed,
- canonical design reference.

Do not copy large design sections into this file.
