# RoDAQ Development Status

**Updated:** 2026-11-07
**Branch:** `develop`
**M0 closure baseline:** `30163c8` — `fix: complete M0 closure repair`
**M1 closure implementation baseline:** `eecd752` — `style: format runtime registry test`
**Convergence implementation commit:** `dda407e` — `feat(runtime): converge V1 runtime foundation`
**Core Structure Closure base:** `develop@9e60230`
**Core Structure Closure implementation:** `850f339`, `d45a945`

This file records factual current repository state. Keep it concise and update it after meaningful implementation or validation work.

## Current milestone

**M2 — Fake End-to-End Runtime + V1 Runtime Foundation Convergence**

Status: **ACTIVE — Status/Result/Logging infrastructure refactor; full
Qualification matrix pending**

M0 historical qualification is **PASS**. D-058 replaces the earlier C/C++
status source interfaces; applications and plugins must rebuild, and old hosted
evidence does not qualify this revision. Native arm64 remains deferred by D-034.

M1 Runtime Core Primitives is **PASS**, contract-aligned and qualified. The V1
Runtime Foundation Convergence closed all reviewed P0/P1 findings: bounded
release/destroy quarantine, complete ABI semantic firewall, Plugin health,
O(1) slot return, isolated Payload size classes, bounded LoggingContext,
YAML ConfigLoader, shared Runtime delivery, dependency convergence, source
reference audit, observability and target workload capacity qualification.
M3 is **NOT STARTED**.

The Release Fake Runtime capacity gate now passes encoded/raw 10-camera 720p30,
aggregated and per-joint joint telemetry, and both mixed profiles. M2 does not
claim real MCAP/filesystem/NVMe production capacity; that remains M5 work.

The V1 backend scope has now converged to ROS2 Acquisition + FastDDS Acquisition + MCAP Storage. ROS2 and FastDDS are required alternative process profiles by default; same-process co-activation remains unsupported unless Fast DDS/Fast CDR dependency coexistence qualification passes. SQLite3, Generic DDS and SocketCAN remain future architecture-validation targets.

## 2026-11-07 Status, Result and Logging infrastructure refactor

Status: **ACTIVE**. The previous M2 PASS / Core FROZEN state is temporarily
reopened until final qualification completes.

Implemented:

- `Status()` and `Status::Ok()` are success; OK plus diagnostics aborts.
- `Status` is a header-only rocomm-style value type with `code()`, `domain()`,
  `error_code()` and `message()`. It preserves arbitrary `StatusCode` values
  without constructor normalization.
- `Result<T>` is a RoDAQ wrapper over private `tl::expected<T, Status>` 1.3.1,
  with no non-void default construction, no implicit boolean conversion and
  abort-on-invalid-access semantics.
- expected-lite was removed; installed `tl/expected.hpp` and its license replace
  it.
- `rodaq::Logger` is a weak, activation-scoped handle. `DAQ::logger(category)`
  succeeds only while Running; old handles no-op after Stop and never revive
  across restart.
- Logging supports sealed record and byte bounds, Sync/AsyncBounded,
  DiscardNew/OverrunOldest/Block, Console/File/RotatingFile/DailyFile sinks,
  level/pattern/time-zone settings, periodic and level flush, backtrace,
  source location, formatting isolation, reentrancy drop, statistics, bounded
  flush/shutdown and worker quarantine.
- `RODAQ_LOG_*` are public logging macros. Stable ABI constants are
  `RODAQ_ABI_LOG_*` with unchanged values 1..7 and no aliases.
- `rodaq::daq` publicly exports exact fmt 12.1.0 and spdlog 1.17.0 package
  dependencies. Installation includes their headers, static archives and CMake
  packages; an external installed consumer compiles and links a fmt logging
  macro.
- `rodaq::sdk` remains independent of DAQ, YAML, fmt and spdlog.

Targeted local evidence:

- Status/Result runtime and compile-fail contracts PASS, including
  `Result<Status>`, array/cv rejection and nodiscard enforcement.
- rocomm-alignment core verification PASS: header-only Status, `Ok()/code()`,
  raw unknown C++ status preservation, ABI 0..10 mapping, SDK compilation,
  installed consumers and Fake Runtime integration — **15/15 targeted tests**.
- Logging context/logger/backend tests PASS for all sinks, duplicate conflicts,
  overflow policies, byte/record bounds, periodic and requested flush, timeout,
  backtrace, formatting failure, reentrancy, stop lifetime and quarantine.
- DAQ Fake Pipeline integration PASS, including Running-only Logger, restart
  invalidation and explicit mapping of every ABI severity.
- ABI header isolation, exact fmt/spdlog installation closure, installed DAQ,
  SDK and combined consumers, public Logger symbols and hidden implementation
  symbols PASS.
- GCC 11.4 Debug with warnings-as-errors: **108/108 CTest PASS** before the
  final active-writer exception-rollback hardening.
- After that final logging change, only `rodaq.logging.context` and
  `rodaq.logging.logger` were rerun: **2/2 PASS**.

Release, ASan/LSan, UBSan, TSan, Ubuntu 24.04, clang-tidy and capacity
requalification have not been completed for this revision. Per current project
direction, the full matrix is deferred rather than rerun during this
implementation pass.

The dated historical sections below preserve prior evidence. Where they refer
to expected-lite, nonstd storage, private fmt/spdlog dependencies, the old
Logger wrapper or `M2 PASS / Core FROZEN`, this section and D-067 through
D-070 supersede them.

## 2026-11-06 Core implementation structure and naming closure

Status: **PASS / FROZEN — local Debug, Release, ASan/LSan, UBSan, Clang TSan,
Ubuntu 24.04, ABI, installation, symbol and capacity qualification complete.
Hosted qualification is PENDING. M2 functional PASS remains valid. M3 is NOT
STARTED.**

The pre-M3 source-layout migration is implemented:

- Public application API is `rodaq::DAQ` with `DAQConfig`, `DAQState`,
  `DAQStats`, `DAQLatencyStats` and `DAQLifecycleSnapshot`.
- Public C++ lifecycle/config surfaces do not return Stable C ABI types.
- C++ and logical configuration use `acquisition_id`; Storage ABI field
  `session_id` remains unchanged for ABI compatibility.
- Internal implementation defaults to namespace `rodaq`; Plugin subsystem
  implementation uses `rodaq::plugin`; no generalized `detail`, `internal` or
  `impl` namespace exists.
- `DAQ::Impl` is one private Pimpl type implemented by `daq.cpp`,
  `lifecycle.cpp` and `recovery.cpp`.
- `ActivationState` is the activation lifecycle aggregate root and composes
  independently owned `CanonicalState`, `BindingState`, `AdmissionState` and
  `ActivationMetrics`. Canonical registration/availability and descriptor lookup,
  Binding lifecycle, Admission accounting and Metrics invariants are held by
  their owning state objects.
- `AcceptedRecord`, `TimestampState` and `AcceptanceSequenceAllocator` have
  Record, Canonical and Admission ownership respectively.
- `src/runtime`, `include/rodaq/runtime`, `tests/runtime`, `src/sdk`, `failure`,
  `resource` and `observability` source-layout paths are removed.
- Shared DAQ target is `rodaq::daq` / `librodaq.so`. `rodaq::sdk` is a
  self-contained static authoring target without DAQ, YAML, Plugin Host, fmt or
  spdlog dependencies. Installed CMake exports contain only those two public
  targets; `rodaq_plugin`, `rodaq_objects` and `rodaq_testing` remain internal.
- ABI type, macro and protocol source names no longer contain version suffixes;
  numeric protocol-version negotiation remains.
- `View`, `Ref` and `Info` follow D-063; Canonical ABI metadata uses
  `rodaq_*_info`, and the obsolete `*_view` source names have no aliases.
- A naming/path residue check runs in CTest.

Local evidence:

- GCC 11.4.0 Debug with warnings-as-errors: **105/105 CTest PASS**, including
  ABI layout/minimum-prefix/scalar/symbol, installed DAQ, SDK and combined
  consumers, Fake end-to-end pipeline, state-ownership tests, quarantine and
  stress coverage.
- GCC 11.4.0 Release with tests and benchmarks enabled: **105/105 CTest PASS**.
- GCC 11.4.0 ASan+LSan and UBSan: **105/105 CTest PASS** in each build.
- Clang 14 TSan selected concurrency/ownership suites: **16/16 CTest PASS**.
  The host requires `setarch x86_64 -R` to avoid a container ASLR mapping
  conflict with the Clang 14 TSan runtime.
- Ubuntu 24.04.5 / GCC 13.3 Release with warnings-as-errors: **105/105 CTest
  PASS**, including installed consumers and DSO symbol closure.
- Release camera, joint and mixed capacity profiles passed three repetitions.
  Median per-joint acceptance is 99,967 Records/s, raw-camera bandwidth
  854.7 MB/s, and raw mixed bandwidth 907.0 MB/s, with no resource or shutdown
  gate regression. Details are recorded in `capacity_qualification.md`.
- After the final BindingState accessor consolidation, Admission, Binding and
  Fake Runtime pipeline tests were rerun in Debug, ASan/LSan, UBSan and TSan;
  the installed consumer/symbol and residue checks were rerun from Release.
  All PASS.
- The project clang-format and clang-tidy check covers 238 files, 146
  translation units, all public headers and installed consumers: PASS.
- `nm -D -C` audit and a linker export map export the required `DAQ`, `Status`,
  Config model and YAML loader symbols; `DAQ::Impl`, Activation, Registry,
  PayloadPool, StoragePipeline, fmt/spdlog/yaml implementation and HdrHistogram
  symbols remain hidden.
- Dependency inventory has seven pinned submodules, no unselected oneTBB or
  mimalloc checkout, and no third-party include in installed headers.
- `BUILD_TESTING=OFF`, `RODAQ_BUILD_TESTS=OFF` and
  `RODAQ_BUILD_BENCHMARKS=OFF` Release configuration/build PASS.

The pre-M3 structural closure is locally complete and frozen. Hosted CI must
rerun before it is cited as externally qualified. M3 remains NOT STARTED until
separately authorized.

## Current repository state

M0-A Review Closure, M0-B Stable C Plugin ABI, M0-C Plugin Loading + Lifetime,
M0-D Runtime Services + ConfigView and M0-E SDK / Closure Repair are complete
and validated. M0 Final Contract Correction is also complete. The repository now
contains:

- a C++17 CMake project foundation,
- warning, sanitizer, CTest, dependency and install support,
- Public `rodaq::Status` and `rodaq::Result<T>` primitives,
- eight Stable C ABI headers under `include/rodaq/plugin/abi/`, with
  `common.h` as the shared primitive header,
- C11/C++17 ABI compilation, layout, minimum-prefix and symbol qualification,
- a private Linux Plugin loader with Boot ABI, family, descriptor and provenance validation,
- private DSO and instance lease ownership with bounded normal unload behavior,
- catalog/manager orchestration and explicit quarantine retention,
- pinned dependency submodules with no normal-build network access,
- installed-header validation for `rodaq::Status`, `rodaq::Result<T>` and the
  C++17 Plugin SDK,
- a complete C++17 Acquisition / Storage Plugin SDK,
- Runtime Service wrappers with invalid-state protection,
- Storage / Acquisition ABI structural input validation,
- frozen caller-output compatibility fixtures and bidirectional APPENDABLE
  capacity qualification,
- exception-firewall matrices for Storage Submit, Capabilities and Descriptor
  readiness,
- bounded Runtime handles, generation-protected Registry, PMR Structural Arena,
  Payload/PayloadView/PayloadPool with Copy/Adopt/Retain lifetime anchors,
  ResourceBudget/Reservation, AdmissionGate, AcceptedRecord,
  AcceptanceSequenceAllocator, StatsAccumulator, HdrHistogram latency wrapper and a
  no-throw Logger facade.

`third_party/` contains seven tracked pinned submodules: the five required
production dependencies plus GoogleTest and Google Benchmark. M0 also includes a
bounded Service Registry, ConfigView, activation-lifetime InteropProjection and
ClockCorrelation services, Fake Plugin lifecycle integration, plugin authoring
CMake support and an installed SDK consumer smoke. M1 Runtime primitives and the
M2 production Runtime core are delivered as a hidden-visibility shared Runtime
target with an installable Application API Candidate. M2 adds
the complete Stable-ABI-backed Fake Acquisition-to-Storage data path,
Activation/Binding lifecycle, bounded Admission and Dispatch, independent
Payload/Persistence completion axes, and bounded Stop/Failure quarantine.

Dependency lifecycle is not collapsed into one completion flag:

```text
tl_expected                    PINNED+BUILT+LINKED+USED+QUALIFIED
HdrHistogram_c                 PINNED+BUILT+LINKED+USED+QUALIFIED
spdlog/fmt                     PINNED+BUILT+LINKED+USED+QUALIFIED (DAQ public CMake)
yaml-cpp                       PINNED+BUILT+LINKED+USED+QUALIFIED
oneTBB                         EVALUATED+NOT-SELECTED; removed from baseline
mimalloc                       EVALUATED+NOT-SELECTED; removed from baseline
GoogleTest/Google Benchmark    development dependencies in use
```

## 2026-11-05 Runtime Foundation Convergence

The reviewed convergence baseline is implemented:

- Pre-Gate Copy materialization and pre-guaranteed external Payload release
  ownership.
- Explicit supervised Plugin destroy and blocked release/destroy worker transfer
  to process-wide quarantine.
- Per-worker release completion state, no detach and no unsafe join after an
  expired deadline.
- Payload move preserves the Plugin lifetime guard.
- Timestamp arrays are bounded before dereference; malformed ABI diagnostics and
  Storage submit outputs fail closed.
- Acquisition health states control submission/rebinding; terminal failure
  cannot be silent.
- Storage completion/release never proceeds from a Stop flag while `submit()` is
  unreconciled.
- Multi-size isolated Payload classes with exhaustion/physical/HWM statistics.
- Runtime-owned byte/record bounded asynchronous logging with Plugin routing,
  backend exception containment and quarantine.
- Public `DAQConfig/ResourceConfig/AcquisitionPluginConfig/StoragePluginConfig`, installed
  shared Runtime and external consumer smoke.
- YAML ConfigLoader with pre-read file limit and post-parse depth/node/duplicate
  validation.
- Runtime latency summaries for Admission, handoff, Payload terminal,
  Persistence terminal and Stop.
- Camera, joint, mixed, storage-slot, Payload-class and Release-worker
  benchmarks with offered-schedule enforcement.

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
  - five production dependencies plus two development dependencies pinned as
    Git submodules; oneTBB/mimalloc were later evaluated and removed,
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

## M2 core implementation

- Production Runtime lifecycle, activation-local Canonical registries and
  generation-protected handles.
- Complete Stable Runtime Service provider path for Acquisition registration,
  report, submit, projection, clock correlation and Storage completion.
- Descriptor readiness and Acquisition-coordinated Binding transitions,
  including initial Binding prepare/commit and `KEEP_PAUSED` / `RESUME_OLD`
  abort behavior.
- Concurrent ingress with per-Stream Admission Commit, irreversible
  AcceptanceSequence allocation and ordered no-drop dispatch publication.
- Preallocated bounded Dispatch and operation slots, one Storage Dispatch Lane,
  one Completion Domain, early cross-thread reconciliation, exact completion
  validation and uncertain-work quarantine.
- Storage capability negotiation now freezes effective Payload, record, byte,
  completion and progress-timeout limits for each activation.
- Independent Payload-byte and operation-record accounting, with Payload release
  proven while Persistence remains pending.
- Real DSO-backed Fake Acquisition and Fake Storage qualification, including
  Copy/Adopt/Retain, mixed modes, stale-handle rejection, immutable views,
  descriptor transitions, saturation, timeout, stall, close/flush and contract
  violation matrices.
- Bounded Stop/quiesce and primary/secondary failure convergence, with
  process-lifetime quarantine when unsafe teardown cannot be proven.
- Activation creation, bind/configure/open, capability/readiness query and
  Acquisition start calls are deadline-supervised; stalled Starting cleanup is
  quarantined rather than destructively released.
- Descriptor publication now returns before Projection-dependent Storage
  readiness through one activation-local, fixed-capacity Descriptor
  reconciliation worker; Projection registration retries readiness and Binding
  activation without any Plugin call under a Runtime structural lock. Runtime
  Start waits for the initial reconciliation pass before opening the Admission
  Gate.
- Runtime Submit captures the monotonic ingress timestamp after structural,
  handle, Binding and Payload-limit validation but before Admission wait and
  Copy/Adopt/Retain materialization.
- Every activation owns one Canonical monotonic ingress Clock and a stable
  ClockCorrelation anchor. Descriptors contain the mandatory
  `rodaq/acquisition_ingress`; Submit transport timestamps are kept distinct and
  Storage receives the complete canonical timestamp set.
- Completion capacity participates in effective admitted-work capacity, and the
  Storage Dispatch Lane has a single bounded watchdog for handoff progress.
- The Storage handoff Watchdog tracks a monotonic Submit generation, so one
  completed call cannot leave a stale deadline on the next call.
- Registry insertion release-publishes fully constructed slots; stable readers
  acquire-observe publication before accessing immutable Registry values.
- TimestampKeySet publication rejects duplicate `(RoleIdentifier, ClockId)` pairs.
- Projection registration only enqueues Descriptor reconciliation; a fake
  Plugin-local lock scenario verifies no synchronous Binding callback reentry.
- Descriptor readiness and Acquisition prepare/commit/abort control callbacks
  are deadline-supervised through the bounded quarantine owner.
- Multi-Acquisition startup rollback uses explicit lifecycle states, partial
  Start failures are quiesced then stopped in reverse order, and timed-out Start
  calls are quarantined.
- Quarantine now has a bounded process-lifetime owner with joinable worker
  reaping and fixed call/activation capacities. The 32 process-wide activation
  roots include healthy and quarantined activations; exhaustion makes
  `DAQ::start()` return `ResourceExhausted`. No `detach()` or lost
  `std::thread::release()` path remains.

## Not started

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

No remaining known P0/P1 static design blocker prevents M3 implementation.

The Stable C ABI and private Plugin loading/lifetime boundary are implemented and
mechanically qualified. Service Registry, ConfigView, the C++17 Plugin SDK,
Runtime Service wrappers and Fake Plugin lifecycle are implemented. The Runtime
generic data plane is implemented and qualified through Stable-ABI Fake Plugins.
Real backend round trips have not been executed. ROS2/MCAP executable
qualification is the active M3 gate; FastDDS/MCAP interoperability is the M4
gate; production hardening/release qualification is M5. SQLite3 is a future
design-validation target rather than a V1 release gate.

The Result policy is now frozen by D-025:

- `rodaq::Result<T>` directly aliases `nonstd::expected<T, Error>`.
- expected-lite 0.10.0 is a pinned Public C++ header dependency.
- V1 always uses expected-lite's nonstd implementation.
- expected-lite never enters the Stable C Plugin ABI.

## Immediate next actions

1. Freeze the M3 ROS2 Acquisition + MCAP mapping against the current Stable ABI.
2. Pin and integrate the MCAP backend without introducing ROS2 runtime coupling
   into Storage Core.
3. Implement the ROS2 + MCAP vertical slice and interoperability fixtures.
4. Revalidate Runtime API Candidate compatibility with real backends.
5. Native arm64 qualification remains deferred until a runner is available.

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

M0-E Plugin SDK + Closure Repair
    IMPLEMENTED
    ABI INPUT VALIDATION TESTED
    INVALID-STATE WRAPPER TESTED
    FROZEN APPENDABLE CAPACITY TESTED
    SEMVER PARITY TESTED
    STORAGE EXCEPTION MATRIX TESTED
    NORMAL / ASAN / UBSAN TESTED
    PASS

M0 Final Contract Correction
    IMPLEMENTED
    APPENDABLE OUTPUT TESTED
    OUTPUT FLAGS / RESERVED TESTED
    FROZEN PREFIX COMPATIBILITY TESTED
    UBUNTU 22 NORMAL / ASAN / UBSAN TESTED
    UBUNTU 24 TESTED
    ARM64 DEFERRED BY D-034
    PASS

M1 Runtime Core Primitives
    IMPLEMENTED
    UNIT / CONCURRENCY / INTEGRATION TESTED
    NORMAL / ASAN / UBSAN TESTED
    CLANG TSAN CONCURRENCY TESTED
    REVIEWED
    CONTRACT-ALIGNED
    BENCHMARK BASELINE ESTABLISHED
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
- `RODAQ_BUILD_TESTS=OFF` and `RODAQ_BUILD_BENCHMARKS=ON`: configure and build
  passed.
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
- Submodule audit: the seven retained production/development submodules and
  the recursive dependency below HdrHistogram_c report clean worktrees at their
  pinned commits. oneTBB and mimalloc are no longer tracked dependencies.
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
- Benchmarks options: benchmarks subtree plus Google Benchmark configured and
  built successfully without optional allocator/concurrency dependencies.
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

M0-E / Closure Repair validation:

- Clean GCC 11.4.0 Debug with warnings-as-errors: build passed; 79/79 CTest
  entries passed.
- Clean ASan Debug with warnings-as-errors: build passed; 79/79 CTest entries
  passed with `ASAN_OPTIONS=detect_leaks=0` and `setarch x86_64 -R`. The host
  requires the ASLR wrapper for the GCC 11 ASan shadow mapping; LeakSanitizer is
  disabled because the environment reports ptrace incompatibility.
- Clean UBSan Debug with non-recovering undefined behavior and
  warnings-as-errors: build passed; 79/79 CTest entries passed.
- Clean Ubuntu 24.04 amd64 container with GCC 13.3.0 and
  warnings-as-errors: configure and build passed; 79/79 CTest entries passed.
- SDK validation covers null pointers, `struct_size`, flags, reserved fields,
  invalid UTF-8, pointer/count pairs, boolean/presence relationships, nonzero
  handles, timestamp arrays, payload views and control dispositions.
- Runtime wrapper tests cover all six public service families in invalid and
  valid states, including no-op void callbacks and `MonotonicTimeNs()` typed
  `InvalidState`.
- Frozen V1 output tests cover Storage capabilities, Descriptor readiness,
  Storage submit result, Projection and Clock outputs at minimum-minus-one,
  frozen-prefix, current-size and oversized capacities with intact tail canaries.
- Storage exception tests pass the 3 methods x 3 exception-type matrix through
  actual SDK ABI entrypoints with complete caller-owned diagnostics.
- Runtime and CMake SemVer validators accept the same boundary corpus, including
  build metadata `1.0.0+build-01`, and reject prerelease numeric leading zeros.
- Installed SDK consumer smoke passed.

M0 Final Contract Correction validation:

- SDK and Runtime caller-owned APPENDABLE outputs return
  `min(caller capacity, current type size)` and only zero the initialized prefix.
- Storage capabilities no longer exposes undocumented output flags.
- Frozen V1 compatibility fixtures validate append-only current layouts while
  retaining independent V1 definitions and exact prefix field offsets/widths.
- Clean Ubuntu 22.04 amd64 normal Debug: 79/79 CTest entries passed.
- Clean Ubuntu 22.04 amd64 ASan: 79/79 passed with
  `ASAN_OPTIONS=detect_leaks=0` and `setarch x86_64 -R`.
- Clean Ubuntu 22.04 amd64 UBSan: 79/79 passed.
- Clean Ubuntu 24.04 amd64 Docker container with GCC 13.3.0: 79/79 passed.
- Native arm64 remains deferred by D-034 and is not reported as PASS.
- Production writer audit found no remaining whole-current APPENDABLE output
  write; remaining `struct_size = sizeof(...)` sites are caller-owned inputs.
- Fresh configure/build network audit recorded no network syscalls.
- clang-format, `git diff --check`, whitespace and recursive submodule audits
  passed.

M1 Runtime Core Primitives closure validation:

- GCC 11.4.0 Debug with warnings-as-errors: 87/87 CTest entries passed.
- ASan: 87/87 CTest entries passed.
- UBSan with non-recovering undefined behavior: 87/87 CTest entries passed.
- Clang 14 TSan: admission, observability, payload and M1 integration
  concurrency tests passed with no reports. This Linux 6.8 host still requires
  the existing ASLR workaround. The GCC 11 TSan runtime reports mutex-ownership
  findings that Clang TSan does not reproduce.
- Handle and Registry tests cover invalid, capacity-exhausted, stale,
  double-erased and reused-slot handles.
- Payload tests cover Copy storage, Adopt and Retain external anchors, moved-from
  state, exact byte equality, pool reuse, exhaustion and exactly-once release.
- Admission tests cover Reject, deadline Wait, cancellation, Gate close,
  generation change, multiple waiters, Stop-style cutoff and repeated races.
- M1 integration verifies pre-gate resource unavailability does not consume an
  AcceptanceSequence, retained bytes stay budgeted, payload and reservation
  ownership compose, and all capacity returns to baseline after destruction.
- ResourceBudget arithmetic was tested at `UINT64_MAX`; PayloadPool setup and
  Registry creation convert allocation and length failures to RoDAQ errors.
- `clang-format`, `git diff --check`, source review and status/plan consistency
  checks passed.

M1 closure review answers:

1. Yes. AcceptedRecord uses `BindingHandle` as its sole Binding/Descriptor anchor.
2. No. Timestamp slots contain only presence and signed values.
3. No. AdmissionGate reports pre-commit `Granted`, never final Accepted.
4. No. UINT64_MAX is terminal; Gate reopening is refused after exhaustion.
5. Yes. Typed Runtime handles are Registry template parameters.
6. No. Failed stored-value construction preserves size and free capacity.
7. No. Invalid non-empty PayloadView remains invalid.
8. No. Adopt/Retain structural validation precedes responsibility acquisition.
9. No. Payload responsibility is released before ResourceBudget capacity.
10. No. Registry generation retirement prevents stale-handle alias.
11. No. Pre-gate resource unavailability cannot consume AcceptanceSequence.
12. No. AcceptanceSequence exhaustion is terminal and sequences never reuse.
13. No. Waiters wake on close, cancel, generation change, release, and deadline.
14. No. M1 Runtime primitives contain no backend dependency.
15. Yes. Normal, ASan, UBSan and required Clang TSan checks pass.
16. No. No known P0/P1 issue remains.

### Historical M2 audit-repair evidence (superseded by final convergence)

- GCC 11.4.0 normal, ASan with LeakSanitizer and UBSan each passed 89/89 CTest
  entries with warnings-as-errors.
- Ubuntu 24.04 with GCC 13.3.0 and warnings-as-errors passed 89/89 CTest
  entries, including benchmark target builds.
- Clang 14 TSan passed the admission, PayloadPool, observability, M1 integration,
  quarantine, Registry publication and complete M2 runtime pipeline subset:
  7/7 CTest entries.
- The M2 E2E executable now contains 55 Stable-ABI-backed tests covering
  lifecycle, Copy/Adopt/Retain, mixed payload modes, immutable canonical views,
  stale handles, multi-producer ordering, Binding switch and abort, saturation,
  blocked Wait timeout, every Payload/Persistence completion ordering,
  early-completion reconciliation, contract violations, bounded stalls,
  primary/secondary failure preservation, Projection-dependent readiness,
  ingress-before-Wait capture, Descriptor publication return ordering,
  reconciliation queue bounds, Stop during readiness, the activation-root
  budget, ClockCorrelation, Binding control stalls, multi-Acquisition rollback,
  Stop under load, duplicate TimestampKey rejection, Plugin-local lock isolation,
  continuous handoff across multiple timeout periods, rate-controlled exclusive
  joint-stream production and restart.
- `benchmarks/joint_pipeline_benchmark.cpp` now measures both 100-joint ×
  100-binary64-parameter × 1 kHz record groupings:
  - production aggregated RobotState Record: 1k target records/s, 80 kB payload.
  - stress per-joint Record: 100k target records/s, 800 B minimum payload.
- The current three-second Release Fake Pipeline baseline reached approximately
  83.7k steady-state records/s for per-joint Records and 1.0k steady-state
  records/s for aggregated RobotState Records. The per-joint profile is
  therefore not yet a PASS.
- The new rate-controlled/exclusive Fake Acquisition producer mode passed the
  complete runtime pipeline test under ASan+LSan, UBSan and Clang TSan.
- The runtime pipeline stress test passed 20 consecutive repetitions; the
  targeted handoff/Projection/Timestamp tests also passed 20 repetitions.
- Registry tests cover unpublished/fabricated handles during blocked insertion,
  concurrent publication, and stable lookup while later slots are inserted;
  the suite passed 20 repetitions and Clang TSan.
- Descriptor readiness, initial prepare/commit, switch prepare/commit failure,
  `KEEP_PAUSED`, `RESUME_OLD`, registration capacity and quarantine paths are
  covered.
- Storage readiness performs actual Canonical Descriptor and Projection queries.
  Initial readiness is reconciled by the bounded activation-local worker before
  the Admission Gate opens; dynamic publication returns before that work while
  fake producers wait for the terminal Binding transition.
- Duplicate-start tests assert that nested readiness and initial Binding
  failures produce one primary error, no duplicate secondary history and zero
  overflow, while genuine cleanup failures remain secondary errors.
- ASan ignores only the bounded process-lifetime quarantine owner and reachable
  roots. TSan thread-leak
  reporting is disabled only for the E2E executable because fault scenarios
  intentionally retain joinable Plugin workers and activation state for process
  lifetime; data-race reporting remains enabled.
- M2 short-run three-repetition medians were approximately 43.1k records/s for
  one producer and 81.7k records/s for four producers x four streams.
  Steady-state 4096-record medians reached approximately 77.6k records/s and
  95.7k records/s respectively in the final DEBUG run.
  The short-run difference is activation/watchdog startup, not a per-record
  structural regression; CPU scaling and DEBUG build warnings limit timing
  precision. These are engineering baselines, not M5 SLOs.
- `clang-format`, `git diff --check`, source review, Stable ABI diff review and
  searches for `detach()`, `recursive_mutex` and Runtime thread `.release()`
  passed.

M2 final review answers:

1. No. Dispatch capacity and operation/completion bookkeeping are reserved
   before Gate pass.
2. No. A full queue only rejects or waits before Gate pass; accepted publication
   cannot fail for ordinary capacity.
3. No. Post-Gate publication uses the already reserved record, ring slot and
   operation slot without ordinary heap growth.
4. No. Sequence allocation and dispatch publication share the per-Stream commit
   domain.
5. No. Only per-Stream order is promised; cross-Stream interleaving is not a
   contract.
6. No. Cross-thread completion is staged before submit return and reconciled
   after return.
7. No. A thread-local call frame distinguishes same-stack callbacks from legal
   early cross-thread callbacks.
8. No. Duplicate callbacks enter the fail-closed path.
9. No. A callback for a synchronous-terminal axis enters the fail-closed path.
10. No. Payload backing and logical byte accounting are released at
    PayloadConsumed even while Persistence remains pending.
11. No. Payload release follows Storage's consumed boundary.
12. No. PersistencePending retains only operation bookkeeping, not the large
    Payload backing charge.
13. No. Activation, Plugin instances and DSO leases remain retained whenever an
    Acquisition-owned release callback may still be reachable.
14. No. Uncertain Storage callbacks retain the complete activation and Storage
    DSO as process-lifetime quarantine.
15. No. Stop uses per-phase progress deadlines and bounded callback/quiescence
    barriers.
16. No. Fatal Storage failure closes the Gate and admission cutoff.
17. No. Accepted history is never rewritten as rejected; persistence failure is
    a separate outcome.
18. No. Primary failure is latched first; subsequent cleanup failures remain
    separately queryable secondary failures.
19. No. Post-close callbacks are detected and force failure/quarantine.
20. No. Activation generation is encoded into handles and stale activation
    handles are rejected through plugin-facing services.
21. No. Binding activation requires descriptor readiness and coordinated
    prepare/commit; failed prepare uses the explicit abort disposition.
22. No. M2 Runtime Core has no ROS2, FastDDS, MCAP or SQLite3 dependency.
23. Yes. Every production E2E record enters and completes through Stable ABI
    Plugin boundaries.
24. No. Ingress is captured after validation and before Admission wait or
    Payload materialization.
25. No. Descriptor publication returns the immutable handle before initial
    readiness work completes; Runtime Start still waits before opening Gate.
26. No. Readiness and Binding publication revalidate Runtime state, Descriptor
    generation, current Binding and transition token.
27. No. Nested readiness/Binding/commit failure is not duplicated into secondary
    failure history.
28. Yes. The 32 process-wide activation roots cover healthy and quarantined
    activations; exhaustion makes Start return `ResourceExhausted`.
29. Yes. Normal, ASan+LSan, UBSan and Clang TSan qualification is clean.
30. No. Each Storage handoff deadline is scoped to a unique Submit generation.
31. No. Registry readers acquire-observation of release-published slots before
    accessing immutable Registry values.
32. No. Duplicate `(RoleIdentifier, ClockId)` TimestampKeys are rejected during
    Descriptor publication.
33. No. Projection registration only enqueues reconciliation; the Plugin-local
    lock probe completes without synchronous Binding callback reentry.
34. No. The continuous handoff regression stayed Running for a total duration
    exceeding multiple per-Submit timeout periods.
35. No. No known P0/P1 issue remains.

## Historical convergence qualification (413ba65; superseded for current closure)

- GCC 11.4.0 Debug normal, ASan+LSan and UBSan: **93/93 CTest passed** in each
  build. ASan ran with `detect_leaks=1` and `halt_on_error=1`.
- GCC 11.4.0 Release with warnings-as-errors: full build and **93/93 CTest
  passed**.
- Ubuntu 24.04 container with GCC 13.3.0: full build and **93/93 CTest passed**,
  including installed DAQ/Plugin consumers and DAQ DSO closure checks.
- Previously recorded Clang 14 TSan under `setarch x86_64 -R` (not accepted
  as final evidence: pipeline disabled thread-leak reporting): admission, PayloadPool,
  observability, logging, Registry, quarantine, StoragePipeline, M1 integration
  and complete Runtime pipeline: **9/9 CTest passed**.
- Normal Runtime build with tests/benchmarks, oneTBB, mimalloc, GoogleTest and
  Google Benchmark removed from the source copy: configure and build passed.
- `clang-format 14 --dry-run --Werror` passed for changed/new C/C++ files;
  `git diff --check` passed; source search found no `detach`,
  `recursive_mutex`, or Runtime thread `.release()`.
- DAQ DSO dynamic exports are limited to `rodaq::DAQ`; private
  fmt/spdlog/yaml/HdrHistogram symbols and `DT_NEEDED` entries are absent.
- Capacity evidence is recorded in
  `docs/development/capacity_qualification.md`. Key Release results:
  per-joint 99.9k accepted Records/s, raw camera 851.8 MB/s, raw camera +
  per-joint mixed 905.7 MB/s, encoded camera + aggregated joint 1.3k Records/s,
  and all profiles converged without rejection/WaitTimeout with zero current
  occupancy after Stop.

Native arm64 remains deferred by D-034. Stable C ABI layout/protocol/vtable/
service IDs remain unchanged. M0, M1 and M2 are PASS. M3, M4 and M5 are
**NOT STARTED**; no milestone is currently ACTIVE.

## M2 Final Closure Fix — COMPLETE

Implementation: `fe4f938adef1680021201334dae0c434f67d4011` (`fix(runtime): retain quarantine roots until worker join`).
Baseline: `413ba65`. FR-01 automatic failure convergence, FR-02 dynamic fatal
Descriptor readiness and QF-01 final Quarantine Join are closed.

- One joinable Runtime supervisor shares logical lifecycle ownership and cleanup
  with Stop; Starting keeps activation abort. No Plugin I/O or Join in
  RequestFailure, no asynchronous Runtime raw pointer, no duplicate cleanup.
- Fatal cutoff prevents future Storage handoff authorization, classifies queued
  Accepted Records as PersistenceFailed and retains uncertain in-flight Payload.
  A returned successful close proves access safety, not persistence success.
- Completed retained workers Join outside the Quarantine mutex, return capacity,
  and safe activation cleanup resumes from its recorded control-call result.
- Eight new integration tests bring the suite to 68 scenarios, including actual
  lifecycle counts, late handoff reconciliation and root-capacity recovery.
- Final GCC 11.4 Debug / ASan+LSan / UBSan / Release: **93/93 CTest PASS each**.
  Release and Debug use warnings as errors. ASan retains leak detection.
- Final Clang 14 TSan required subset: **9/9 PASS** with thread-leak and race
  detection enabled. The previous pipeline thread-leak exemption is removed.
- Final offline Ubuntu 24.04.4 / GCC 13.3: **93/93 CTest PASS**.
- Runtime-only Release with development dependency directories absent: configure
  and build PASS. Formatting, diff and unchanged Stable C ABI checks PASS.
- All existing Release capacity profiles PASS in three-repeat baseline/current
  comparisons; CPU, latency and resource evidence is in
  `docs/development/capacity_qualification.md`.

Full implementation/test report: `docs/development/review_report.md`, final
closure appendix. No new hosted CI run was triggered; the evidence above is
local execution, not an assertion of remote CI status. M0/M1/M2 are PASS;
M3/M4/M5 NOT STARTED. Native arm64 remains DEFERRED by D-034.

## 2026-11-05 M2 Ownership Closure — COMPLETE / local PASS

All six Acquisition ownership/isolation findings and the Quarantine retention
allocation finding are closed. Stable C ABI/public headers remain unchanged.
The generic M2 Runtime is locally qualified and frozen for this milestone;
M3 remains NOT STARTED and requires a separate task.

Final working-tree validation based on `3adefb28`: GCC 11 Debug, ASan/LSan,
UBSan, Release and Ubuntu 24/GCC 13 each **94/94 PASS**; Clang 14 TSan
**10/10 PASS**, with thread-leak detection enabled. Runtime-only Release
without development dependencies **PASS**. All eight existing Release benchmark
executables ran three repetitions and passed their existing gates. Formatter,
whitespace and public-header/ABI diff checks passed.

Historical evidence: local `fe4f938` qualification PASS; subsequent `3adefb28`
[RoDAQ Qualification #41968952350](https://github.com/SiYueY/rodaq/actions/runs/41968952350) **SUCCESS**, independently verified.
Local qualification preceded commit/push; subsequent hosted qualification for
`be448c8` passed at [RoDAQ Qualification #41975605291](https://github.com/SiYueY/rodaq/actions/runs/41975605291). The older `3adefb28` run does not
qualify those changes. Native arm64
remains DEFERRED under D-034. Detailed evidence is in `review_report.md` and
`capacity_qualification.md`. No remaining P0/P1 finding from this closure.

## 2026-11-05 C++ coding standards review — COMPLETE / local PASS

Project-owned C++ functions, including the DAQ Application Candidate and
Plugin SDK authoring hooks, use snake_case. D-056 deliberately supersedes the
SDK source-name freeze in D-033: existing C++ applications and Plugin sources
must migrate and rebuild; C++ Runtime symbols changed. Stable C Plugin ABI
headers, layouts, entry points and frozen V1 binary fixtures remain unchanged.
No legacy PascalCase aliases were added. M3 remains NOT STARTED.

Clang-format 14 and clang-tidy 14 passed for 182 project C/C++ files, 110 build
translation units, 26 public C++ headers, eight C ABI headers in both C/C++
modes and five installed-consumer sources. Two intentionally failing compile-fail
fixtures are verified by CTest instead of being linted as valid translation
units. Seven approved lint checks are automated in CI; documented exceptions are
limited to externally mandated identifiers and targeted moved-from tests.

Final local qualification: Normal, ASan/LSan, UBSan, Release and Ubuntu 24/GCC
13 each **94/94 PASS**; Clang 14 TSan **10/10 PASS** with thread-leak detection;
Runtime-only Release **PASS**; all eight Release capacity executables passed
three repetitions. No known P0/P1 issue was confirmed by this review. See
`review_report.md` and `capacity_qualification.md` for scope and evidence.

Historical `be448c8` hosted qualification: [RoDAQ Qualification #41975605291](https://github.com/SiYueY/rodaq/actions/runs/41975605291) **SUCCESS**.
Current changes are uncommitted and have no new hosted run; **new hosted
qualification is PENDING**. Native arm64 remains DEFERRED under D-034.


## 2026-11-05 Declaration / implementation separation

After the C++ naming review, non-template SDK service, configuration and
validation implementations were moved to `src/plugin/` and linked through the
then-existing static `rodaq_api_support` dependency of `rodaq::sdk`. That support
target was removed by the later Core Structure Closure. Selected internal
Runtime definitions were moved to their corresponding `.cpp` files. Templates,
`constexpr`, Plugin entrypoint/trampoline definitions and public Runtime inline
accessors needed for the existing hidden-visibility linking contract remain in
headers. The Stable C ABI headers and Plugin C exports remain unchanged.

The working tree after this separation passed Normal, Release, ASan/LSan,
UBSan and offline Ubuntu 24: 94/94 CTest each; the Clang 14 TSan CI subset:
10/10 with thread-leak detection; and Runtime-only Release build. Clang-format
and clang-tidy passed for 189 C/C++ files and 117 build translation units,
including public headers and installed consumers. All eight Release capacity
benchmark executables passed three repetitions; see `capacity_qualification.md`.
The full Clang TSan build
was also attempted: an existing null-descriptor fixture fails `-Werror` for an
unused `descriptor`; the specified TSan subset builds and passes. The earlier
qualification and hosted evidence above predate this additional refactoring;
new hosted qualification is PENDING, and M3 remains NOT STARTED.


## 2026-11-05 Error native accessor rename

The C++ `Error::native_code()` accessor and backing member were renamed to
`native()` and `native_`; callers must migrate and rebuild. The current `Error`
member order also differs from commit `ab3b69e`, so C++ consumers must rebuild
against the matching headers and library. Stable C ABI `rodaq_error.native_code`
remains unchanged. A full Debug build and CTest passed 94/94; clang-format and
clang-tidy checks passed for 189 files, 117 build translation units and the
configured header/consumer checks. Hosted qualification for these latest
working-tree changes has not run. M3 remains NOT STARTED.


## 2026-11-05 RoDAQ error-result construction

`rodaq::make_error` forwards errors to the expected-lite failure wrapper;
project callers and the installed consumer use the RoDAQ name. `Result<T>`
remains the D-025 expected-lite alias, and the Stable C ABI is unchanged.
Debug warnings-as-errors build and full CTest passed 94/94, including the
installed consumer and ABI checks; clang-format and clang-tidy passed for 189
files and 117 build translation units. New hosted qualification remains
pending. The earlier `Error::native()` working-tree changes are retained.


## 2026-11-05 Contextual variable names

Renamed selected ambiguous `value` parameters and locals in latency histogram,
timestamp, hashing, SDK configuration size and Runtime service results to names
that express their roles. Generic container and library `value` interfaces are
unchanged. Debug build and full CTest passed 94/94; clang-format and clang-tidy
passed for 189 files and 117 build translation units. Hosted qualification for
the current working tree remains pending.


## 2026-11-05 Direct error construction

Added `make_error` overloads that construct `Error` from a code and message,
with an optional domain and native diagnostic code. Existing Error forwarding
remains supported; direct call sites were migrated. Debug warnings-as-errors
build and full CTest passed 94/94, including installed consumers and ABI checks;
clang-format and clang-tidy passed for 189 files and 117 build translation units.
Hosted qualification for the current working tree remains pending.


## 2026-11-05 C++ StatusCode convergence

C++ `ErrorCode` was replaced by `StatusCode`, including `Ok`; `Error::status_code()`
replaces `code()`. An Error constructed with `Ok` or an unknown status is
normalized to `InternalError` and preserves its diagnostics. Explicit C ABI
mapping covers all status values and preserves unknown / malformed diagnostic
handling. Stable C ABI headers, layout and frozen fixtures are unchanged. C++
applications and Plugin sources must migrate and rebuild; prior local/hosted
qualification records above are historical evidence for earlier trees.

Warnings-as-errors Debug, Release, ASan/LSan and UBSan full CTest: 94/94 each;
Clang 14 TSan CI subset: 10/10 with thread-leak detection; Runtime-only Release
build: PASS; clang-format 14 / clang-tidy 14: PASS for 189 files, 117 build
translation units, public headers and installed consumers. The current working
tree has no hosted qualification; hosted verification is PENDING. M3 remains
NOT STARTED; native arm64 remains D-034 DEFERRED.


## Status + Result migration — COMPLETE / local PASS

The public C++ model is Status for no-value operations and validated Result<T>
for value operations. Error, make_error, the direct expected-lite alias and the
old result accessors are removed from implementation, SDK, consumers and tests.
OK Status has empty diagnostics; unknown codes and OK used as a failed Result
become UnknownError. Result exposes immutable status access and preserves value
copy/move behavior. primary_failure() returns Status; OK means no primary failure.
D-058 supersedes the earlier contracts.

C uses rodaq_status_code and rodaq_status, with status_code / error_code fields
and bounded diagnostics. Current independent fixture sources and layout tests
replace legacy compatibility code; historical evidence remains in Git history.
No source compatibility aliases remain. This migration does not change the
Acquisition ownership, admission, Payload/Persistence, ordering or quarantine
contracts. Expected-lite remains pinned and private to Result storage.

Final qualification of the current working tree:

- Debug / warnings-as-errors, Release, ASan/LSan, UBSan, Ubuntu 24/GCC 13:
  **96/96 CTest PASS each**, including installed SDK/application consumers.
- Clang 14 TSan CI subset: **10/10 PASS**, thread-leak detection enabled.
- Runtime-only Release without development dependencies: **build PASS**.
- Eight existing Release capacity executables: **three repetitions PASS each**;
  medians are recorded in capacity_qualification.md.
- clang-format 14 / clang-tidy 14: **PASS**, covering 191 C/C++ files,
  117 build translation units, 26 public C++ headers, eight C ABI headers in
  C/C++ modes and five installed consumers.
- Namespace/name residue scan, plugin C export review and git diff checks: PASS.

New hosted qualification is **PENDING**; no earlier hosted run qualifies these
uncommitted changes. M3 remains **NOT STARTED**; native arm64 is D-034 DEFERRED.

## 2026-11-06 Direct Status failure storage — COMPLETE / local PASS

Runtime primary_failure() now returns Status directly: OK means no primary
failure. Removed every optional<Status> from project code, including retained
failure history, Descriptor worker/deferred failures, Storage completion effects
and Quarantine/control-call diagnostics. Existing completion flags, Join and
mutex protocols continue to govern asynchronous completion. No compatibility
aliases are retained; callers must migrate and rebuild.

Debug warnings-as-errors, ASan/LSan and UBSan full suites: 96/96 each. Clang 14
TSan subset with thread-leak detection: 10/10. Clang-format/clang-tidy 14:
191 files, 117 TUs, 26 C++ headers, 8 C ABI headers in both languages and
5 install consumers PASS. The updated installed DAQ consumer also passes.
New assertions cover no primary failure across a healthy lifecycle and exact
Quarantine failure diagnostics. The initial sandbox ASan run encountered LSan's
ptrace restriction and was stopped; the complete run outside the sandbox passed
with leak detection enabled.

Release capacity, Runtime-only and Ubuntu 24 were not rerun for this follow-up;
the preceding migration results remain historical evidence. Hosted qualification
is still PENDING. M3 remains NOT STARTED; D-034 remains unchanged.

## 2026-11-06 Result<void> operation migration — COMPLETE / local PASS

No-value operations now return Result<void>, privately backed by
nonstd::expected<void, Status>. Its default construction is success. Constructing
from non-OK Status represents failure; passing OK instead produces UnknownError.
Value Results retain their previous default-failure semantics. Result<Status>
remains prohibited. Status remains the diagnostic object for ABI conversion,
primary failure observations, failure history and deferred diagnostics.

Migrated Runtime, loader, SDK virtuals/trampolines, services, logging, quarantine
callbacks, tests, benchmarks and installed consumers without old signatures or
compatibility aliases. Void operation failure propagation explicitly extracts
status(); implicit void-result-to-value conversion is rejected, including the
bool case that could otherwise swallow Binding failures. Quarantine completion,
Join, admission, ownership and C ABI contracts are unchanged.

Final validation of this working tree:

- Debug / warnings-as-errors, Release, ASan/LSan, UBSan and offline Ubuntu 24:
  **95/95 CTest PASS each**, including installed SDK/Runtime consumers and ABI
  layout, symbol and boundary tests.
- Clang 14 TSan CI subset: **10/10 PASS**, thread-leak detection enabled.
- Runtime-only Release with only the five production dependencies: **build PASS**.
- All eight Release benchmark executables: **three repetitions PASS each**;
  current medians and comparisons are recorded in capacity_qualification.md.
- clang-format/clang-tidy 14: **PASS**, 191 files, 118 translation units,
  26 C++ headers, eight C ABI headers in both languages and five install consumers.
- Final source/signature audit, unchanged C ABI diff/export check and staged /
  unstaged whitespace checks: **PASS**.

CTest count decreased from 96 to 95 because the obsolete Result<void> compile-fail
fixture was removed. Three positive void-result unit cases and independent
header/install checks replace that negative requirement; Result<Status> remains
a compile-fail test. No tests were removed to bypass a failure.

New hosted qualification is **PENDING**. Earlier hosted CI remains historical.
M3 remains NOT STARTED; native arm64 remains D-034 DEFERRED. Existing staged
changes were preserved; this follow-up has not been committed or pushed.
