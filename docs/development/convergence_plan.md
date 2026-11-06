# RoDAQ V1 Design & M0-M2 Implementation Convergence Plan

**Status:** Implemented; M2 Final Closure Fix locally qualified on 2026-11-05

**Plan revision:** R1 (executed)

**Implementation baseline:** `develop@3f0f1392eebf21c1e4917e14a1b569e63df87b49`

**Audit date:** 2026-11-04

This plan is the implementation input for the V1 Runtime Foundation
Convergence. It does not itself change code, tests, CMake, Stable ABI, or
milestone status.

## 2026-11-06 Pre-M3 Core Structure Closure

The user-authorized Core Structure Closure has been implemented on top of the
converged M2 foundation:

- `CanonicalState`, `BindingState`, `AdmissionState` and `ActivationMetrics`
  now own their corresponding state and invariants; `ActivationState` retains
  lifecycle and cross-domain orchestration.
- Activation uses internal `ActivationPhase` and immutable `DAQConfig` plus
  negotiated `ActivationLimits`.
- Plugin service adapters are split by protocol; the thin activation Logging
  wrapper is removed; configuration validation is owned by `config/validation`.
- View/Ref/Info naming and Canonical `*_info` C ABI source names are closed
  without compatibility aliases.
- Installed CMake exports only `rodaq::daq` and `rodaq::sdk`; the SDK is
  independent of DAQ, YAML, Plugin Host, fmt and spdlog.
- A combined DAQ+SDK consumer, complete public symbol allowlist and automated
  naming/path residue check are present.

Local qualification is recorded in `status.md` and
`capacity_qualification.md`: Debug, Release, ASan/LSan and UBSan are
**105/105 PASS**; Ubuntu 24.04/GCC 13 is **105/105 PASS**; Clang 14 TSan
selected suites are **16/16 PASS**; style and all eight Release benchmarks pass.
Hosted CI remains PENDING. The core structure is locally FROZEN; M3 remains
NOT STARTED.

The plan incorporates the R1 review corrections:

- Copy Payload materialization occurs before Gate Pass.
- Every Accepted Adopt/Retain Payload owns pre-guaranteed release work.
- Release state remains orthogonal to Dispatch, Payload, and Persistence state.
- A stuck release worker is transferred to the existing process-wide
  quarantine owner.
- Release executor parallelism is evidence-driven, not frozen to one worker.
- M2 proves Fake Runtime capacity, while real storage production capacity
  remains an M5 qualification.
- Current occupancy returns to zero; high-water marks remain bounded and
  observable.
- Optional and development dependencies do not burden a normal Runtime build.
- Runtime delivery uses an explicit shared-library packaging decision.
- Runtime C++ API is an installable Candidate in M2 and freezes only before V1
  release.

---

## 1. Executive Summary

RoDAQ needs one coordinated convergence rather than another local M2 Closure
Fix. The work covers:

```text
Runtime lifecycle, ownership, and ABI correctness
Payload size classes and workload-driven capacity
Logging, configuration, DAQ Application API, and packaging
Dependency lifecycle, reference-driven implementation audit, and qualification
```

The intended milestone state is:

```text
M0 Stable C ABI contract
    PASS unchanged

M1 Runtime primitives
    PASS as originally scoped

M2 Fake End-to-End Runtime
    ACTIVE
    V1 Runtime Foundation Convergence

M3 ROS2 Acquisition + MCAP Storage
    NOT STARTED

M4 FastDDS Acquisition + MCAP Interoperability
    NOT STARTED

M5 Production Hardening + Release Qualification
    NOT STARTED

V1
    NOT QUALIFIED
```

No Stable C ABI layout, protocol version, vtable, service ID, or frozen struct
layout may be changed by this work.

---

## 2. Authoritative Baseline

### 2.1 Code baseline

```text
Repository: SiYueY/rodaq
Branch: develop
Commit: 3f0f1392eebf21c1e4917e14a1b569e63df87b49
Message: fix: close M2 concurrency audit
```

The implementation phase must begin from a reviewed and committed successor of
this baseline. Final evidence must record the exact repository commit,
submodule commits, compiler versions, OS/kernel, CMake version, test commands,
and benchmark hardware.

### 2.2 Existing local work

The following local changes are audit inputs or prototypes, not completion
evidence:

- `benchmarks/joint_pipeline_benchmark.cpp`
- Fake Acquisition exclusive/rate-controlled producer mode
- draft decisions D-042 through D-044
- `docs/development/review_report.md`
- convergence edits in `status.md`, `plan.md`, and `decisions.md`

The current joint benchmark's fixed capacity is not final. Its capacities must
be derived from the approved workload and stall-tolerance contract before it
can qualify a capacity gate.

### 2.3 Authority hierarchy

```text
Explicit current user instruction
Accepted durable decision
rodaq_v1_system_design.md
Specialist normative design
C++ coding guidelines
plan.md / status.md
```

Plan and status describe execution state and cannot override normative design.

---

## 3. Confirmed Problems

| ID | Severity | Classification | Problem |
|---|---|---|---|
| A-01 | P0 | Implementation defect | A blocking Plugin Payload release can prevent Completion Worker shutdown and make `DAQ::Stop()` hang |
| A-02 | P1 | ABI implementation defect | Timestamp pointer/count validation may iterate before enforcing the V1 limit and has a 32-bit wraparound risk |
| A-03 | P1 | Design + implementation | Acquisition Plugin health states do not drive submission or recovery behavior |
| A-04 | P1 | Lifecycle defect | Plugin `destroy()` can block outside Runtime supervision |
| A-05 | P1 | Hot-path defect | Storage slot return uses an O(N) free-list scan |
| A-06 | P1 | Design + implementation | One-size PayloadPool cannot represent the standard mixed workload |
| A-07 | P1 | Missing foundation | Runtime LoggingContext and Plugin log/diagnostic routing are absent |
| A-08 | P1 | Delivery gap | DAQ Application API and DAQ target are not installable |
| A-09 | P2 | Semantics/latency gap | Eligibility invalidation does not promptly interrupt Admission wait |
| A-10 | P1 | ABI semantic firewall | Storage submit outputs are not fully validated |
| A-11 | P3 | Generation exhaustion | Handle generation can wrap after very long activation history |
| Q-01 | P1 | Observability gap | Queue/class latency/rejection/Plugin health metrics are incomplete |
| Q-02 | P1 | Qualification gap | Camera, joint, mixed, real backend, and storage-worker evidence is incomplete |
| D-01 | P1 | Design gap | Camera and joint production/stress profiles are not fully frozen |
| D-02 | P1 | Design gap | Capacity sizing is not a complete workload-driven contract |
| D-03 | P2 | Dependency positioning | oneTBB has no demonstrated V1 Runtime Core use |
| D-04 | P2 | Dependency positioning | mimalloc build availability is confused with actual allocator use |
| D-05 | P1 | Public API gap | DAQ Application API and install boundary are not frozen as a Candidate |
| D-06 | P1 | Concurrency design gap | Lock ownership and ordering are not formally documented |
| D-07 | P1 | Failure design gap | Error-to-disposition mapping is not frozen |
| D-08 | P2 | Engineering process gap | RoDAQ-owned modules lack source-level upstream reference evidence |

---

## 4. Design Amendments

Phase 1 must resolve the following durable decisions before implementation.

### D-045 - Plugin-origin cleanup is supervised

- A Payload anchor carries a `PluginInstanceLease`.
- Copy materialization occurs before Gate Pass.
- Adopt/Retain ownership transfer occurs only after the required Pre-Gate
  resources are reserved.
- Every Accepted operation has pre-guaranteed release ownership.
- Plugin release callbacks execute on a bounded Release Executor.
- A blocked release transfers its worker, anchor, leases, native owner, and
  activation root to the process-wide PluginQuarantine.
- `destroy()` is explicit and supervised; it is not an implicit destructor
  side effect.

### D-046 - V1 ABI semantic firewall

- Enforce scalar and count limits before dereferencing pointer/count arrays.
- Use checked size conversions and checked addition/multiplication.
- Validate caller-owned APPENDABLE outputs by returned `struct_size`, minimum
  prefix, reserved fields, enum domain, and semantic state combinations.
- Keep every frozen C ABI layout unchanged.

### D-047 - Acquisition Plugin health is explicit state

- `AVAILABLE`, `RECOVERABLE_FAILURE`, and `TERMINAL_FAILURE` are operational
  states, not ordinary diagnostics.
- All submission eligibility decisions use one shared predicate.
- Entering `RECOVERABLE_FAILURE` invalidates the Plugin's existing routing.
- `AVAILABLE` restores discovery/registration eligibility but does not
  automatically reactivate stale Bindings.
- `TERMINAL_FAILURE` requires a synthesized error when no diagnostic is
  supplied and ends the Plugin's participation in the activation.

### D-048 - Slot state is orthogonal

Dispatch, operation, Payload consumption, Payload release, and Persistence are
separate state dimensions. Release pending is not a DispatchState.

### D-049 - Copy uses isolated Payload size classes

- Select the smallest accommodating class before Gate Pass.
- Reserve the class block before Payload materialization.
- Class object and physical-byte budgets are independent.
- Classes do not silently borrow capacity from each other.
- No Gate-Pass-after allocation or unbounded heap fallback is allowed.

### D-050 - Runtime API Candidate

- M2 produces an installable DAQ Application API Candidate.
- M3/M4 validate that Candidate with real backends.
- V1 source-compatibility is frozen only before V1 release.
- The Stable C Plugin ABI remains independently frozen.

### D-051 - Bounded logging lifecycle

- Runtime owns a bounded LoggingContext.
- spdlog/fmt remain private implementation dependencies.
- Plugin logs and diagnostics route through Runtime.
- Queue, drop policy, flush, shutdown, recursion, and backend failure are
  explicitly bounded and observable.

### D-052 - Fake capacity and production qualification are distinct

- M2 qualifies Fake Runtime capacity.
- M3/M4 qualify real backend interoperability and initial baselines.
- M5 qualifies real storage hardware, filesystem, compression, chunking, flush,
  crash recovery, and long-duration production throughput.

### D-053 - Runtime application delivery is shared by default

- Build the application-facing Runtime as `librodaq.so`.
- Hidden visibility exposes only the DAQ Application API Candidate.
- Private logging/config/plugin-loader/histogram dependencies remain behind the
  DAQ DSO.
- The C++ Runtime ABI is not promised across arbitrary compiler or STL
  versions.
- Static Runtime delivery requires a separate later decision and a fully
  merged transitive static-link closure.

---

## 5. Phase 0 - Audit Baseline Freeze

### Tasks

1. Convert `review_report.md` into an evidence ledger with this schema:

```text
ID
severity
classification
design reference
source file/line
trigger
observed behavior
impact
owner phase
acceptance test
```

2. Preserve the current staged prototype work without treating it as PASS.
3. Inventory every ABI pointer/count/size input and APPENDABLE output.
4. Inventory all Runtime and Plugin locks, their owners, and nested acquisitions.
5. Inventory all hot-path allocations and callback execution sites.
6. Record the baseline build and test evidence.
7. Produce a new committed audit baseline after independent review.

### Acceptance

- Every P0/P1 has a design and code reference.
- No finding uses a non-existent `PASS with follow-up` status.
- No local prototype is counted as qualified.

---

## 6. Phase 1 - Design Convergence

### Documents to update

```text
docs/design/rodaq_v1_system_design.md
docs/design/runtime.md
docs/design/admission.md
docs/design/payload.md
docs/design/logging.md
docs/design/rodaq_plugin_system.md
docs/design/acquisition_plugin.md
docs/design/storage_plugin.md
docs/design/canonical_profile.md
docs/development/decisions.md
```

### Design outputs

- Admission ordering and materialization contract.
- Pre-Gate resource set, including release-work ownership.
- Complete Plugin release/destroy/quarantine state machine.
- Plugin health and unified submission eligibility predicate.
- ABI semantic firewall matrix.
- Payload size-class and capacity formulas.
- Lock ownership and lock-order table.
- Failure-to-disposition matrix.
- Logging lifecycle and shutdown ordering.
- Config trust boundary and validation limits.
- DAQ Application API Candidate.
- Shared-library packaging decision.

### Reference audit before implementation

Study and record upstream source-level references for ResourceBudget/Admission,
PayloadPool, slot state, lifecycle/quiescence, Plugin loading, logging, and
configuration. Records go to `docs/development/reference_audit.md`.

### Acceptance

The design answers every implementation question needed by a later Codex
session. No code implementation starts while a P0/P1 design ambiguity remains.

---

## 7. Phase 2 - Lifecycle and ABI Correctness

### 7.1 Admission ordering

The required Copy sequence is:

```text
submit called
    -> structural validation
    -> capture ingress timestamp
    -> pre-Gate reserve:
           dispatch capacity
           operation slot
           completion capacity
           logical Payload-byte budget
           selected size-class block
           release-work ownership
    -> copy Payload into reserved block
    -> final eligibility revalidation
    -> per-Stream commit
    -> Gate Pass
    -> inflight accounting and AcceptanceSequence
    -> allocation-free ordered publication
```

For Adopt/Retain, validation and ownership acquisition occur after their
Pre-Gate budgets are reserved and before Gate Pass. A final revalidation
failure releases Runtime-owned responsibility exactly once.

### 7.2 Pre-guaranteed release ownership

Every Accepted operation carries exactly one of:

```text
Copy block release
Plugin Adopt/Retain release work
No Plugin release required
```

The release work index ring and release task slots have capacity equal to
`operation_capacity`. An operation slot is not returned before its release
state is terminal. Consequently an Accepted operation cannot discover missing
release capacity after Gate Pass.

If release enqueue fails after Gate Pass, this is a Runtime invariant
violation, not an ordinary Admission rejection.

### 7.3 Orthogonal slot state

Implement separate dimensions:

```cpp
enum class DispatchState {
    Free,
    Reserved,
    Ready,
    InFlight,
};

enum class OperationState {
    Inactive,
    Active,
    Terminalizing,
};

enum class PayloadAxis {
    AwaitingReturn,
    Pending,
    Consumed,
};

enum class PayloadReleaseState {
    NotRequired,
    Required,
    Queued,
    Running,
    Released,
    Quarantined,
};

enum class PersistenceAxis {
    AwaitingReturn,
    Pending,
    Succeeded,
    Failed,
    Unknown,
};
```

`PayloadReleaseState` may be running while `PersistenceAxis` is pending and the
operation remains active.

### 7.4 Release executor

Introduce a `PayloadReleaseExecutorPolicy` with configurable worker count.

Initial V1 development default may be one worker, but this is not a frozen
performance claim. Qualified profiles must measure Copy, Adopt, Retain, camera,
and 100k small-record pathways and select the minimum worker count that meets
the declared profile.

Plugin release contract:

```text
noexcept
short
non-blocking
no Runtime lock wait
no unbounded native shutdown
no Runtime re-entry
```

Runtime supervision is failure containment, not permission for Plugin code to
block.

### 7.5 Quarantine ownership transfer

On release timeout or invariant failure, move the complete worker dependency
bundle into the existing PluginQuarantine:

```text
std::thread
release worker state
current PayloadAnchor
PluginInstanceLease
PluginLibraryLease
native owner context
activation root reference
```

No detach, no destructive `dlclose`, no join under an expired deadline, and no
new unlimited release quarantine domain. The bundle counts against the existing
bounded process-wide quarantine capacities.

### 7.6 Supervised destroy

- Remove implicit external `destroy()` calls from destructors.
- Add explicit BeginDestroy/MarkDestroyed/Quarantine transitions.
- Invoke destroy only after Payload, callback, completion, and lease quiescence.
- A destroy timeout quarantines the instance, DSO, and activation.

### 7.7 Plugin health

Add per-Acquisition Plugin health state and one central submission eligibility
predicate:

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

Storage readiness is represented by Binding Active; do not add a duplicated
readiness boolean.

`RECOVERABLE_FAILURE` invalidates existing routing. `AVAILABLE` restores only
discovery/registration eligibility. New Descriptor publication and Binding
transition are required before normal submission resumes.

### 7.8 ABI semantic firewall

- Enforce timestamp count limits before pointer dereference.
- Acquisition transport timestamps are limited to three.
- Canonical timestamps including Runtime ingress are limited to four.
- Use checked conversions for all pointer/size and pointer/count pairs.
- Validate Storage submit output prefix, reserved fields, enum values,
  diagnostics, and axis combinations.
- Verify unknown tails are ignored or rejected according to frozen rules.

### 7.9 O(1) slot return

Replace the free-list linear search with state/generation transitions.
Terminalization must return an index exactly once, and cancellation is valid
only from an eligible reserved state.

### 7.10 Admission interruption

Source/Stream/Binding eligibility changes notify the same condition used by
pre-Gate resource wait. Notification occurs after struct locks are released.
Final revalidation remains authoritative.

### 7.11 Generation exhaustion

Refuse a new activation before a truncated handle generation can alias a stale
handle. Exhaustion is explicit and terminal for that Runtime instance.

### Tests

- Blocking release in a subprocess.
- Blocking destroy in a subprocess.
- `UINT32_MAX` timestamp counts.
- Null/count mismatches.
- Malformed Storage output matrix.
- Plugin health transition matrix.
- Recoverable failure followed by Descriptor/Binding re-establishment.
- Availability and Binding invalidation during resource wait.
- Double terminalization and slot return races.
- Release enqueue invariant.
- Quarantine ownership and no premature unload.

---

## 8. Phase 3 - Payload and Resource Model

### Configuration types

Introduce RoDAQ-owned configuration equivalent to:

```cpp
struct PayloadClassConfig {
    std::size_t max_payload_bytes;
    std::size_t block_count;
};

struct PayloadPoolConfig {
    std::vector<PayloadClassConfig> classes;
};
```

Validation requires strictly increasing class sizes, nonzero block counts,
checked multiplication, and a final class covering the effective maximum
Payload.

### Allocation algorithm

```text
payload size
    -> smallest accommodating class
    -> reserve class block
    -> reserve logical Payload bytes
    -> reserve dispatch/operation/completion/release ownership
    -> materialize Copy into the reserved block
    -> final revalidation
    -> Gate Pass
```

Adopt/Retain reserve logical bytes and release-work ownership but do not consume
a Copy class block.

### Statistics

Per class:

```text
configured blocks
current used blocks
current physical bytes
available blocks
object high-water
physical-byte high-water
exhaustion count
```

### Acceptance

- Large and small Payload coexistence.
- Independent class exhaustion.
- No cross-class automatic borrowing.
- Correct rollback before Gate Pass.
- All current occupancy returns to zero after Stop.
- High-water remains bounded and is retained for diagnostics.

---

## 9. Phase 4 - Logging, Config, and Runtime Delivery

### Logging

Add Runtime-owned LoggingContext with:

- fixed record and byte capacity;
- drop-new and drop accounting;
- one or more logging workers selected by configuration;
- flush/shutdown deadlines;
- recursion detection;
- backend failure containment;
- Plugin log/diagnostic routing;
- private spdlog/fmt backend.

spdlog/fmt types must not appear in Public API or Plugin ABI.

### Configuration

Add a RoDAQ-owned public ConfigTree/ConfigView model and YAML ConfigLoader.

Trust boundary:

```text
operator-controlled local configuration
```

Apply a pre-read file-size limit. Apply duplicate-key, node-count, depth, scalar
type, and schema checks after parsing. Do not claim yaml-cpp enforces node/depth
limits before constructing the YAML node graph.

### DAQ Application API Candidate

Expose public Runtime headers and owned configuration types without private
`src/` includes.

Separate concepts:

```text
DAQConfig
ResourceConfig
PayloadPoolConfig
LoggingConfig
AcquisitionPluginConfig
StoragePluginConfig
```

Include `Start`, `Stop`, state, stats, failure history, and Plugin topology.

### Packaging

Build the application-facing Runtime as a shared library with hidden visibility.
Install and export only the Runtime API Candidate and required transitive
dependencies. Add an installed DAQ consumer smoke and installed Plugin
loading smoke.

Static Runtime delivery is not implemented in this convergence unless a
separate decision defines a single merged static archive and complete
transitive link closure.

---

## 10. Phase 5 - Dependency Convergence

Every dependency is tracked independently as:

```text
PINNED
BUILT
LINKED
USED
QUALIFIED
```

Dependency groups:

```text
Required production:
    expected-lite
    HdrHistogram_c
    spdlog
    fmt
    yaml-cpp

Optional candidate:
    mimalloc

Development test:
    GoogleTest

Development benchmark:
    Google Benchmark

Evaluated but not selected:
    oneTBB
```

Required changes:

- Validate submodules conditionally.
- `BUILD_TESTING` requires GoogleTest only when tests are enabled.
- `RODAQ_BUILD_BENCHMARKS` requires Google Benchmark only when enabled.
- mimalloc is required only when an explicit allocator backend is enabled.
- Rename `RODAQ_USE_MIMALLOC` so build availability is not confused with use.
- Remove oneTBB and mimalloc from the V1 baseline if the reference audit finds
  no concrete use case. Record them as evaluated and not selected.
- fmt is a real dependency when spdlog uses `SPDLOG_FMT_EXTERNAL=ON`.
- Add license/notice and SBOM inventory.
- Verify normal build without optional/development submodules present.

---

## 11. Phase 6 - Reference-driven Implementation Review

Create `docs/development/reference_audit.md` with one record per relevant
module:

```text
Module
Reference project
Pinned tag/commit
License
Reviewed source/API area
Mechanism considered
Mechanism adopted
Mechanism rejected and why
RoDAQ semantic differences
Evidence tests/benchmark
```

Review after implementation:

- ResourceBudget and Admission.
- Payload size classes and release lifecycle.
- Slot state machine.
- Registry and handles.
- Structural Arena.
- Plugin loading/destroy/quarantine.
- Logging queue and shutdown.
- Config validation.

An upstream reference does not replace RoDAQ correctness tests.

---

## 12. Phase 7 - Code Quality and Structural Consolidation

Freeze lock ownership and ordering in Phase 1, and evolve code within those
boundaries during Phases 2 through 4.

Rules:

```text
no Plugin or backend callback under Runtime structural locks
no blocking condition wait while holding unrelated structural locks
no ordinary allocation after Gate Pass
no duplicate cleanup paths with different semantics
one central failure/disposition mapping
```

Candidate responsibility boundaries, adopted only where lock and lifetime
analysis supports them:

```text
RuntimeLifecycleController
SubmissionEligibility
PluginHealthState
DescriptorReconciler
StorageHandoffCoordinator
CompletionCoordinator
PayloadReleaseCoordinator
FailureCoordinator
```

Phase 7 performs final consolidation, not the first large structural refactor.
File size alone is not justification for a split.

---

## 13. Phase 8 - Complete Qualification

During development, run targeted compile/tests only. At the end of convergence,
run the complete matrix:

```text
Ubuntu 22.04 / GCC 11
    Debug normal with warnings-as-errors
    ASan + LSan
    UBSan

Clang 14 TSan
    admission
    Payload classes
    release worker
    Storage slots
    logging
    Registry
    quarantine
    M1 integration
    complete Runtime pipeline

Ubuntu 24.04
    normal full CTest

Installation
    Runtime shared-library consumer
    Plugin SDK consumer
    installed Plugin loading
    RPATH/RUNPATH/DT_NEEDED smoke

Performance
    Release capacity profiles
    class and queue high-water
    tail latency
    Stop latency
    soak
```

If the TSan runtime cannot execute because of the environment, use a qualified
runner or record `DEFERRED`; never report PASS without actual execution.

---

## 14. Exit Gate

M2 may return to PASS only when all conditions below have linked evidence:

```text
[x] Stable C ABI layout/protocol/vtable/service IDs unchanged
[x] A-01 blocking release converges or quarantines within deadline
[x] A-02 all pointer/count inputs are bounded before dereference
[x] A-03 Plugin health transitions and recovery are implemented
[x] A-04 Plugin destroy is supervised or quarantined
[x] A-05 normal slot return is O(1)
[x] A-06 size-class PayloadPool and independent budgets pass
[x] A-07 bounded LoggingContext and Plugin routing pass
[x] A-08 installed DAQ Application consumer passes
[x] A-09 eligibility invalidation interrupts blocked Admission
[x] A-10 malformed Storage outputs fail closed
[x] A-11 generation exhaustion is explicit and non-wrapping
[x] Copy materialization occurs before Gate Pass
[x] Every Accepted Adopt/Retain has pre-guaranteed release ownership
[x] Release state remains orthogonal to Persistence state
[x] No stuck release worker is detached or destroyed unsafely
[x] Accepted no-drop and same-Stream ordering still pass
[x] Camera Runtime capacity profile passes on Fake Storage
[x] Raw camera Runtime pressure profile passes on Fake Storage
[x] Aggregated joint Runtime capacity profile passes on Fake Storage
[x] Per-joint 100k records/s Runtime capacity profile passes on Fake Storage
[x] Mixed Runtime capacity profiles pass on Fake Storage
[x] current occupancy and all current permits converge to zero
[x] high-water remains bounded and is retained
[x] Stop converges for all lifecycle, logging, and release failure tests
[x] normal builds work without optional/dev submodules
[x] dependency lifecycle and reference-audit records are complete
[x] install/export and package consumer pass
[x] Runtime API is marked Candidate, not prematurely frozen
[x] plan/status/decisions match actual evidence
[x] M3 remains NOT STARTED
```

M2 Fake capacity is not evidence of sustained MCAP/NVMe/filesystem production
throughput. That remains an M5 qualification.

---

## 15. File-by-file Modification Map

### Design and development records

| File | Change |
|---|---|
| `docs/development/review_report.md` | Convert findings to evidence ledger |
| `docs/development/convergence_plan.md` | This approved implementation plan |
| `docs/development/reference_audit.md` | Add upstream source-reference records |
| `docs/development/decisions.md` | Add D-045 through D-053 |
| `docs/development/plan.md` | Track M2 convergence work and gates |
| `docs/development/status.md` | Record only verified evidence |
| `docs/design/rodaq_v1_system_design.md` | Align gates, workload layers, dependencies |
| `docs/design/runtime.md` | Admission ordering, slots, locks, capacity |
| `docs/design/admission.md` | Pre-Gate materialization and release ownership |
| `docs/design/payload.md` | Size classes and physical/logical accounting |
| `docs/design/logging.md` | Final logging lifecycle |
| `docs/design/rodaq_plugin_system.md` | Release/destroy/health/lease contracts |
| `docs/design/acquisition_plugin.md` | Health and recovery semantics |
| `docs/design/storage_plugin.md` | Submit output semantic matrix |
| `docs/design/canonical_profile.md` | Timestamp count and generation limits |

### Public API and Runtime implementation

| File | Change |
|---|---|
| `include/rodaq/daq.hpp` | New public DAQ Application API Candidate |
| `include/rodaq/daq_config.hpp` | Public configuration boundaries |
| `src/logging.hpp` | Logging facade/configuration |
| `src/payload_pool.hpp` | Size-class API |
| `include/rodaq/daq_stats.hpp` | Extended metrics |
| `include/rodaq/config/config.hpp` | Public RoDAQ-owned config model |
| `src/daq/daq_impl.hpp` | Remove from public boundary |
| `src/daq/daq.cpp` | Explicit lifecycle and public API |
| `src/activation/activation_state.hpp` | Health, eligibility, supervised cleanup |
| `src/activation/activation_state.cpp` | ABI service bridge and state coordination |
| `src/storage_pipeline.hpp` | Orthogonal slot axes and release integration |
| `src/storage_pipeline.cpp` | O(1) return and pre-guaranteed release work |
| `src/payload.cpp` | Lease-bearing Payload anchors |
| `src/payload_pool.cpp` | Size-class allocation |
| `src/payload_pool_state.hpp` | Per-class state |
| `src/payload_release_executor.hpp` | New release executor policy/state |
| `src/payload_release_executor.cpp` | Worker and quarantine handoff |
| `src/quarantine.hpp` | Worker bundle ownership |
| `src/quarantine.cpp` | Adopt/reap bounded quarantine bundles |
| `src/admission/resource_budget.cpp` | Admission-owned Resource integration |
| `src/logging_context.hpp` | New bounded logging context |
| `src/logging_context.cpp` | Queue, drop, metrics, flush/shutdown |
| `src/spdlog_backend.hpp` | Private backend |
| `src/spdlog_backend.cpp` | spdlog/fmt adaptation |
| `src/logger.cpp` | Logger facade delegation |
| `src/metrics/stats_accumulator.cpp` | Class/rejection/queue/health metrics |
| `src/config/yaml_config_loader.hpp` | New private parser |
| `src/config/yaml_config_loader.cpp` | YAML to ConfigTree conversion |

### Plugin implementation

| File | Change |
|---|---|
| `src/plugin/plugin_instance.hpp` | Explicit destroy and lease state |
| `src/plugin/plugin_instance.cpp` | No implicit external destroy |
| `src/plugin/acquisition_plugin_instance.cpp` | Release/health lease integration |
| `src/plugin/storage_plugin_instance.cpp` | Output validation |
| `src/plugin/config_view.hpp` | Internal adapter to public config model |
| `src/plugin/config_view.cpp` | Config ABI bridge |
| `src/abi_support.cpp` | Checked ABI helpers |
| `include/rodaq/plugin/abi_support.hpp` | Bounded validation declarations |
| `include/rodaq/plugin/services.hpp` | Semantic firewall |
| `include/rodaq/plugin/storage_plugin.hpp` | Semantic firewall |

### Build and packaging

| File | Change |
|---|---|
| `CMakeLists.txt` | Shared Runtime option/default |
| `src/CMakeLists.txt` | Public/private target boundaries |
| `src/CMakeLists.txt` | New sources and shared library |
| `src/plugin/CMakeLists.txt` | Hidden private linkage |
| `cmake/dependencies.cmake` | Conditional dependency groups and states |
| `cmake/options.cmake` | Explicit mimalloc/static options |
| `cmake/install.cmake` | Install/export Runtime and dependencies |
| `cmake/RoDAQConfig.cmake.in` | Consumer import contract |
| `.github/workflows/m0-qualification.yml` | Add convergence/package checks |

### Benchmarks and tests

| File | Change |
|---|---|
| `benchmarks/joint_pipeline_benchmark.cpp` | Formula-driven capacity and full metrics |
| `benchmarks/daq_pipeline_benchmark.cpp` | Capacity-driven profiles |
| `benchmarks/payload_pool_benchmark.cpp` | Size-class coverage |
| `benchmarks/camera_pipeline_benchmark.cpp` | New encoded/raw camera benchmark |
| `benchmarks/mixed_workload_benchmark.cpp` | New mixed Runtime capacity benchmark |
| `benchmarks/payload_class_benchmark.cpp` | New class selection/allocation benchmark |
| `benchmarks/storage_slot_benchmark.cpp` | New O(1) slot-return benchmark |
| `tests/payload/payload_pool_test.cpp` | Multi-class/budget tests |
| `tests/storage/storage_pipeline_test.cpp` | New release/slot state tests |
| `tests/logging/logging_context_test.cpp` | New logging tests |
| `tests/config/config_loader_test.cpp` | New YAML tests |
| `tests/metrics/observability_test.cpp` | Extended metrics tests |
| `tests/admission/admission_test.cpp` | Eligibility interruption tests |
| `tests/canonical/registry_test.cpp` | Generation exhaustion |
| `tests/plugin/plugin_lifecycle_supervision_test.cpp` | New destroy/release tests |
| `tests/plugin/abi_semantic_firewall_test.cpp` | New ABI matrices |
| `tests/plugin/runtime_services_test.cpp` | Health and validation tests |
| `tests/integration/fake_acquisition_plugin.cpp` | Blocking release/health scenarios |
| `tests/integration/fake_storage_plugin.cpp` | Malformed output scenarios |
| `tests/integration/runtime_pipeline_test.cpp` | End-to-end convergence cases |
| `tests/abi/install/daq_consumer/CMakeLists.txt` | Installed Runtime consumer |
| `tests/abi/install/daq_consumer/main.cpp` | Installed Runtime smoke |
| `tests/CMakeLists.txt` | Test target registration |
| `tests/CMakeLists.txt` | Runtime test registration |
| `tests/plugin/CMakeLists.txt` | Plugin test registration |
| `tests/integration/CMakeLists.txt` | Integration test registration |
| `tests/abi/CMakeLists.txt` | Install and ABI qualification |

---

## 16. Tests to Add or Change

Mandatory correctness tests:

```text
blocking Payload release
blocking Plugin destroy
release worker ownership transfer
release executor progress timeout
UINT32_MAX timestamp key/value count
pointer/count mismatch
oversized APPENDABLE output
Storage submit output matrix
Storage callback diagnostic matrix
Plugin health transitions
terminal failure without diagnostic
recoverable failure invalidates old Binding
available requires new Binding
Source unavailable during Admission wait
Stream unavailable during Admission wait
Binding invalidation during Admission wait
payload class exhaustion
large and small Payload coexistence
release enqueue invariant violation
O(1) slot return
double terminalization
generation exhaustion
logging queue exhaustion
logging byte exhaustion
logging recursion
logging flush/shutdown timeout
YAML file size, depth, node count, duplicate key, and type errors
installed DAQ consumer
installed Plugin loader
```

Each test must identify the contract it verifies. A passing test without a
contract-to-test mapping is insufficient.

---

## 17. Benchmarks to Add or Change

### Camera Runtime capacity

```text
10 cameras x 1280x720 x 30 FPS
encoded configured Payload size
```

### Raw camera Runtime pressure

```text
10 cameras x 1280x720 RGB8 x 30 FPS
approximately 791 MiB/s of offered Payload bytes
```

### Joint Runtime capacity

```text
Aggregated:
    1,000 records/s
    80,000 B Payload/record

Per-joint:
    100,000 records/s
    800 B minimum Payload/record
```

### Mixed Runtime capacity

```text
encoded cameras + aggregated joint telemetry
raw cameras + per-joint telemetry
```

### Payload and slot benchmarks

```text
single-size vs size-class pools
Copy vs Adopt vs Retain
release worker count 1/2/N
dispatch capacity scaling
Storage batching
slot terminalization scaling
```

Every benchmark report includes:

```text
offered and accepted record rates
offered and accepted Payload bytes/s
CPU
RSS
allocation count
copy bytes
P50/P95/P99/P99.9
Admission wait
current and high-water values
WaitTimeout/rejection counts
release latency
Stop duration
```

---

## 18. Documentation Changes

- Update normative documents before code implementation.
- Link each code change to a design amendment and durable decision.
- Replace ambiguous `HWM returns to zero` wording with current occupancy
  convergence and retained bounded HWM.
- Distinguish M2 Runtime capacity from M5 production qualification.
- Record whether config limits are pre-parse or post-parse.
- Remove any conditional milestone status such as `PASS with follow-up`.
- Add evidence links and validation commands to `status.md`.
- Keep `plan.md` checkboxes factual.
- Do not claim a dependency is implemented merely because its submodule exists.

---

## 19. CMake and Packaging Changes

- Build `librodaq.so` with hidden visibility.
- Keep private dependencies behind the DAQ DSO.
- Export the Runtime API Candidate.
- Install public headers and configuration model only.
- Provide a CMake package consumer.
- Add installed Plugin SDK and Plugin-loading smoke.
- Add RPATH/RUNPATH and recursive `DT_NEEDED` qualification foundation.
- Split dependency validation into required, optional, test, and benchmark
  groups.
- Ensure a normal Runtime build succeeds when optional/development submodules
  are absent.
- Add third-party notice/SBOM inventory generation.
- Define AppImage input layout and dependency-closure checks, while leaving final
  AppImage production qualification to M5.

---

## 20. Dependency Decision Matrix

| Dependency | Classification | V1 decision |
|---|---|---|
| expected-lite | Production Public C++ header | Keep |
| HdrHistogram_c | Production private metrics | Keep and instrument |
| spdlog | Production private logging | Link/use/qualify with LoggingContext |
| fmt | Required external fmt for spdlog | Link/use/qualify |
| yaml-cpp | Production private config frontend | Link/use/qualify |
| oneTBB | Evaluated optional candidate | Remove from V1 baseline unless a concrete use appears |
| mimalloc | Optional allocator experiment | Remove unless explicit allocator backend is selected |
| GoogleTest | Development test | Conditional only |
| Google Benchmark | Development benchmark | Conditional only |
| mcap-cpp | M3 backend dependency | Do not add in this convergence |
| ROS2 libraries | M3 Plugin dependency | Do not add in this convergence |
| Fast DDS / Fast CDR | M4 Plugin dependency | Do not add in this convergence |

The final dependency choice is recorded after source audit and benchmark
evidence. No dependency is retained merely because it is already pinned.

---

## 21. Open-source Reference Matrix

| RoDAQ module | Required reference areas |
|---|---|
| ResourceBudget / Admission | gRPC ResourceQuota/MemoryQuota, Aeron backpressure, Fast DDS ResourceLimits |
| PayloadPool | iceoryx MemPool, Fast DDS PayloadPool/resource manager |
| Slot lifecycle | bounded pool and reusable-index implementations |
| Registry / handles | iceoryx FixedPositionContainer, LLVM handle/container patterns |
| Structural Arena | Protobuf Arena, LLVM BumpPtrAllocator |
| Lifecycle / quiescence | eCAL, rosbag2, gRPC, PipeWire |
| Plugin system | PipeWire SPA, PlotJuggler SDK, LLVM Plugin, class_loader/pluginlib, gz-plugin |
| Logging | spdlog internals and bounded logging queue/shutdown designs |
| Config | yaml-cpp conversion and validation behavior |

Each record must include project, pinned commit/tag, license, reviewed source,
adopted/rejected mechanisms, RoDAQ differences, and linked tests.

---

## 22. Risks and Rollback Strategy

| Risk | Mitigation |
|---|---|
| Release worker hangs or leaks | Pre-guaranteed release ownership, quarantine bundle transfer, subprocess tests |
| Copy remains after Gate Pass | Admission-order invariant tests and code inspection |
| Size classes consume too much memory | Derived profiles, class HWM, occupancy metrics |
| Plugin health creates contradictory routing | One submission eligibility predicate and routing invalidation |
| Logging blocks Runtime shutdown | Fixed queue, flush deadline, quarantinable worker |
| Shared Runtime introduces DSO closure complexity | Hidden visibility, DT_NEEDED and AppImage smoke |
| Public API over-frozen too early | Keep API Candidate through M3/M4 |
| Optional dependencies still required | Conditional CMake validation tests |
| Benchmark machine noise | Repeat runs, CPU controls, medians/tail metrics, recorded hardware |

Rollback is by independent commit or complete revert. Do not retain broken
private implementations or compatibility shims merely to simplify rollback.

---

## 23. Commit Breakdown

```text
1. docs: freeze convergence evidence and design decisions
2. fix(runtime): enforce Pre-Gate materialization and release ownership
3. fix(runtime): supervise Plugin release and destroy
4. fix(plugin): implement ABI semantic firewall
5. feat(runtime): implement Acquisition Plugin health and eligibility
6. fix(runtime): make Storage slot return O(1)
7. feat(payload): implement isolated Payload size classes
8. feat(logging): implement bounded Runtime logging
9. feat(config): add public config model and YAML loader
10. build(runtime): deliver and export shared Runtime package
11. build(deps): converge dependency validation and optionality
12. refactor(runtime): consolidate lock domains and responsibilities
13. test(bench): add convergence correctness and capacity suites
14. docs(status): record final convergence qualification evidence
```

Commits must remain individually buildable and reviewable. The exact number may
change only when implementation dependencies require a different boundary.

---

## 24. Final Self-review Checklist

1. All confirmed P0/P1 findings are assigned: **yes**.
2. Design and implementation defects are distinguished: **yes**.
3. Every design requirement has an owning phase: **yes**.
4. Dependency pinning is not treated as implementation: **yes**.
5. Accepted no-drop is preserved: **yes**.
6. Same-Stream ordering is preserved: **yes**.
7. Stable C Plugin ABI remains unchanged: **yes**.
8. Stop deadlines cover Plugin release and destroy: **yes**.
9. Copy and Adopt/Retain materialization occur before Gate Pass: **yes**.
10. Accepted records have pre-guaranteed release capacity: **yes**.
11. Release and Persistence states remain orthogonal: **yes**.
12. Stuck release workers transfer to bounded quarantine ownership: **yes**.
13. Large camera and high-rate small Record workloads are covered separately
    and mixed: **yes**.
14. M2 Fake capacity and M5 real production qualification are distinct: **yes**.
15. Current occupancy returns to zero while HWM remains observable: **yes**.
16. Optional and development dependencies are truly conditional: **yes**.
17. Runtime delivery has an explicit shared-library decision: **yes**.
18. Runtime public API remains a Candidate until real backend validation: **yes**.
19. Plugin health uses one eligibility predicate: **yes**.
20. Storage diagnostics are implemented from an explicit matrix: **yes**.
21. Reference audits occur before design freeze and after implementation: **yes**.
22. M2 Exit Gate is mechanically tied to tests, installs, metrics, and evidence:
    **yes**.

**Self-review result:** R1 is ready for independent approval as the convergence
implementation baseline. Implementation must not start until that approval and
the Phase 0 baseline commit are complete.

## M2 Final Closure Fix — 413ba65 follow-up

Baseline: `413ba651f7d9fb1611ec294d4cc8e9799d4363b4`. Historical review and
qualification above are retained; they do not qualify the final closure.

- FR-01: automatic supervisor, unique Stop/Failure ownership, failure Dispatch
  cutoff, queued Failed classification and safe cleanup continuation implemented.
- FR-02: dynamic Descriptor FAILED escalates outside structural locks;
  Unsupported stays local and Projection retry is retained.
- QF-01: completed workers Join outside the Quarantine mutex, return capacity,
  and are reaped by process maintenance. TSan thread-leak exemption removed.
- Final implementation: `fe4f938adef1680021201334dae0c434f67d4011`. Full local matrix PASS: GCC 11
  Debug/ASan+LSan/UBSan/Release and Ubuntu 24 GCC 13 each 93/93 CTest;
  Clang 14 TSan 9/9 without thread-leak exemption; capacity and Runtime-only
  PASS. See `review_report.md` final closure appendix. No new hosted CI Run.

M2 Final Closure Fix COMPLETE; M2 PASS; M3 NOT STARTED. Native arm64 remains
DEFERRED by D-034.
