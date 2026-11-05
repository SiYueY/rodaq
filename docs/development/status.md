# RoDAQ Development Status

**Updated:** 2026-11-05
**Branch:** `develop`
**M0 closure baseline:** `30163c8` — `fix: complete M0 closure repair`
**M1 closure implementation baseline:** `eecd752` — `style: format runtime registry test`
**Convergence implementation commit:** `dda407e` — `feat(runtime): converge V1 runtime foundation`

This file records factual current repository state. Keep it concise and update it after meaningful implementation or validation work.

## Current milestone

**M2 — Fake End-to-End Runtime + V1 Runtime Foundation Convergence**

Status: **PASS**

M0 is **PASS**. Stable ABI core prefix is frozen. M0 Final Contract Correction
is **PASS**. Native arm64 qualification is explicitly deferred by D-034.

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

## Current repository state

M0-A Review Closure, M0-B Stable C Plugin ABI, M0-C Plugin Loading + Lifetime,
M0-D Runtime Services + ConfigView and M0-E SDK / Closure Repair are complete
and validated. M0 Final Contract Correction is also complete. The repository now
contains:

- a C++17 CMake project foundation,
- warning, sanitizer, CTest, dependency and install support,
- Public `rodaq::Error` and `rodaq::Result<T>` primitives,
- eight Stable C ABI headers under `include/rodaq/plugin/abi/`, with
  `common.h` as the shared primitive header,
- C11/C++17 ABI compilation, layout, minimum-prefix and symbol qualification,
- a private Linux Plugin loader with Boot ABI, family, descriptor and provenance validation,
- private DSO and instance lease ownership with bounded normal unload behavior,
- catalog/manager orchestration and explicit quarantine retention,
- pinned dependency submodules with no normal-build network access,
- installed-header validation for `rodaq::Error`, `rodaq::Result<T>` and the
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
  ResourceBudget/Reservation, AdmissionGate, RuntimeRecord,
  AcceptanceSequenceAllocator, RuntimeStats, HdrHistogram latency wrapper and a
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
expected-lite                  PINNED+BUILT+LINKED+USED+QUALIFIED
HdrHistogram_c                 PINNED+BUILT+LINKED+USED+QUALIFIED
spdlog/fmt                     PINNED+BUILT+LINKED+USED+QUALIFIED
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
- Public `RuntimeConfig/ResourceConfig/PluginConfig/StorageConfig`, installed
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
  `Runtime::Start()` return `ResourceExhausted`. No `detach()` or lost
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

1. Yes. RuntimeRecord uses `BindingHandle` as its sole Binding/Descriptor anchor.
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
  including installed Runtime/Plugin consumers and Runtime DSO closure checks.
- Previously recorded Clang 14 TSan under `setarch x86_64 -R` (not accepted
  as final evidence: pipeline disabled thread-leak reporting): admission, PayloadPool,
  observability, logging, Registry, quarantine, StoragePipeline, M1 integration
  and complete Runtime pipeline: **9/9 CTest passed**.
- Normal Runtime build with tests/benchmarks, oneTBB, mimalloc, GoogleTest and
  Google Benchmark removed from the source copy: configure and build passed.
- `clang-format 14 --dry-run --Werror` passed for changed/new C/C++ files;
  `git diff --check` passed; source search found no `detach`,
  `recursive_mutex`, or Runtime thread `.release()`.
- Runtime DSO dynamic exports are limited to `rodaq::runtime::Runtime`; private
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
