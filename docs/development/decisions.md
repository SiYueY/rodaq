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

Required production baseline:

- spdlog 1.17.0 — static, external fmt.
- fmt 12.1.0 — static.
- HdrHistogram_c 0.12.1 — core-only static.
- yaml-cpp 0.9.0 — static + PIC.
- tl::expected 1.3.1 — pinned Public C++ header dependency used privately by `rodaq::Result<T>`; see D-067.

Approved optional candidates:

- oneTBB 2022.3.0 — not linked into V1 Runtime Core by default; future explicit CPU-parallel work only.
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
rodaq.acquisition.control
rodaq.storage.control
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

`rodaq.runtime.common::get_lifecycle_snapshot()` is a bounded, thread-safe Runtime-facing query. A recovery attempt may start only from a snapshot reporting `RUNNING` and `normal_acquisition_allowed=true`.

The snapshot can become stale immediately; subsequent Runtime registration/report/submit calls remain authoritative. Plugins do not continuously poll it.

**Reference:** `docs/design/rodaq_plugin_system.md`, `docs/design/acquisition_plugin.md`.

---

## D-025 — RoDAQ V1 Result uses expected-lite directly

**Status:** Superseded by D-058; retained as historical context

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

The RoDAQ `make_error` forwarding function wraps an error for constructing a
failed `Result`, so callers do not depend on the third-party factory name.
The underlying `Result` type remains unchanged.

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
rodaq.projection.ros2
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

**Status:** Accepted; C++ SDK source names superseded by D-056.

M0 PASS freezes the Stable ABI core prefix, Service Registry, ConfigView,
Plugin Interface Query, C++ SDK authoring semantics, exception firewall,
metadata/CMake authoring pipeline and initial V1 compatibility fixtures. D-056
revises the C++ SDK source names to snake_case without changing the Stable C ABI.
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

M2 executes potentially blocking Plugin activation and lifecycle calls
(`create`, `bind`, `configure`, `open`, capability query, Descriptor readiness,
`start`, `quiesce_plugin`, `flush`, `close`, `stop`) on deadline-supervised
workers. C++ cannot cancel a thread executing Plugin code, so a timed-out call:

```text
returns Runtime Failed within the configured bound
transfers the joinable worker to a bounded process-lifetime quarantine owner
retains the captured activation
retains Plugin instances and DSO leases
retains all unsafe-to-release Payload and operation state
blocks Runtime restart
```

Retained calls own or hold shared ownership of every input view and activation
object they can still access after the supervising Runtime call returns.
Completed retained workers are joined and removed at later safe lifecycle
points. The owner has fixed internal call and activation-root capacities;
exhaustion rejects a new unsafe activation.

This is intentional process-lifetime quarantine, not ordinary shutdown.
Destroying an activation while Plugin code may still access it is never an
acceptable timeout response.

ASan/LSan ignores only the process-lifetime quarantine owner and reachable
unsafe roots. Thread-leak reporting and race reporting remain enabled. The
previous M2 thread-leak exception is superseded by Final Closure Fix: one
process maintenance worker claims completed entries, joins outside its mutex,
and only then releases captures/capacity. It also resumes safe activation
cleanup continuations; unsafe barriers remain retained. Its own thread is
joined during process teardown.

**Reference:** `docs/design/runtime.md`, `docs/design/rodaq_plugin_system.md`,
`docs/design/acquisition_plugin.md`, `docs/development/status.md`.

## D-038 — Descriptor publication precedes retryable Storage readiness

**Status:** Accepted

Canonical Descriptor publication and Storage readiness are separate lifecycle
steps. `PublishStreamDescriptor` commits and returns the immutable
`DescriptorHandle` first. Acquisition may then register a typed
InteropProjection through the existing Runtime projection service.

Storage readiness evaluates the immutable Descriptor plus its current immutable
projection set. `UNSUPPORTED` caused by missing projection leaves the Descriptor
published but non-routable and is retryable; successful projection registration
notifies Runtime after the Projection Registry lock is released and triggers
reconciliation again.

Descriptor readiness and Acquisition prepare/commit/abort execute outside
Runtime structural locks. Runtime snapshots under lock, performs Plugin I/O
without the lock, then reacquires the lock to validate Descriptor/Binding
generation before publication.

Each activation owns one Descriptor reconciliation worker with a fixed request
capacity equal to `max_stream_descriptors` and per-handle deduplication.
`PublishStreamDescriptor` only validates, publishes immutable state, enqueues
reconciliation and returns the handle. Initial requests are critical to
Activation Commit; a running activation may return from dynamic publication
before reconciliation completes. Stop closes the request entry and waits for
the current bounded Plugin calls to converge, or quarantines the activation.

**Reference:** `docs/design/runtime.md`,
`docs/design/acquisition_plugin.md`, `docs/design/storage_plugin.md`.

## D-039 — Activation roots have one fixed process-wide budget

**Status:** Accepted

A process may retain at most 32 activation roots at once. The count includes
healthy activation slots as well as roots retained for uncertain Plugin
progress. Every `DAQ::start()` reserves one root slot before activation;
exhaustion returns `ResourceExhausted` without entering `Running`.

This capacity is reserved in full at Start so every running activation has a
safe quarantine destination if one of its Plugin calls later fails to make
progress. It is a safety budget, not a performance target, and does not add a
Public ABI field or configurable property.

**Reference:** `docs/design/runtime.md`, `src/quarantine.hpp`,
`docs/development/status.md`.

## D-040 — Storage handoff timeouts are scoped to one Submit generation

**Status:** Accepted

The Storage dispatch lane assigns a monotonically increasing generation to every
`Storage::submit()` call. The Watchdog captures the active generation and its
deadline, and may report Timeout only if that same generation is still active
at the deadline. A completed Submit followed immediately by another Submit
therefore cannot inherit the previous call's timeout.

**Reference:** `src/storage_pipeline.cpp`,
`docs/design/storage_plugin.md`, `docs/development/status.md`.

## D-041 — Insert-only Registry slots use acquire/release publication

**Status:** Accepted

Registry insertion fully constructs a value before release-publishing an atomic
slot flag. `Find()` and `FindStable()` acquire-observation of that flag before
accessing the immutable value or generation. This makes concurrent lookup of a
published slot safe while another preallocated slot is being inserted and makes
an unpublished or fabricated handle resolve to null.

This does not authorize concurrent `Erase()`; erasure and slot reuse retain the
existing external-serialization and reader-lifetime requirements.

**Reference:** `src/registry.hpp`,
`docs/design/runtime.md`, `docs/development/status.md`.

## D-042 — Joint telemetry has two explicit benchmark record granularities

**Status:** Accepted

The V1 joint benchmark baseline covers 100 joints with 100 binary64 parameters
per joint at 1 kHz. It distinguishes production and stress groupings:

```text
production qualification:
    aggregated RobotState Record
    1,000 records/s
    80,000 B/record

small-record stress:
    per-joint Record
    100,000 records/s
    at least 800 B/record
```

Parameter-level Records are not a V1 standard mapping. The benchmark must state
which grouping it uses because record-count and mixed-resource behavior differ
by two orders of magnitude. If a production source natively publishes one
message per joint, the per-joint grouping becomes a production profile and must
pass the full production gate rather than only the stress gate.

**Reference:** `docs/design/rodaq_v1_system_design.md`,
`benchmarks/joint_pipeline_benchmark.cpp`, `docs/development/status.md`.

## D-043 — Standard camera + joint workload is an early V1 gate

**Status:** Accepted

The V1 reference workload is:

```text
10 cameras × 1280×720 × 30 FPS
+
100 joints × 100 parameters per joint × 1 kHz
```

Fake Runtime capacity qualification belongs to M2. Real ROS2/FastDDS + MCAP and
filesystem qualification belongs to M3/M4. Long-duration soak and release
hardware qualification remain in M5.

A standard profile passes only when the offered workload is accepted without
steady-state Admission WaitTimeout/rejection, bounded queues do not grow
without limit, Accepted Records reach terminal completion, and Stop converges.
Merely recording a lower throughput number is not a PASS.

**Reference:** `docs/design/rodaq_v1_system_design.md`, `docs/design/runtime.md`,
`docs/development/plan.md`.

## D-044 — V1 Copy PayloadPool uses isolated size classes

**Status:** Accepted

V1 Runtime Copy uses activation-owned, fixed-capacity PayloadPool size classes.
Each class has independent block count and physical-byte capacity. A Payload is
assigned to the smallest accommodating class before Gate Pass; class exhaustion
fails Admission rather than borrowing unbounded heap or silently consuming an
unrelated class.

Logical Payload-byte accounting, class object occupancy and physical reserved
bytes are all observable. This is required so raw camera frames and high-rate
small joint Records can coexist without one workload exhausting the other's
physical resource pool.

**Reference:** `docs/design/payload.md`, `docs/design/runtime.md`,
`docs/design/admission.md`, `docs/development/status.md`.

## D-045 — Plugin-origin cleanup is supervised and quarantinable

**Status:** Accepted

Every external Adopt/Retain Payload anchor carries the owning Acquisition
instance lifetime lease until its exactly-once release callback returns.
Accepted operations have release-work ownership reserved before Gate Pass.
Plugin release callbacks run on bounded Release Executor workers.

If a release worker cannot make progress before the Runtime Stop deadline, its
joinable thread and worker state are transferred to the process-wide
`PluginQuarantine`; the activation root, Plugin instance/Dso leases and
Payload anchor remain retained. The worker is never detached and `dlclose` is
never attempted while it may still execute.

Plugin `destroy()` is explicit and invoked through the same deadline-supervised
call path. A timed-out destroy remains owned by its retained supervised call
and activation root. Implicit destroy from a normal destructor is not part of
the production lifecycle.

**Reference:** `docs/design/payload.md`,
`docs/design/rodaq_plugin_system.md`, `docs/design/runtime.md`,
`tests/integration/runtime_pipeline_test.cpp`.

## D-046 — V1 ABI semantic firewall is fail-closed

**Status:** Accepted

ABI validation order is:

```text
scalar/count limits
    -> pointer/count structural relationships
    -> element dereference
    -> enum/reserved/presence semantics
```

Timestamp arrays are bounded before iteration: three Acquisition transport
timestamps and four canonical slots including Runtime ingress. APPENDABLE
outputs use only the caller-provided initialized prefix. Storage submit
outputs validate payload/persistence enums, reserved fields, diagnostics and
success/failure relationships.

Plugin-origin diagnostic buffers validate status, flags, reserved words,
capacity bounds and UTF-8 before Runtime reads them. Malformed diagnostics are
an ABI violation or rejected input, never an indefinite or partially trusted
value.

**Reference:** `docs/design/canonical_profile.md`,
`docs/design/rodaq_plugin_system.md`, `docs/design/storage_plugin.md`,
`docs/development/review_report.md` A-02/A-10.

## D-047 — Acquisition health is operational state

**Status:** Accepted

`AVAILABLE`, `RECOVERABLE_FAILURE`, and `TERMINAL_FAILURE` are Runtime
operational states, not ordinary diagnostics.

Submission eligibility is centralized as:

```text
RuntimeRunning
&& GateOpen
&& PluginHealth == Available
&& SourceEligibleIfPresent
&& StreamAvailable
&& BindingCurrent
&& BindingLifecycle == Active
&& SequenceCanAllocate
```

A recoverable failure invalidates current routing and requires new Descriptor
and Binding activation before normal submission resumes. Availability does not
reactivate stale Bindings. Both failure kinds also mark owned Sources Unavailable;
health recovery leaves that state unchanged until the owner explicitly reports
Source recovery. Terminal failure synthesizes an error when no
diagnostic is supplied and ends that Plugin's participation in the activation.

**Reference:** `docs/design/acquisition_plugin.md`,
`docs/design/runtime.md`, `tests/integration/runtime_pipeline_test.cpp`.

## D-048 — Dispatch, Payload, release and Persistence axes are orthogonal

**Status:** Accepted

Dispatch and operation state do not encode Payload or Persistence progress.
Payload consumption, Plugin-origin release work, and Persistence each have an
independent state dimension. An operation may have Payload Released while
Persistence remains Pending. An operation slot is not returned until all three
required axes are terminal.

Normal slot return uses a state/generation transition and does not scan a
free list. Duplicate or late terminal callbacks fail closed.

**Reference:** `docs/design/runtime.md`,
`docs/design/storage_plugin.md`, `src/storage_pipeline.*`.

## D-049 — Copy Payload uses pre-Gate size-class reservation

**Status:** Accepted

Runtime selects the smallest accommodating configured Payload size class
before Gate Pass. It reserves the physical block, logical Payload bytes,
dispatch/operation/completion capacity and release ownership as one
all-or-nothing Pre-Gate set. Copy materialization completes before final
eligibility revalidation and Gate Pass.

Classes have independent object counts and physical bytes. Exhaustion of one
class neither borrows from another class nor falls back to ordinary heap
allocation.

**Reference:** `docs/design/payload.md`,
`docs/design/admission.md`, `docs/design/runtime.md`,
`tests/payload/payload_pool_test.cpp`.

## D-050 — DAQ Application API is an installable Candidate

**Status:** Accepted

`DAQConfig`, `ResourceConfig`, `AcquisitionPluginConfig`, `StoragePluginConfig`,
`DAQ`, Runtime state/failure/stats, Plugin topology and Payload statistics
form the M2 DAQ Application API Candidate.

The Candidate is installed and validated by an external CMake consumer. It is
not frozen for arbitrary compiler/STL compatibility and may evolve through
M3/M4 backend validation. The Stable C Plugin ABI remains independently
frozen.

**Reference:** `src/`, `cmake/install.cmake`,
`tests/abi/install/daq_consumer/`.

## D-051 — Logging is Runtime-owned, bounded and quarantinable

**Status:** Superseded by D-068; retained as historical context.

Runtime owns a `LoggingContext` with fixed record and byte capacity,
drop-new policy, high-water/drop/backend-failure counters, direct recursion
detection, bounded flush, bounded shutdown, and Plugin log/diagnostic routing.
spdlog and fmt are private implementation dependencies and do not appear in
Public API or Plugin ABI.

A backend exception or timeout is contained. A worker that cannot stop by the
deadline is transferred to the process-wide worker quarantine rather than
detached or destroyed while executing backend code.

**Reference:** `docs/design/logging.md`,
`src/logging.hpp`, `src/logging_context.*`,
`src/spdlog_backend.*`.

## D-052 — Fake Runtime capacity and production storage are separate gates

**Status:** Accepted

M2 qualification proves bounded Fake Runtime capacity for camera, joint and
mixed workloads. It does not prove MCAP serialization, compression, filesystem
durability, NVMe sustained write, crash recovery, or long-duration production
behavior. Those remain M3/M4/M5 evidence.

A benchmark passes a capacity profile only when the offered workload is
accepted, terminal operation occupancy returns to zero, HWM remains bounded,
no steady-state WaitTimeout/rejection is introduced by capacity, ordering is
preserved, and Stop converges.

**Reference:** `docs/design/rodaq_v1_system_design.md`,
`docs/design/runtime.md`, `benchmarks/`.

## D-053 — Application-facing Runtime ships as a shared library

**Status:** Accepted

The production DAQ target is `librodaq.so` with hidden default
visibility. Only the DAQ Application API Candidate uses export
attributes. Private logging, config parser, Plugin loader, histogram and
storage-pipeline implementation symbols stay behind the DAQ DSO.

Normal Runtime builds require only the production dependencies. Test and
benchmark dependencies remain conditional. Static Runtime delivery is not a
V1 convergence deliverable because it requires a separate fully merged
transitive static-link closure.

**Reference:** `src/CMakeLists.txt`, `cmake/install.cmake`,
`cmake/dependencies.cmake`, `cmake/dependency_inventory.json.in`.

## D-054 — Fatal failure automatically converges through one lifecycle owner

**Status:** Accepted

RequestFailure performs bounded failure recording/cutoff and coalesced shared
mailbox notification, with no Plugin I/O and no retained Runtime pointer. One
Runtime supervisor shares Stop's cleanup implementation and logical ownership;
Starting uses activation abort, Running starts automatic failure cleanup, and
a failure during Stop belongs to its existing owner. Plugin calls and Join run
outside the lifecycle mutex. Repeated Stop cannot duplicate cleanup.

Descriptor FAILED, readiness timeout and malformed output are Runtime-wide
fatal Storage failure in Starting and Running. Unsupported remains local and
Projection-retryable. Failure cutoff classifies never-handed-off Accepted work
as PersistenceFailed; uncertain handoff keeps Unknown disposition and Payload
ownership. A returned successful close proves access quiescence, not success
of unconfirmed persistence. Retained cleanup uses the same continuation and
recorded control-call result, never repeats a timed-out destructive call.

**Reference:** `docs/design/runtime.md §11.6.2–§11.6.3`,
`docs/design/storage_plugin.md §3.3.3`, D-037, D-039, D-045.

## D-055 — Quarantine publishes retained threads into fixed storage

**Status:** Accepted

Supervised calls and retired workers use fixed 64-slot arrays and share a
64-entry budget, including calls in progress and entries being Joined. Activation
roots retain their independent 32-slot budget. Run prepares entry/completion
storage and reserves capacity before starting external code; timeout publication
cannot allocate. RetainWorker checks capacity before moving the caller's thread,
then constructs the entry using non-throwing moves. Failed retention leaves the
joinable thread with its caller. Join and captured-state destruction remain
outside the manager mutex, and capacity returns only after both complete.

This is a guarantee about retention publication, not arbitrary global heap
exhaustion recovery. No new production dependency or public interface is needed.
The allocation regression executable uses GNU linker wrapping so sanitizer
new/delete interceptors remain active.

**Reference:** `docs/design/runtime.md §11.6.4`,
`tests/plugin/plugin_quarantine_allocation_test.cpp`, D-054.

## D-056 — C++ authoring interfaces use snake_case

**Status:** Accepted; supersedes the C++ SDK source-name freeze in D-033.

Project-owned C++ functions, including DAQ Application and Plugin SDK
interfaces and virtual authoring hooks, use snake_case. The SDK's previous
PascalCase names and the DAQ Application Candidate's old C++ symbols are
removed rather than aliased; C++ applications and Plugin source must migrate
and rebuild. This user-approved source-level revision changes neither the Stable
C Plugin ABI nor its table layouts, exported C entry points, semantics or V1
compatibility fixtures. Prebuilt Stable-C-ABI Plugin binaries remain compatible.

The GNU linker `__real__`/`__wrap__` hooks, external overrides, C++ operators,
generated code and frozen ABI fixtures retain externally mandated names.
Clang-tidy 14 and clang-format 14 check project code in CI without adding a
production dependency or a network fetch to normal builds.

**Reference:** `docs/coding_guidelines/cpp_coding_guidelines.md`, D-033, D-050.


## D-057 — Unify C++ and ABI status terminology

**Status:** Superseded by D-058; retained as historical context; supersedes the `ErrorCode` spelling in D-027 and D-035.

C++ `StatusCode` includes `Ok` and matches the Stable C ABI status values.
`Error` represents failure only: both constructors normalize `Ok` and unknown
values to `InternalError` while preserving diagnostic domain, native code and
message. The C++ accessor is `status_code()`. Explicit mappings at the ABI
boundary preserve ABI success and malformed-diagnostic behavior. C++ sources
must migrate and rebuild; the Stable C ABI and frozen fixtures remain unchanged.

**Reference:** `docs/design/rodaq_plugin_system.md`, D-025, D-027, D-035, D-056.


## D-058 — Status and validated Result unify operation outcomes

**Status:** Superseded by D-067; retained as historical context. It supersedes D-025's direct alias, D-057's failure-only Error,
and the earlier C/C++ source-name and diagnostic contracts where they conflict.

No-value operations return Result<void>; value operations return Result<T>. Status
has ok(), status_code(), domain(), error_code() and message(). Default Status
is OK with no diagnostics. Unknown codes normalize to UnknownError. Result is a
RoDAQ class privately backed by pinned expected-lite 0.10.0, not a public alias.
Its failure state is immutable through its public interface; attempting to create
a failed Result from OK produces UnknownError. Result<Status> is a compile-time
error. Result<void> privately stores nonstd::expected<void, Status> and defaults
to success; constructing it from OK Status is misuse and produces UnknownError.
Void results cannot implicitly become values of non-void results through their
boolean conversion; propagate failure via status(). value() requires ok();
lvalue value references follow the result's lifetime. A default non-void Result is an UnknownError, not a default T.
Runtime primary_failure() returns Status; OK means no primary failure. Failure
latches and deferred diagnostics also use Status directly. Completion flags,
not diagnostic presence, establish asynchronous call completion.

C uses rodaq_status_code for integers and rodaq_status for caller-owned bounded
diagnostics. The latter has status_code, error_code and bounded domain/message
storage. UnknownError is 10 on both sides. Source compatibility aliases and the
old Error/make_error/expected-style result accessors are removed. Applications
and plugins must migrate and rebuild; no legacy binary compatibility is promised.
Independent current ABI fixture definitions and layout tests replace the old
compatibility-source suite; Git history retains historical fixtures and results.

This changes no Acquisition admission/ownership, Payload/Persistence axes,
ordering, boundedness, stop/quarantine or backend scope contract. M3 is not started.

## D-059 — Default C++ namespace is `rodaq`

**Status:** Accepted.

Public and internal domain types default to namespace `rodaq`. The Plugin
subsystem may use `rodaq::plugin` because that sub-namespace is a long-lived
semantic boundary. RoDAQ does not use generalized `detail`, `internal` or `impl`
namespaces. `DAQ::Impl` remains valid because it is the Pimpl type bound directly
to `DAQ`, not a namespace or compatibility alias.

Public/private boundaries are enforced by installed headers, CMake target
ownership and symbol visibility rather than namespace naming. Callers may not
include `src/` headers as a substitute for a public API.

**Reference:** `include/rodaq/`, `src/`, `src/daq/daq_impl.hpp`, `src/plugin/`.

## D-060 — ABI source names are version-free

**Status:** Accepted; supersedes the version-suffixed ABI source names in
D-033/D-045 where they conflict.

C ABI type names, macros, service/interface identifiers and source files do not
contain `_v1`, `.v1` or equivalent version suffixes. Protocol versions remain
explicit runtime data through `protocol_version` and minimum-prefix checks.

The source cleanup preserves existing C structure layouts, field offsets,
alignment, scalar values, enum values, callback signatures and ownership
semantics. ABI layout, minimum-prefix, scalar and symbol qualification was
rerun after the rename.

**Reference:** `include/rodaq/plugin/abi/`, `tests/abi/`.

## D-061 — CMake ownership separates DAQ, SDK, and Plugin Host

**Status:** Accepted.

The installed application target is `rodaq::daq` with file name
`librodaq.so`. The installable Plugin SDK target is the static `rodaq::sdk`,
which owns SDK wrappers, validation implementations, Config model access, and
the common Status/ABI support required by Plugin authors.

`rodaq_plugin`, `rodaq_objects` and `rodaq_testing` are internal. Plugin Host
implementation is not installed as an independent product dependency. DAQ and
Plugin Host targets never depend back on the application DAQ target. The SDK
does not depend on YAML parsing, the DAQ DSO, Plugin Host, fmt, or spdlog.

Installed `RoDAQTargets.cmake` exposes only `rodaq::daq` and `rodaq::sdk`.
Third-party and implementation-support targets are not exported. Public
out-of-line symbols are explicit in `cmake/rodaq.map`; internal implementation
symbols remain hidden.

**Reference:** `src/CMakeLists.txt`, `src/plugin/CMakeLists.txt`,
`src/daq/CMakeLists.txt`, `cmake/install.cmake`, `cmake/rodaq.map`.

## D-062 — DAQ lifecycle and Application config are not ABI DTOs

**Status:** Accepted.

The DAQ application C++ API returns C++ lifecycle, stats and config DTOs. The
Stable C ABI lifecycle/config views are adapted inside implementation units and
the Plugin SDK. `ConfigNodeRef` exposes RoDAQ `ConfigKind`; conversion to
`rodaq_config_view` is the `config/abi_bridge.hpp` adapter used by DAQ and
Plugin Host.

The frozen Storage ABI continues to use `session_id`; the DAQ application config
field is `acquisition_id`, mapped explicitly when Storage Open is called.

**Reference:** `include/rodaq/daq.hpp`, `include/rodaq/config/config.hpp`,
`src/config/abi_bridge.hpp`, D-060.

## D-063 — View, Ref and Info have distinct long-term semantics

**Status:** Accepted.

`View` means borrowed, non-owning data or a proxy over existing data, such as
`PayloadView`, `ConfigView`, `rodaq_string_view` and `rodaq_bytes_view`.

`Ref` means a reference-like handle to an existing object or capability whose
lifetime may be intentionally extended, such as `InterfaceRef`,
`ServiceRegistryRef` and `ConfigNodeRef`.

`Info` means caller-owned metadata output with borrowed fields, such as
`rodaq_source_info`, `rodaq_clock_info`, `rodaq_schema_info`,
`rodaq_stream_info`, `rodaq_stream_descriptor_info`,
`rodaq_timestamp_key_set_info` and `rodaq_projection_info`.

No compatibility alias is provided for the former `*View`/`*_view` source
names. `ConfigTree::root()` replaces `ConfigTree::view()`.

**Reference:** `docs/coding_guidelines/cpp_coding_guidelines.md`,
`include/rodaq/plugin/`, `include/rodaq/plugin/abi/`.

## D-064 — Activation composes domain state owners

**Status:** Accepted.

`ActivationState` is the activation lifecycle aggregate root. It composes:

```text
CanonicalState
BindingState
AdmissionState
ActivationMetrics
```

`CanonicalState` owns Canonical registries, handles, ID allocators, registration
interning, descriptor metadata lookup and availability state. `BindingState`
owns Binding records, current Binding publication, lifecycle transitions and
in-flight admission counts. `AdmissionState` owns the Gate, submission and
admission in-flight accounting, eligibility generation and AcceptanceSequence
allocation. `ActivationMetrics` owns counters and latency histograms.

Activation retains lifecycle, failure cutoff, shutdown ordering, quarantine
handoff, Descriptor reconciliation orchestration and cross-domain admission
submission. Domain invariants are not delegated to a new Manager or Controller
layer.

**Reference:** `src/activation/`, `src/canonical/`, `src/binding/`,
`src/admission/`, `src/metrics/`.

## D-065 — Activation phase and effective limits are internal state

**Status:** Accepted.

Activation uses `ActivationPhase` (`Starting`, `Running`, `Stopping`, `Stopped`,
`Failed`) rather than `DAQState` as its internal state machine. The DAQ
application object maps its own public lifecycle state to
`DAQLifecycleSnapshot`.

`DAQConfig` is immutable for the activation lifetime. Resource values negotiated
with Storage capabilities, including timeouts and capacities, are held in
`ActivationLimits`; effective values never mutate the original configuration
DTO.

**Reference:** `src/activation/activation_phase.hpp`,
`src/activation/activation_limits.hpp`, `src/daq/daq_impl.hpp`.

## D-066 — Verification cadence is incremental until code freeze

**Status:** Accepted.

During refactoring, run only the affected compile targets and domain/integration
tests. Do not rerun the full sanitizer, cross-platform, style or benchmark
matrix after each structural edit.

Run the complete Qualification matrix once after the implementation and tests
are stable, and rerun only affected checks for later non-behavioral edits.
This preserves correctness evidence without making the full matrix the normal
edit loop.

**Reference:** `docs/coding_guidelines/cpp_coding_guidelines.md §5.42`,
`docs/development/plan.md`.

## D-067 — Result uses tl::expected with strong contract semantics

**Status:** Superseded by D-070; retained as historical context. It supersedes
D-025 and D-058.

`rodaq::Result<T>` privately stores `tl::expected<T, Status>` from pinned
tl::expected 1.3.1 at commit
`1770e3559f2f6ea4a5fb4f577ad22aeb30fbd8e4`. The installed include tree supplies
`tl/expected.hpp` and its license; expected-lite is removed.

A default `Status` is success. Constructing `Status(Ok, ...)`, constructing a
failed `Result` from an OK Status, or reading `value()`, `operator*` or lvalue
`operator->()` from a failed Result is a contract violation and calls
`std::abort()`. Non-void Result has no default constructor, no implicit boolean
conversion, and rejects array, reference, cv-qualified and `Status` value types.
`Result<void>` defaults to success and its `value()` aborts on failure. Rvalue
`Status::message()` and rvalue `Result::operator->()` are deleted to prevent
dangling access. Unknown status values normalize to `UnknownError`.

**Reference:** `include/rodaq/status.hpp`, `include/rodaq/result.hpp`,
`tests/daq/`, `tests/abi/compile_fail/`.

## D-068 — Logging is Activation-owned with weak public handles

**Status:** Accepted; supersedes D-051.

Each activation owns its `LoggingContext`, sink collection, bounded queue and
single worker. There is no process-global production logger.
`DAQ::logger(category)` succeeds only while the DAQ is Running. The returned
`Logger` owns its category but observes the activation state through a weak
reference. After Stop, logging calls are safe no-ops and control operations
return `InvalidState`; restart requires obtaining a new Logger.

The queue is bounded by both record count and materialized category/message
bytes; source file/function strings are also copied into the record and charged
to that byte budget. Sync, AsyncBounded, Console/File/RotatingFile/DailyFile
sinks, sink-level
patterns and levels, Block/OverrunOldest/DiscardNew overflow, periodic and
level-triggered flush, backtrace, source location, formatting exception
isolation, reentrancy drop, statistics and bounded quarantine are required.
Default async capacity is 8192 records and 8 MiB with `DiscardNew`.

The public Logging header intentionally uses pinned fmt/spdlog for header-only
formatting and macros. `rodaq::daq` therefore exports `fmt::fmt` and
`spdlog::spdlog` usage requirements, and `RoDAQConfig.cmake` resolves exact
pinned package versions with `find_dependency()`. No third-party type enters a
RoDAQ-owned function signature or Stable C ABI. `rodaq::sdk` remains independent
of DAQ, fmt, spdlog and YAML.

**Reference:** `docs/design/logging.md`, `include/rodaq/logging.hpp`,
`include/rodaq/logging_config.hpp`, `src/logging/`, `cmake/install.cmake`.

## D-069 — Public logging macros and ABI severity macros are distinct

**Status:** Accepted; clarifies D-068 and replaces conflicting historical
source-name statements in D-051.

Public logging macros are `RODAQ_LOG_TRACE`, `RODAQ_LOG_DEBUG`,
`RODAQ_LOG_INFO`, `RODAQ_LOG_WARN`, `RODAQ_LOG_ERROR` and
`RODAQ_LOG_CRITICAL`. `RODAQ_LOG_ACTIVE_LEVEL` controls compile-time pruning and
disabled macros do not evaluate arguments. `RODAQ_LOG_LEVEL_*` constants are the
compile-time thresholds.

Stable ABI severity constants are `RODAQ_ABI_LOG_*` with numeric values 1..7
unchanged. Plugin SDK and adapter code map `LogLevel` and ABI severity with an
explicit mapping. No old `RODAQ_LOG_*` ABI-name aliases are retained; C and C++
Plugin consumers must rebuild.

**Reference:** `include/rodaq/logging.hpp`,
`include/rodaq/plugin/abi/runtime.h`, `src/plugin/common_service_adapter.cpp`,
`tests/integration/`.

## D-070 — Status/Result align with rocomm

**Status:** Accepted; supersedes D-067.

`rodaq::Result<T>` is adapted from the rocomm source implementation. It keeps
the same construction, access, abort and move-only semantics as rocomm, fixes
the error type to RoDAQ `Status`, and retains the tl::expected 1.3.1 header
version check. RoDAQ does not depend on the rocomm project or library.

`rodaq::Status` is a header-only rocomm-style value type extended with
`domain_` and native `error_code_`. Its public names are `Status::Ok()`,
`ok()`, `code()`, `domain()`, `error_code()` and lvalue `message()`.
`StatusCode` uses `int32_t` and retains `UnknownError = 10`; constructors do
not normalize arbitrary C++ status values. `Status(Ok, ...)` remains a
contract violation that aborts.

Stable C ABI values and layouts remain unchanged. The ABI bridge explicitly
maps known `rodaq_status_code` values, and unrecognized ABI values are
normalized to `RODAQ_STATUS_UNKNOWN_ERROR`. Status is no longer exported from
`librodaq.so`; consumers compile its inline implementation from the installed
header. No old `success()` or `status_code()` aliases are retained.

**Reference:** `include/rodaq/status.hpp`, `include/rodaq/result.hpp`,
`src/plugin/abi_support.cpp`, `tests/daq/`, `tests/plugin/service_registry_test.cpp`.
