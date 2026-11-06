# RoDAQ V1 Reference-Driven Implementation Audit

**Status:** Executed for the M2 convergence baseline

**Audit date:** 2026-11-05

**Purpose:** Record source-level references used to challenge RoDAQ-owned
generic mechanisms. A reference is not a substitute for RoDAQ correctness
tests, and no reference project is copied wholesale into the V1 Runtime.

## 1. Method

For each mechanism the implementation review asked:

1. Does a mature project already solve the generic problem?
2. Which exact source mechanism is relevant?
3. Can the mechanism be reused directly without violating RoDAQ's no-drop,
   Stable ABI, bounded-capacity, or lifecycle contracts?
4. If not reusable, which invariant or implementation technique should RoDAQ
   adopt?
5. Which RoDAQ test or benchmark proves the resulting behavior?

The review examined source at pinned tags/commits. Dependency availability is
not recorded as use: a dependency is only `USED` when RoDAQ links and exercises
it on a production or development path.

## 2. Reference Records

### R-001 - ResourceBudget and Admission

- **Reference:** gRPC `v1.60.2`,
  commit `0bab87ede8244a21fce01294a364cc9a7fc99ed8`, Apache-2.0.
- **Reviewed source:**
  `src/core/lib/resource_quota/memory_quota.h`.
- **Mechanism considered:** quota-controlled memory accounting, explicit
  pressure reporting, and staged reclamation.
- **Mechanism adopted:** explicit bounded accounting and rejection/wait
  decisions before committing work. Runtime admission keeps all-or-nothing
  reservation and a final eligibility revalidation.
- **Mechanism rejected:** gRPC's dynamic reclamation passes and pressure
  controller. RoDAQ Accepted Records cannot be dropped after Gate Pass, and its
  reservation dimensions are domain-specific.
- **RoDAQ differences:** no destructive reclamation of accepted Runtime state;
  no adaptive byte quota; per-Stream ordering and Release work are first-class.
- **Evidence:** `admission_test`, `resource_budget_test`, runtime pipeline
  WaitTimeout/ordering tests.

### R-002 - DDS resource limits

- **Reference:** Fast DDS `v2.14.7`,
  commit `f052f239538ebc0df56835fdb57f7628628010bc`, Apache-2.0.
- **Reviewed source:**
  `include/fastdds/dds/core/policy/QosPolicies.hpp`,
  `ResourceLimitsQosPolicy`.
- **Mechanism considered:** explicit maximum samples, instances, samples per
  instance, allocated samples, and extra samples.
- **Mechanism adopted:** capacity is expressed as named dimensions with
  consistency validation rather than one generic "queue length".
- **Mechanism rejected:** DDS history semantics and infinite-value defaults.
  RoDAQ requires concrete bounded capacities for every accepted-work
  dimension.
- **RoDAQ differences:** dispatch, operation, completion, Payload class,
  release-work, quarantine, and activation-root capacities are independently
  owned.
- **Evidence:** `DAQConfig` validation, `ResourceBudget`, Payload class
  exhaustion tests, and capacity benchmarks.

### R-003 - Fixed-size Payload pools with size classes

- **Reference:** Eclipse iceoryx `v2.0.8`,
  commit `35311bfe5338392d65b18f148de70b803f6fc48e`, Apache-2.0.
- **Reviewed source:**
  `iceoryx_posh/include/iceoryx_posh/mepoo/mepoo_config.hpp`,
  `iceoryx_posh/include/iceoryx_posh/internal/mepoo/memory_manager.hpp`.
- **Mechanism considered:** a bounded set of chunk-size classes, each with an
  independent chunk count; no pool for an oversized request; explicit
  out-of-chunks result.
- **Mechanism adopted:** ordered size classes, smallest accommodating class,
  independent per-class budgets, no cross-class borrowing, and distinct
  "no class" / "class exhausted" failure.
- **Mechanism rejected:** iceoryx shared-memory management, chunk headers, and
  process-global mempool configuration. RoDAQ PayloadPool is activation-local.
- **RoDAQ differences:** logical bytes, physical class bytes, per-class HWM,
  and Plugin lifetime guards are additional Runtime contracts.
- **Evidence:** `payload_pool_test`, Runtime large/small coexistence test,
  Payload class benchmark.

### R-004 - Contract-level Payload release ownership

- **Reference:** Fast DDS `v2.14.7`,
  commit `f052f239538ebc0df56835fdb57f7628628010bc`, Apache-2.0.
- **Reviewed source:**
  `src/cpp/rtps/history/BasicPayloadPool_impl/Preallocated.hpp`.
- **Mechanism considered:** Payload storage has an owner separate from
  descriptor/history state, and copy paths explicitly reassign ownership.
- **Mechanism adopted:** Payload ownership is an independent axis with an
  exactly-once release responsibility.
- **Mechanism rejected:** DDS CacheChange and history lifetime model.
  RoDAQ must preserve Plugin callbacks and leases after an operation terminal
  state when release work is still pending.
- **RoDAQ differences:** Dispatch, operation, Payload consumption, Release
  work, and Persistence have separate states.
- **Evidence:** Adopt/Retain tests, early Payload release with pending
  Persistence, blocked release quarantine test.

### R-005 - Structural arena

- **Reference:** Protocol Buffers `v25.3`,
  commit `2fc6cf3fb16b431874a8428b66efe0fda8f26e01`, BSD-3-Clause.
- **Reviewed source:** `src/google/protobuf/arena.h`; LLVM
  `llvmorg-18.1.8`, commit `3b5b5c1ec4a3095ab096dd780e84d7ab81f3d7ff`,
  Apache-2.0 with LLVM exceptions,
  `llvm/include/llvm/Support/Allocator.h`.
- **Mechanism considered:** aggregate allocation, non-individual
  deallocation, bounded initial blocks, explicit total-space accounting, and
  destruction/reset boundaries.
- **Mechanism adopted:** activation-lifetime structural storage and aggregate
  cleanup.
- **Mechanism rejected:** unbounded slab growth and general arbitrary-type
  arena APIs. RoDAQ preallocates a configured maximum and fails closed.
- **RoDAQ differences:** immutable Canonical records, fixed capacity, and no
  general-purpose object escape.
- **Evidence:** `structural_arena_test`, exhaustion paths.

### R-006 - Generation-protected Registry handles

- **Reference:** ROS `class_loader` `2.6.0`,
  commit `7a6f3212d91b74bd62548b7a164d6a1d58054ce4`, BSD-3-Clause; LLVM
  `llvmorg-18.1.8`, commit
  `3b5b5c1ec4a3095ab096dd780e84d7ab81f3d7ff`.
- **Reviewed source:** `include/class_loader/class_loader.hpp`,
  `include/class_loader/class_loader_core.hpp`,
  `llvm/include/llvm/Support/Allocator.h`.
- **Mechanism considered:** stable registration identity, lookup by full
  qualified identity, reference-counted loaded libraries, and rejection of
  use after ownership is gone.
- **Mechanism adopted:** fixed slots plus generation-bearing handles; stale
  handles resolve to no entry. Insertion is release-published after complete
  construction.
- **Mechanism rejected:** global string-key factory maps and process-global
  recursive locks.
- **RoDAQ differences:** numeric Canonical handles, activation generations,
  no concurrent generic Erase, and explicit generation exhaustion.
- **Evidence:** `handle_registry_test`, stale handle pipeline tests, ABI
  compatibility fixture.

### R-007 - Plugin load, reference lifetime, and destruction

- **Reference:** PipeWire `1.0.7`,
  commit `59fe89d41a67f6daad086f8f3fe522935be77a3f`, MIT; ROS
  `class_loader` `2.6.0`, commit
  `7a6f3212d91b74bd62548b7a164d6a1d58054ce4`, BSD-3-Clause.
- **Reviewed source:** `src/pipewire/impl-module.c`,
  `include/class_loader/class_loader.hpp`,
  `include/class_loader/class_loader_core.hpp`.
- **Mechanism considered:** `RTLD_NOW | RTLD_LOCAL`, mandatory entry symbol,
  explicit module destruction, load/unload refcounts, warning when non-pure
  plugin code prevents safe unload, and keeping loaded-library ownership.
- **Mechanism adopted:** hidden plugin-local symbol loading, explicit
  destroy, instance and library leases, no unload while Plugin code or a
  Payload anchor can still run, and process-lifetime quarantine for timeout.
- **Mechanism rejected:** recursive global plugin mutex, implicit unload from
  a smart-pointer deleter, and `dlclose` immediately after a nominal
  deactivation.
- **RoDAQ differences:** stable C ABI, family-specific protocols, Service
  Registry, bounded activation/worker quarantine, and no hot reload.
- **Evidence:** plugin loader/quarantine tests, blocked destroy pipeline test,
  symbol-visibility and installed SDK smoke tests.

### R-008 - Lifecycle quiescence

- **Reference:** PipeWire `1.0.7`,
  commit `59fe89d41a67f6daad086f8f3fe522935be77a3f`, MIT.
- **Reviewed source:** `src/pipewire/impl-module.c` and
  `src/pipewire/impl-node.c`; lifecycle transitions explicitly remove
  runtime work, send Pause/Suspend/Stop commands, and retain an Error/Idle
  distinction.
- **Mechanism considered:** state-specific teardown, separating Idle from
  Error, and failure state observability.
- **Mechanism adopted:** Stop barriers cover submissions, admission, dispatch,
  Payload/Persistence completion, Release, Plugin stop, destroy, logging, and
  activation roots. Unknown/stuck work goes to quarantine.
- **Mechanism rejected:** unbounded join in destructors and treating all
  teardown outcomes as one generic error.
- **RoDAQ differences:** primary/secondary failure history, exact-once
  completion axes, deadline supervision, and no detached callbacks.
- **Evidence:** runtime Stop scenario tests, blocked release/destroy tests,
  logging timeout quarantine test.

### R-009 - Bounded asynchronous logging

- **Reference:** spdlog `v1.17.0`,
  commit `79524ddd08a4ec981b7fea76afd08ee05f83755d`, MIT.
- **Reviewed source:** `include/spdlog/details/thread_pool-inl.h`,
  `include/spdlog/details/circular_q.h`.
- **Mechanism considered:** fixed-capacity queue, explicit
  `discard-new`/overrun policy, overrun counters, worker flush, and graceful
  terminate-message shutdown.
- **Mechanism adopted:** Runtime-owned fixed record/byte queue, drop-new,
  counters, flush/shutdown deadlines, recursion detection, and private
  spdlog/fmt backend.
- **Mechanism rejected:** placing the production contract directly on
  spdlog's queue and relying on blocking/overrun modes. Runtime must retain its
  own byte limit and quarantine semantics.
- **RoDAQ differences:** Plugin routing, byte accounting, direct-reentrancy
  detection, and worker quarantine on shutdown timeout.
- **Evidence:** `logging_context_test`, Runtime Plugin log-routing assertion,
  logging benchmark/stress evidence.

### R-010 - YAML/config trust boundary

- **Reference:** yaml-cpp `0.9.0`,
  commit `56e3bb550c91fd7005566f19c079cb7a503223cf`, MIT.
- **Reviewed source:** `src/nodebuilder.cpp`, `src/node_data.cpp`,
  `include/yaml-cpp/node/detail/node_data.h`; RoDAQ pinned checkout.
- **Mechanism considered:** YAML parsing, node/map representation, scalar
  tags, and duplicate map pairs.
- **Mechanism adopted:** yaml-cpp only as a parser frontend converting into a
  RoDAQ-owned ConfigTree; no YAML type escapes the loader.
- **Mechanism rejected:** assuming yaml-cpp enforces RoDAQ file/node/depth or
  duplicate-key policy before constructing its graph, and exposing
  `YAML::Node` to Plugins or Runtime consumers.
- **RoDAQ differences:** pre-read file-size bound, post-parse node/depth and
  duplicate checks, scalar semantic conversion, and stable Config ABI views.
- **Evidence:** `config_loader_test`, Plugin `ConfigView` tests, installed
  Runtime consumer smoke.

## 3. Dependency Outcome

| Dependency | Audit result |
|---|---|
| expected-lite | Directly reused; no replacement needed |
| HdrHistogram_c | Directly reused behind Runtime metrics |
| spdlog | Reused as private backend under RoDAQ LoggingContext |
| fmt | Required by external-fmt spdlog configuration |
| yaml-cpp | Reused only as ConfigLoader parsing frontend |
| GoogleTest | Development-only |
| Google Benchmark | Development-only |
| oneTBB | Evaluated, no V1 Core requirement, not selected |
| mimalloc | Evaluated, no measured V1 bottleneck, not selected |

## 4. Review Gate

The reference audit does not authorize copying source code. It establishes
why RoDAQ implements or reuses each mechanism and which local contract tests
remain authoritative. Any later replacement of Admission, PayloadPool, slot
state, Registry, StructuralArena, Plugin loading, logging, or Config parsing
must update this record and preserve the linked RoDAQ tests.
