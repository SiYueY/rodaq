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

Runtime -> Plugin capability discovery uses Plugin Interface Query. Optional
capabilities are represented as optional extension interfaces.

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
- expected-lite 0.10.0 — pinned Public C++ header dependency used directly by `rodaq::Result<T>`; see D-025.
- mimalloc 3.6.1 — optional explicit allocator backend.

Development:

- GoogleTest 1.18.0.
- Google Benchmark 1.10.0.

The `rodaq::Result<T>` policy is frozen by D-025.

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
M4 — FastDDS Acquisition + MCAP Interoperability
M5 — Production Hardening + Release Qualification
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

## D-017 — V1 required control interfaces are part of family conformance

**Status:** Accepted

V1 Acquisition and Storage keep small core family vtables, while
`query_interface()` discovers typed Plugin interfaces. Correctness-critical
control protocols are required interfaces:

```text
rodaq.acquisition.control.v1
rodaq.storage.control.v1
```

Acquisition control carries Binding-switch and two-level quiescence coordination. `quiesce_stream()` is stream-local; `quiesce_plugin()` is the required instance-wide **non-destructive** barrier used before Runtime drain. Destructive native shutdown remains in core `stop()` and is invoked only after Runtime/Storage drain and Plugin-origin Payload anchors no longer require native release capability. Binding abort carries explicit `KEEP_PAUSED / RESUME_OLD` disposition.

Storage control carries capability negotiation and Descriptor readiness. Missing
or incompatible required control interface is protocol incompatibility, not
optional feature absence.

**Reference:** `docs/design/rodaq_plugin_system.md`, `docs/design/acquisition_plugin.md`, `docs/design/storage_plugin.md`.

---

## D-018 — ABI diagnostics are caller-owned and self-contained

**Status:** Accepted

`rodaq_error` uses fixed inline caller-owned storage. No callee-owned
diagnostic string pointer may escape an ABI call.

Caller-owned APPENDABLE outputs use `struct_size` as entry capacity. The callee
zeroes and initializes at most `min(capacity, sizeof(callee-known-type))` bytes
and returns exactly that initialized-prefix size in `struct_size`. It never
reports bytes it did not initialize.

Output `flags` may contain documented bits only; V1 Storage capabilities has no
defined capability flags and therefore returns zero. Output reserved fields are
always zero. `rodaq_error`'s documented message-truncation flag remains a valid
diagnostic flag.

APPENDABLE structs are not embedded or passed by value; nested by-value ABI
objects must be FROZEN.

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

---

## D-025 — RoDAQ V1 Result uses expected-lite directly

**Status:** Accepted

RoDAQ V1 freezes:

```cpp
namespace rodaq {

template <typename T>
using Result = nonstd::expected<T, Error>;

}
```

The pinned dependency is expected-lite 0.10.0. RoDAQ does not maintain a custom
`expected` implementation. expected-lite is an explicit Public C++ source-level
header dependency because it is visible through the `rodaq::Result<T>` alias, but
it must never appear in the Stable C Plugin ABI.

V1 explicitly selects expected-lite's nonstd implementation. A consumer language
mode or toolchain configuration must not silently switch the alias to
`std::expected`. Migration to `std::expected` requires a future explicit decision.

This decision is the only approved exception to the default rule that RoDAQ-owned
Public C++ APIs do not expose third-party types. Stable C ABI contracts continue
to prohibit all third-party types.

**Reference:** `docs/design/rodaq_v1_system_design.md`.

---

## D-026 — Git gitlinks are the sole dependency commit authority

**Status:** Accepted

Exact third-party commit pins live only in the parent repository's Git submodule
gitlinks. `cmake/dependencies.cmake` records human-readable release/tag metadata
and build integration paths, but does not duplicate commit SHAs.

Normal configure and build do not require a Git executable and do not perform
network access. Developer qualification may use Git to inspect the gitlinks.

**Reference:** `docs/design/rodaq_v1_system_design.md`,
`docs/design/rodaq_plugin_system.md`.

---

## D-027 — Plugin loader identity and failure classification

**Status:** Accepted

Plugin IDs contain at least two non-empty dot-separated segments. Each segment
contains only ASCII `a-z`, `0-9`, `_` or `-`. Plugin versions use complete
SemVer 2.0 syntax. Plugin `id`, `name` and `version` are non-empty valid UTF-8
without embedded NUL; description may be empty.

Loader failures use the existing ErrorCode taxonomy:

- missing file or symbol: `NotFound`,
- `dlopen` or loader-runtime failure: `Unavailable`,
- malformed ABI, protocol, descriptor, family or symbol provenance:
  `InvalidArgument`,
- duplicate Plugin ID from a different canonical DSO: `Conflict`,
- lifecycle misuse: `InvalidState`,
- invariant failure: `InternalError`.

No loader-specific ErrorCode family is introduced.

**Reference:** `docs/design/rodaq_plugin_system.md`.

---

## D-028 — Plugin-facing public headers use the rodaq/plugin hierarchy

**Status:** Accepted

The Stable C Plugin ABI is installed under `include/rodaq/plugin/abi/*.h`.
Common Plugin ABI primitives are defined in `common.h`. The future C++17 Plugin
SDK uses `include/rodaq/plugin/*.hpp` and may depend on the Stable C ABI; the
Stable C ABI must not depend on the C++ SDK.

The former `include/rodaq/abi/` source path is removed without compatibility
headers during the pre-release phase.

**Reference:** `docs/design/rodaq_plugin_system.md`,
`docs/design/rodaq_v1_system_design.md`.

---

## D-029 — Storage domain outcomes use the full Stable ABI Error representation

**Status:** Accepted

Synchronous persistence failure, asynchronous persistence failure, Descriptor
readiness failure, and systemic Storage failures use `rodaq_error` for
self-contained diagnostics.

Function-level `out_error` remains reserved for ABI/control-call failure.
Domain outcomes and call-boundary failures must remain distinct.

`rodaq_error` pointers passed through Storage Runtime callbacks are borrowed for
the callback duration. The Runtime must copy the value before retaining it.

**Reference:** `docs/design/rodaq_plugin_system.md`,
`docs/design/storage_plugin.md`.

---

## Adding a decision

Use the next `D-NNN` identifier.

Each entry should contain:

- status,
- decision,
- short reason only if needed,
- canonical design reference.

Do not copy large design sections into this file.

---

## D-030 — V1 implements ROS2 + FastDDS Acquisition and ROS2-compatible MCAP only

**Status:** Accepted

V1 implementation scope is frozen as:

```text
Acquisition
    ROS2
    FastDDS

Storage
    MCAP
```

FastDDS is implemented directly on Fast DDS / Fast CDR, uses DDS-Pipe and DDS Record & Replay as source-level engineering references, and must not require a ROS2 runtime on the recording host. Its V1 qualified input is ROS2-compatible DDS pub/sub traffic, with `rmw_fastrtps_cpp` as the primary qualification baseline.

ROS2 Acquisition and FastDDS Acquisition both publish the versioned typed projection:

```text
rodaq.projection.ros2.v1
```

through the Runtime Projection Registry. MCAP Storage consumes that common projection and must not branch on Acquisition implementation type.

V1 normal release artifacts use the ROS2 MCAP profile and must pass:

```text
ros2 bag play
PlotJuggler
Foxglove
RoDAQ full-fidelity validation
```

SQLite3, Generic DDS, SocketCAN and additional Storage formats remain architecture/design targets but are not V1 implementation or release gates.

This decision supersedes the old scope implication in D-015 that M5 implemented DDS + SQLite3; D-015 still governs the single authoritative M0–M5 milestone model.

**Reference:** `docs/design/rodaq_v1_system_design.md`, `docs/design/acquisition_plugin/fastdds.md`, `docs/design/storage_plugin/mcap.md`.

---

## D-031 — ROS2 and FastDDS Acquisition are alternative V1 process profiles by default

**Status:** Accepted

Both ROS2 Acquisition and FastDDS Acquisition are required V1 implementations, but V1 does not require them to be simultaneously active in one RoDAQ process.

The required deployment profiles are:

```text
ROS2 Acquisition + MCAP
FastDDS Acquisition + MCAP
```

Cross-acquisition equivalence may use separate runs/processes over the same publisher corpus.

Same-process co-activation is supported only after recursive dependency, SONAME, ABI, symbol-resolution and runtime coexistence qualification proves that ROS2 `rmw_fastrtps_*` and the RoDAQ-pinned Fast DDS/Fast CDR dependency closure are compatible. Otherwise PluginManager/config validation must reject the combination before activation.

**Reference:** `docs/design/rodaq_v1_system_design.md`, `docs/design/acquisition_plugin/fastdds.md`.

---

## D-032 — M0-D owns Runtime service plumbing; M0-E owns the Plugin SDK

**Status:** Accepted

M0-D implements the additive projection and clock-correlation Stable Runtime
Service ABI, bounded Service Registry, Runtime-owned ConfigView,
InteropProjectionRegistry, activation ClockCorrelation and Fake
Acquisition/Storage `bind/configure` integration.

M0-D does not implement the C++17 Plugin SDK, exception trampolines, metadata
generation or complete Fake Plugin lifecycle/state-machine qualification. Those
remain M0-E work. M0-D therefore does not satisfy the complete M0 Exit Gate and
must not be reported as full M0 PASS.

**Reference:** `docs/design/rodaq_plugin_system.md`, `runtime.md`,
`docs/design/rodaq_v1_system_design.md`, `docs/development/plan.md`.

## D-033 — M0 plugin foundation is frozen for V1 implementation

**Status:** Accepted

M0 PASS freezes the Stable ABI core prefix, Service Registry, ConfigView,
Plugin Interface Query, C++ SDK authoring surface, exception firewall,
metadata/CMake authoring pipeline and initial V1 compatibility fixtures.
M1/M2 own behavioral ownership, quiescence, timeout and completion proof.

## D-034 — Native arm64 qualification is deferred for M0 closure

**Status:** Accepted

M0 Closure PASS may be granted after full Ubuntu 22.04 amd64 normal/ASan/UBSan
and Ubuntu 24.04 qualification. Native arm64 remains an explicit deferred
qualification and must not be recorded as PASS. A manual self-hosted arm64 CI
job remains available for later evidence without reopening the frozen ABI.

**Reference:** `.github/workflows/m0-qualification.yml`,
`docs/design/rodaq_v1_system_design.md`.

## D-035 — Runtime Service wrappers use typed invalid-state semantics

**Status:** Accepted

Default-constructed or incompletely bound public Runtime Service wrappers must
remain safe. Result-returning APIs report `ErrorCode::InvalidState`; void log,
diagnostic and Storage completion APIs are no-ops. Monotonic time cannot use a
numeric error sentinel, so:

```cpp
Result<std::uint64_t> MonotonicTimeNs() const;
```

This is a C++ SDK source-level contract only. It does not change the Stable C
ABI table or any frozen layout.

**Reference:** `docs/design/rodaq_plugin_system.md`.

## D-036 — M1 ResourceBudget dimensions follow the implemented primitives

**Status:** Accepted

M1 `ResourceAmount` accounts for Runtime record slots and logical payload bytes;
`PayloadPool` separately owns physical block and byte bounds. Dispatch,
completion-bookkeeping and Storage in-flight dimensions are introduced with
their owning M2 mechanisms instead of existing as unused M1 counters.

This keeps a single owner for each configured resource metric and prevents M1
from claiming enforcement for a mechanism it does not yet implement. All
dimensions added later must participate in the same all-or-nothing reservation
discipline.

**Reference:** `docs/design/rodaq_v1_system_design.md` M1/M2 scope,
`docs/design/admission.md`, `docs/development/plan.md`.

## D-037 — Uncertain Plugin progress uses process-lifetime activation quarantine

**Status:** Accepted

M2 executes potentially blocking Plugin lifecycle calls (`quiesce_plugin`,
`flush`, `close`, `stop`) on deadline-supervised workers. C++ cannot cancel a
thread executing Plugin code, so a timed-out call:

```text
returns Runtime Failed within the configured bound
retains the joinable worker object and captured activation
retains Plugin instances and DSO leases
retains all unsafe-to-release Payload and operation state
blocks Runtime restart
```

This is intentional process-lifetime quarantine, not ordinary shutdown.
Destroying an activation while Plugin code may still access it is never an
acceptable timeout response.

ASan/LSan ignores only the explicit quarantine roots. The M2 E2E CTest entry
disables TSan thread-leak reporting because fault tests intentionally retain
quarantined joinable threads; TSan data-race reporting remains enabled. Normal
leak detection and normal plugin symbol validation remain active.

**Reference:** `docs/design/runtime.md`, `docs/design/rodaq_plugin_system.md`,
`docs/design/acquisition_plugin.md`, `docs/development/status.md`.
