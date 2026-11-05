# RoDAQ Storage Plugin Design Specification

**Version:** v0.5
**Status:** Plugin ABI Reconciliation Candidate
**Scope:** RoDAQ V1 Storage Plugin Architecture, Projection, Handoff, Persistence and Finalization Contract

**Normative Dependencies:**

- RoDAQ Canonical Data Model Design Specification v0.9
- RoDAQ V1 Canonical Profile v0.6
- RoDAQ System Architecture Design Specification v0.6
- RoDAQ Runtime Design Specification v0.6
- RoDAQ Payload Design Specification v0.5
- RoDAQ Admission Design Specification v0.5
- RoDAQ Acquisition Plugin Design Specification v0.7
- RoDAQ V1 Plugin System Design — current Freeze Candidate

---

# 1. 目标与设计边界

## 1.1 设计目标

Storage Plugin 是 RoDAQ Runtime 与具体持久化 backend 之间的适配边界。

其职责是：

> **把 Runtime 已经建立的 Canonical State、Descriptor Graph 和 Accepted RuntimeRecord 精确投影到具体 Storage backend，并向 Runtime 明确报告 Payload consumption、Record persistence、Flush、Close 和 failure outcome。**

基本路径为：

```text
Canonical Registry
        ↓
Descriptor Projection
        ↓
Storage Descriptor Readiness
        ↓
Runtime Binding Active
        ↓

Accepted RuntimeRecord
        ↓
Runtime Storage Dispatch
        ↓
Storage Plugin
        │
        ├── backend projection
        ├── batching
        ├── payload consumption
        ├── persistence
        └── finalization
        ↓
Persistent Artifact
```

Storage Plugin 必须保证：

- 不重新定义 Canonical semantics；
- 不分配 Canonical ID；
- 不修改 immutable Canonical Descriptor；
- 不根据 backend convenience 静默丢弃 Canonical information；
- Descriptor 在引用它的 Record 之前完成 Storage readiness；
- 同一 Stream 的 Storage logical Record order 与 AcceptanceSequence order 一致；
- Payload 只在 Runtime 授予的 access lease 内读取；
- `PayloadConsumed`、`PersistenceSucceeded` 和 crash durability 明确分离；
- Storage internal buffering / batching 有界；
- Handoff / Descriptor readiness / Flush / Close / failure quiescence 都具有 bounded progress；
- backend failure 不把已经 Accepted 的 RuntimeRecord 改写为 Admission rejection；
- full-fidelity 无法保证时必须拒绝，而不是 silent degradation；
- Artifact 必须能够显式区分 normal complete 与 partial / incomplete 状态；
- normal Close success 只在输出 Artifact 完整、可解释且满足 full-fidelity contract 时返回。

---

## 1.2 V1 目标 Storage Plugin

V1 Storage Contract 的 **required implementation** 为：

```text
MCAP Storage
```

V1 正式 recording artifact 采用 ROS2-compatible MCAP，并要求同时支持：

```text
ros2 bag play
PlotJuggler
Foxglove
RoDAQ full-fidelity decode
```

SQLite3 继续作为 **future / designed Storage target** 保留完整 mapping 文档，用于证明 generic Storage Contract 没有被 MCAP container model 反向塑形，但不进入 V1 implementation / release gate。

同时不能依赖这些 backend 的具体 API 或数据结构。

Runtime generic boundary 不得要求：

```text
mcap::Writer
mcap::Schema
mcap::Channel

sqlite3*
sqlite3_stmt*
```

等 backend-specific object。

未来可以增加：

```text
SQLite3
Parquet
HDF5
MDF4
custom binary format
remote object storage
database backend
streaming archival backend
```

而不改变 Canonical Model 和 Runtime 基础 contract。

---

## 1.2.1 Typed Interoperability Projection Consumption

Storage Plugin 除了查询 Canonical Descriptor Graph，还可以通过 Runtime 的：

```text
rodaq.runtime.projection.v1
```

查询与当前 StreamDescriptor 关联的 versioned typed InteropProjection。

规则：

```text
Canonical Descriptor
    = full-fidelity authority

InteropProjection
    = external ecosystem projection input
    = non-Canonical
    = immutable / bounded / versioned
```

Storage 不得：

```text
直接调用 Acquisition Plugin
根据 Plugin concrete type 分支
把 projection 当作 Canonical identity
在 projection 与 Canonical Descriptor 冲突时静默选择其中一个
```

Storage Descriptor readiness 必须把所需 projection 作为 capability/precondition 检查的一部分。例如 V1 MCAP Storage 对正式 ROS2-compatible Artifact 要求：

```text
rodaq.projection.ros2.v1
```

存在且完整；否则该 Descriptor 为 Unsupported。

未来 Storage 可以支持其它 projection identifier，或者完全不需要任何 projection。这一机制不能演变为任意 GenericMetadata / PropertyBag。

---

## 1.3 Storage Plugin 与 Runtime 的责任边界

Storage Plugin 负责：

```text
backend lifecycle

Artifact creation

Canonical-to-backend projection

Descriptor readiness

Record handoff

Payload read access

backend batching

backend write / transaction

PayloadConsumed reporting

Persistence outcome reporting

Flush

Artifact finalization

Close

backend failure detection

Storage-side quiescence
```

Runtime 负责：

```text
Canonical ID allocation

Canonical Registry

Descriptor graph correctness

Binding lifecycle

Admission

AcceptanceSequence

RuntimeRecord construction

Payload lifetime ownership

Storage Dispatch

Storage Completion Domain

Stop / failure coordination

Plugin callback target lifetime
```

因此：

> **Runtime 提供已经完成 Canonical interpretation 的数据；Storage Plugin 只负责精确投影和持久化。**

---

## 1.4 Storage Projection ≠ Canonical Model

Storage Plugin 可以为了 backend 需要建立：

```text
backend channel id
table row id
surrogate key
batch id
chunk id
transaction id
```

这些都是：

> **Storage Projection state**

不是 Canonical identity。

例如：

```text
StreamDescriptorId = 17
```

可以映射到：

```text
backend_channel_id = 3
```

只要 Artifact 仍能准确恢复：

```text
Canonical StreamDescriptorId = 17
```

及其 identity relationships。

Storage Plugin 不得因为 backend 已经有自己的 ID：

> 把 backend ID 重新解释为 Canonical ID。

---

## 1.5 Record Acceptance 与 Storage Persistence

必须严格区分：

```text
Admission Accepted
```

与：

```text
Storage Persistence
```

Record 在进入 Storage Plugin 之前：

> 已经是 Accepted RuntimeRecord。

因此：

```text
Storage failure
```

不能产生：

```text
AdmissionRejected
```

也不能：

```text
rollback AcceptanceSequence
```

Storage 只能针对已经取得 Storage responsibility 的 Record 报告：

```text
PayloadConsumed

PersistenceSucceeded

PersistenceFailed

PersistenceUnknown
```

以及 Plugin-wide：

```text
FatalStorageFailure
```

---

## 1.6 V1 Storage Topology

V1 一个 Runtime activation 使用：

```text
exactly one
active Storage Plugin instance
```

该 instance 对应一个逻辑：

```text
Artifact
```

V1 Running 期间不支持：

```text
replace Storage Plugin

hot-switch backend

add second active Storage Plugin

remove active Storage Plugin
```

Storage Plugin 内部可以：

```text
split physical files

create chunks

create SQLite tables

use sidecar files

use backend-specific segments
```

只要这些 physical objects 共同构成：

> 一个明确的 logical Artifact。

具体 physical layout：

> 由 backend mapping 定义。

---

## 1.7 Storage Failure 的作用域

V1 active Storage Plugin 是：

> Runtime-wide persistence path。

因此 fatal Storage Plugin failure：

```text
Storage → Failed
```

是：

> **Runtime-wide failure condition。**

Runtime 必须：

```text
close Admission Gate
        ↓
stop creating
new Admission Attempts
        ↓
allow healthy
already Gate-passed attempts
to finish Admission
        ↓
enter Runtime
failure shutdown
```

Storage failure：

> 不回写 Canonical admission history。

---

# 2. Storage 生命周期、能力与 Runtime Integration

## 2.1 Storage Operational Lifecycle

Storage Plugin 的逻辑 lifecycle 为：

```text
Configured
    ↓
Prepared
    ↓
Ready
    ↓
Closing
    ↓
Closed
```

异常 normal-operation terminal state：

```text
Failed
```

这些是 contract state。

不要求 implementation 使用完全相同的 enum。

### 2.1.1 Stable Plugin ABI Lifecycle Mapping

上述：

```text
Configured
Prepared
Ready
Closing
Closed
Failed
```

是 Storage 的领域运行状态，不是跨 DSO ABI method 列表。

V1 Stable Storage ABI 冻结：

```text
create
bind
configure
open
submit
flush
close
query_interface
destroy
```

映射关系：

```text
ABI create
    → 构造 Plugin-local instance

ABI bind
    → 通过 Service Registry 获取 Runtime Services

ABI configure
    → 建立本文 Configured state

ABI open
    → 封装 Prepare / backend open / capability validation
    → 成功后进入能够处理 Descriptor / Record 的 Ready/Open operational state

ABI submit
    → 对应本文 HandoffRecord domain operation

ABI flush
    → 对应本文 persistence barrier

ABI close
    → 对应 Closing → Closed，并完成 callback / Payload-access quiescence

ABI destroy
    → 只在 Closed 或未 Open 的安全状态调用
```

因此本文中的：

```text
Prepare
EnsureDescriptorReady
HandoffRecord
Deliver Completion
Quiesce after Failure
```

属于领域 capability / Runtime coordination step，**不是**要求继续向 `rodaq_storage_plugin` core vtable 增加同名 ABI slot。

V1 correctness 所必需的 capability negotiation 与 Descriptor readiness 必须通过 required interface：

```text
rodaq.storage.control.v1
```

暴露。

其中：

```text
get_capabilities()
ensure_descriptor_ready()
```

是 V1 required protocol；缺失属于 protocol incompatibility。

只有 rotation / durability 等非核心能力才是 optional Storage Extension。

---

## 2.2 Configured

Configured 表示：

> Storage backend configuration 已完成静态解析和基本验证。

配置可以包括：

```text
output target

backend options

compression

batching

transaction size

buffer limits

flush policy

close timeout

durability options

backend-specific extension policy
```

Configured 阶段：

> 不要求 Artifact 已创建。

---

## 2.3 Prepared

Runtime Starting 期间执行 Storage Prepare。

Prepare 负责：

```text
open / create backend

establish Artifact staging state

initialize Artifact
finalization status

validate backend capabilities

validate resource limits

establish internal workers

establish completion mechanism
```

Prepared 成功后：

> Storage Plugin 已具备处理 Descriptor projection 的能力。

### 2.3.1 Activation-Abort Cleanup

Storage Prepare 可以已经：

```text
create / open backend output
establish Artifact staging state
start internal workers
```

但 Runtime Activation Commit 仍可能尚未发生。

如果 Runtime 在 `Starting` 阶段 abort activation，Storage Plugin 必须支持：

> **prepared-output abort / cleanup semantics。**

逻辑要求为：

```text
Runtime activation abort
        ↓
no normal Record handoff can begin
        ↓
settle / cancel pending Descriptor readiness work
        ↓
stop Storage workers / backend work safely
        ↓
close / release backend resources
        ↓
Artifact remains Incomplete / Aborted
        ↓
never publish NormalComplete
```

本文不冻结 API 名称，不要求实现新增 `Aborting` enum 或独立 manager。

一个实现可以使：

```text
Prepared
    → cleanup operation
    → Closed
```

但该 `Closed` 只表示 Storage execution/resources 已安全终止，**不等于 normal `CloseSuccess`**。

Activation-abort cleanup：

- 不产生 `NormalComplete` marker；
- 不把尚未成功 activation 的空/部分 Artifact 伪装为正常 conformant output；
- 可以保留 backend-specific incomplete artifact 供诊断 / 后续离线工具识别；
- 必须具有 bounded progress / explicit failure semantics。

如果 abort cleanup 无法建立 no-future-access / safe backend termination：

> Runtime 按 failure disposition 进入 `Failed`，并根据 memory-safety contract retain / quarantine unsafe state。

---

## 2.4 Storage Capability Contract

Prepare 时 Storage Plugin 必须向 Runtime 建立确定的 V1 capability contract。

至少包括：

```text
FullFidelity = supported

maximum accepted Payload size

bounded internal record capacity

bounded internal byte capacity

Payload consumption mode

required progress deadlines /
progress guarantees
```

其中：

```text
maximum accepted Payload size
```

可以表示：

> Storage 不比 Runtime 自身配置的 limit 更严格。

具体 ABI structure 由 `docs/design/rodaq_plugin_system.md` 冻结为：

```text
rodaq_storage_capabilities
rodaq.storage.control.v1
```

Runtime 必须在 Running / Binding Active 前取得并冻结 effective capability。

---

### 2.4.1 Effective Payload Limit

如果 Storage backend 的单 Record Payload 上限低于 Runtime configured limit，

Runtime 必须在 Running 前建立：

```text
effective_payload_limit
=
min(
    runtime_payload_limit,
    storage_payload_limit
)
```

并且：

> **Runtime v0.5 MUST 在 Starting 阶段冻结该 activation 的 effective limit，Admission v0.5 MUST 在 Gate pass 前使用它。**

因此：

```text
payload_size
>
effective_payload_limit
```

必须在 Gate pass 前处理。

正常 Accepted RuntimeRecord：

> 不允许在 Storage handoff 时第一次因为普通 Payload size limit 被拒绝。

Storage Plugin 可以假设：

```text
normal conforming Handoff
⇒
payload_size
<=
effective_payload_limit
```

---

### 2.4.2 Effective-limit Invariant Violation

如果 Storage Plugin 实际收到：

```text
Accepted RuntimeRecord
with
payload_size
>
effective_payload_limit
```

这不是：

```text
ordinary Storage capacity pressure
```

也不是：

```text
normal per-Record rejection
```

而是：

> **Runtime ↔ Storage Contract invariant violation。**

由于该 Record 已经 Admission Accepted，

Storage 不能：

```text
drop
reject and continue normally
```

因此 Storage MUST：

```text
report ContractInvariantViolation
        ↓
fail closed
        ↓
FatalFailure
```

Runtime：

> 进入 failure shutdown。

这里的 FatalFailure：

> 表示系统 contract 已破坏，不意味着根因一定是 Storage backend 自身故障。

Runtime 不得把：

```text
configuration mismatch
oversize accepted Record
```

重新解释成 ordinary Storage backpressure。

---

### 2.4.3 Capability Stability

进入：

```text
Storage Ready
```

以后，

会影响已接受 Record 合法性的 capability：

```text
maximum Payload size
full-fidelity support
required representation limits
```

不得在当前 activation 中变得更严格而仍保持 Ready。

如果 backend runtime state 发生变化，使原 capability 不再成立：

> Storage 必须进入 Failed。

不得：

```text
shrink limit
and reject previously valid Records
```

---

### 2.4.4 Full-Fidelity Capability

RoDAQ V1 conforming Storage Plugin：

> 必须支持本文定义的 full-fidelity projection。

如果 backend 原生格式无法表示某种 Canonical information，

Storage Plugin 可以：

```text
use backend extension

use metadata table

use sidecar structure

use explicit canonical
mapping layer
```

实现 full fidelity。

如果仍无法表示：

> 必须拒绝对应 configuration / Descriptor。

不得：

```text
silently drop field

silently merge identity

silently normalize
unknown identifier
```

---

## 2.5 Ready

Storage Plugin 进入 Ready 表示：

```text
backend operational

Artifact writable

completion path operational

Storage can accept
Descriptor readiness requests

Storage can accept
Record handoff
for ready descriptors
```

Storage Ready：

> 是 Runtime 打开 Admission Gate 的必要条件之一。

---

## 2.6 Runtime Running

Runtime Running 期间 Storage Plugin：

```text
accepts dynamic
Descriptor projection

accepts Record handoff

performs batching /
persistence

reports completions
```

Descriptor Control Plane operation：

> 可以与已有 Stream 的 Record persistence 并发。

Storage Plugin 必须正确协调 backend thread-safety。

---

## 2.7 Closing

Closing 表示：

> Storage Plugin 不再接受新的 normal Record handoff / Descriptor registration，并正在完成 final Flush / backend finalization。

正常情况下 Runtime 在进入 Closing 前保证：

```text
no future RuntimeRecord handoff
```

Storage Close：

> 不是用来解决持续到达 Record 的 backpressure mechanism。

---

## 2.8 Closed

Closed 时必须保证：

```text
no Storage worker
will access Runtime Payload

no Storage worker
will emit normal completion

backend resources closed

Artifact finalization result fixed
```

必须区分进入 `Closed` 的 disposition：

```text
normal finalization
    → CloseSuccess
    → Artifact NormalComplete

Starting activation-abort cleanup
    → cleanup/abort success
    → Artifact remains Incomplete / Aborted
    → not normal CloseSuccess
```

因此：

> **normal `CloseSuccess` 才要求 Artifact 满足 §7 的 Normal Complete Artifact contract；resource-closed 本身不等价于 NormalComplete。**

---

## 2.9 Failed

Failed 表示：

> Storage Plugin 无法继续可靠执行 normal persistence。

进入 Failed 后：

```text
no new Descriptor readiness
becomes Ready

no new Record handoff
is normally accepted

no recovery to Ready
within same Runtime activation
```

V1：

> 不支持 active Storage Plugin 在同一 activation 中 `Failed → Ready`。

因为：

```text
Artifact continuity

Persistence uncertainty

Descriptor ordering

Record outcome
```

可能已经无法可靠证明。

Storage Failed 后仍必须支持：

```text
failure reporting

outstanding completion resolution

Payload-access quiescence

best-effort backend cleanup
```

直到 Runtime 能安全完成 failure shutdown。

---

## 2.10 Runtime Lifecycle Mapping

正常生命周期：

```text
Runtime Starting
        ↓
Storage Configure / Prepare
        ↓
Storage Ready
        ↓
Runtime Running
        ↓
Descriptor + Record operations
        ↓
Runtime Stopping
        ↓
no future Record handoff
        ↓
Storage Flush
        ↓
resolve persistence outcomes
        ↓
Storage Close
        ↓
Storage Closed
        ↓
Runtime Stopped
```

Starting activation abort：

```text
Runtime Starting
        ↓
Storage Prepared / maybe Ready
        ↓
Activation Commit does not occur
        ↓
Runtime requests prepared-output abort / cleanup
        ↓
no NormalComplete publication
        ↓
Storage resources Closed
Artifact remains Incomplete / Aborted
        ↓
Runtime finishes activation abort
```

该路径不是 normal Runtime Stop 后的 Storage Close success。

---

fatal Storage failure：

```text
Storage fatal failure
        ↓
Storage Failed
        ↓
Runtime closes Admission Gate
        ↓
Runtime failure shutdown
        ↓
resolve outstanding
Storage responsibility
        ↓
establish Storage quiescence
        ↓
best-effort cleanup
        ↓
Runtime Failed
```

---

# 3. Descriptor Projection 与 Readiness

## 3.1 Descriptor-before-Record

V1 必须满足：

> **Storage Plugin 在接受引用某个 StreamDescriptor 的 Record 之前，必须已经完成该 StreamDescriptor 及其依赖 graph 的 Storage readiness。**

逻辑关系：

```text
Canonical Descriptor Graph
        ↓
Storage Projection
        ↓
Storage Descriptor Ready
        ↓
Runtime Binding Active
        ↓
Record handoff allowed
```

不得：

```text
Record handoff
        ↓
later discover
descriptor cannot be represented
```

---

## 3.2 Descriptor Graph

Storage projection 的 graph 至少可能包含：

```text
Stream identity

StreamDescriptor

optional SchemaDescriptor

TimestampKeySet

ClockDescriptors
```

Runtime 提供的 Descriptor graph：

> 必须已经 Canonically referentially closed。

Storage Plugin：

> 不负责修复非法 Canonical graph。

---

## 3.3 Descriptor Readiness Operation

Runtime 通过 required Storage control interface 调用：

```text
rodaq.storage.control.v1::ensure_descriptor_ready(...)
```

该 API 是 V1 Stable Plugin Contract 的组成部分。

operation outcome 至少区分：

```text
Ready

Unsupported

Failed
```

`Ready` and `Unsupported` carry `failure.code == OK`. `Failed` carries a non-OK
`rodaq_error` value with the backend domain, native code and message. The
function-level `out_error` remains reserved for ABI/control-call failure.

Readiness is evaluated against the current immutable Descriptor and immutable
projection set. `Unsupported` caused by a missing projection leaves the
Descriptor published but non-routable; Runtime retries after a new projection
registration. A missing projection is not permanent Descriptor failure.

---

### 3.3.1 Ready

`Ready` 表示：

> Storage Plugin 已建立该 Descriptor graph 的完整 backend projection，并保证后续引用它的 Record 不会超越现有 backend representation。

它不意味着：

```text
fsync completed
crash durability established
Artifact Closed
```

---

### 3.3.2 Unsupported

`Unsupported` 表示：

> Canonical Descriptor 本身合法，但当前 Storage backend 无法以 full-fidelity 方式表示。

例如：

```text
unsupported identifier length

unsupported exact
schema representation

unsupported required
full-fidelity extension
```

此时：

```text
Binding must not
become Active
```

但：

> Storage Plugin 本身不必进入 Failed。

其他 ready Stream：

> 可以继续持久化。

如果 Descriptor 对应：

```text
required startup Binding
```

Runtime 可以：

> 使 activation 失败。

如果是动态 Stream：

> 该 Stream registration / activation 失败，其他 Stream继续。

---

### 3.3.3 Failed

`Failed` 表示：

> Descriptor readiness 因 backend I/O、internal corruption 或 Storage correctness failure 无法可靠完成。

这属于：

```text
fatal Storage failure
```

Storage：

```text
→ Failed
```

Runtime：

```text
→ Runtime-wide fatal failure
→ automatic Control Plane failure shutdown
```

This applies to both initial readiness while Starting and dynamic readiness
while Running; it is independent of whether the Descriptor is startup-critical.
Backend failure diagnostics remain the Primary Failure. A readiness deadline
expiry, malformed ABI output, or unexpected readiness enum also fails closed.
`UNSUPPORTED` remains Stream-local and retryable after Projection registration;
it does not imply fatal Storage failure. Runtime lifecycle ownership and worker
shutdown follow `runtime.md §11.6.2–§11.6.3`.

---

## 3.4 Descriptor Readiness Bounded Progress

Descriptor readiness：

> 必须具有 bounded progress condition。

Runtime 必须最终获得：

```text
Ready
Unsupported
Failed
TimedOut
```

之一。

如果：

```text
TimedOut
```

且无法确定只是 Descriptor-specific unsupported，

则：

> 按 fatal Storage failure 处理。

不得无限等待。

---

## 3.5 Descriptor Identity

Storage projection 必须保持：

```text
same Canonical DescriptorId
⇒
same immutable descriptor
```

如果 Runtime 提供：

```text
same DescriptorId
different content
```

这是：

```text
Runtime / Registry
invariant violation
```

Storage MUST：

> fail closed。

不得自行选择一个版本继续。

---

## 3.6 Content Equality 不等于 Identity Equality

两个 Descriptor：

```text
Descriptor A
Descriptor B
```

即使 content 完全相同，

如果：

```text
A.Id != B.Id
```

Storage 不得在 full-fidelity projection 中：

> 无映射地把它们合并为同一 identity。

Backend 可以 physical deduplicate，

但必须保留：

```text
A identity
B identity
```

并能精确重建。

---

## 3.7 Schema Projection

SchemaDescriptor 必须保留：

```text
SchemaId
SchemaFormat
optional TypeIdentifier
exact representation bytes
```

Storage Plugin 不得：

```text
parse schema
        ↓
regenerate equivalent schema
        ↓
discard original bytes
```

然后仍声明 full fidelity。

backend 可以额外保存解析结果，

但：

> exact original representation bytes 必须可恢复。

---

## 3.8 Clock Projection

ClockDescriptor 必须保留：

```text
ClockId

resolution numerator

resolution denominator

ClockOrigin
```

不得：

```text
convert all clocks
to one backend time base
and discard ClockId
```

Storage 可以额外保存：

```text
converted convenience timestamp
```

但 original canonical clock reading：

> 必须保留。

---

## 3.9 TimestampKeySet Projection

Storage 必须保留：

```text
TimestampKeySetId

RoleIdentifier

ClockId
```

完整关系。

未知或未来 RoleIdentifier：

> 不能因为 Storage Plugin 不认识而丢弃。

---

## 3.10 Descriptor Projection 与 Backend Native IDs

Backend 可以建立：

```text
Canonical SchemaId
    ↔
backend schema id

Canonical StreamDescriptorId
    ↔
backend channel / row id
```

但 mapping 必须：

```text
stable for Artifact lifetime

unambiguous

full-fidelity reversible
```

---

## 3.11 Dynamic Descriptor Readiness

Runtime Running 期间可以出现新 Descriptor。

已有 Stream persistence：

> 不应因为 unrelated Descriptor readiness 长时间停止。

Storage SHOULD：

> 尽可能把 descriptor control work 与 unrelated data persistence 解耦。

如果 backend 本身要求 serialization：

> Plugin 可以内部串行处理。

但仍必须满足 bounded progress。

---

## 3.12 Ready Irreversibility

一旦某 Descriptor：

```text
Storage Ready
```

在 Storage Plugin 保持 Ready 的前提下：

> readiness 不应回退。

如果 backend 状态损坏导致 readiness 不能继续保证：

> Storage Plugin 必须进入 Failed。

不得：

```text
silently deactivate descriptor
while continuing with
inconsistent Artifact state
```

---

# 4. Record Handoff、Payload Lease 与 Ordering

## 4.1 Record Handoff Preconditions

Runtime 只允许 Record handoff，当：

```text
Storage == Ready

Record == Admission Accepted

referenced Descriptor
== Storage Ready

RuntimeRecord fully initialized

payload_size
<=
effective_payload_limit
```

并且：

```text
same-Stream Runtime
dispatch order
==
AcceptanceSequence order
```

---

## 4.2 Storage Record View

Storage Plugin 接收：

> RuntimeRecord 的 storage-facing immutable projection。

逻辑上至少包含：

```text
Stream / StreamDescriptor identity

AcceptanceSequence

optional Source identity

optional SourceSequence

TimestampStateMap

PayloadEncoding context

PayloadView
```

implementation 可以使用：

```text
cached descriptor handle

compiled slot mapping

internal immutable
projection handle
```

避免每 Record 重建 Canonical map。

但 Storage 最终看到的语义：

> 必须等价于 Canonical Record。

---

## 4.3 Runtime Internal Handle 不得成为 Artifact Semantics

Runtime 内部：

```text
BindingHandle
SourceHandle
DescriptorHandle
```

可以用于高效 handoff。

Storage 不得直接把：

```text
raw Runtime handle value
```

当成持久化 Canonical identity。

必须解析到：

```text
Canonical identity
```

或：

```text
Artifact-defined
reversible mapping
```

---

## 4.4 Handoff Outcome

本文中的 `HandoffRecord` 是领域语义；Stable Plugin ABI 对应：

```text
rodaq_storage_plugin::submit()
```

ABI 映射冻结为：

```text
Plugin-internal HandoffCommit
    → Plugin 已不可逆决定 submit() 将返回 OK + matching PENDING
    → 相应 Pending axis 可以发布给 async worker
    → 该时点不对外建立 HandoffAccepted

submit() function return == OK
    → confirms HandoffAccepted
    → out_result 同步给出 Payload axis / Persistence axis 当前状态

Runtime reconcile
    → 合并 return 与可能已到达的 early completion event
    → 之后才执行可观察 terminalization / resource reclamation

submit() function return != OK
    → HandoffAccepted 从未成立
    → FatalFailure
    → return 后不得保留 Runtime Payload access
    → 不得发送该 operation 的正常 completion callback
```

因此 normal Record handoff 的 immediate domain outcome 为：

```text
HandoffAccepted
```

或者：

```text
FatalFailure
```

函数级 non-OK 不表示普通 backpressure。Record 在 Runtime 中已经 Accepted，因此 Runtime 必须进入 Storage failure path。

V1 不提供：

```text
Drop
BestEffortReject
QueueFullReject
OversizeRejectAndContinue
```

作为已经 Accepted RuntimeRecord 的 normal Storage outcome。

---

### 4.4.1 HandoffAccepted

表示：

> Storage Plugin 已取得该 Record 的 Storage-processing responsibility。

`HandoffAccepted` 同时是 V1 对 Runtime-provided PayloadView 的 asynchronous-retention authorization boundary。

在该 boundary 之后，Storage 才允许把 PayloadView / 其读取责任保留到 Handoff 调用返回之后，并必须遵守 §4.7 的 Payload Access Lease。

从这一点开始，Storage 必须最终解决：

```text
Payload axis
```

和：

```text
Persistence axis
```

---

### 4.4.2 FatalFailure

表示：

> Storage 无法取得或继续该 Record 的可靠 persistence responsibility，或者 Runtime ↔ Storage Contract 已经破坏到无法继续 normal path。

Storage：

```text
→ Failed
```

Runtime：

> 进入 failure shutdown。

Record：

> 不重新变成 AdmissionRejected。

如果 Handoff 调用明确返回 `FatalFailure`，且从未到达 `HandoffAccepted`：

> **Storage 不得在 return 之后保留 Runtime-provided PayloadView 或任何异步访问该 Payload 的 task/reference。**

也就是说 synchronous call-scoped Payload access 必须在 `FatalFailure` return 前结束。

如果 Runtime 观察到的是 timeout / no-progress，而不是一个已经返回的 `FatalFailure`：

> timeout 本身不证明 Storage execution context 已经停止访问 Payload；Runtime 必须按 Storage failure quiescence contract 建立 no-future-access guarantee 后才能释放 Payload。

---

### 4.4.3 Contract-invariant Handoff

如果 Storage 收到违反已冻结 capability 的 Record，例如：

```text
payload_size
>
effective_payload_limit
```

则属于：

```text
ContractInvariantViolation
```

Storage MUST：

```text
fail closed
→ FatalFailure
```

不得：

```text
treat as ordinary pressure
drop
or
continue Ready
```

原因不是：

> Storage 可以随意把 Runtime bug 变成 backend error，

而是：

> 该 Record 已经 Accepted，系统没有合法的 ordinary rejection path 可以再把它移除。

---

## 4.5 Ordinary Capacity Pressure

Storage internal queue / worker capacity 暂时不足时：

> 不得 drop Accepted RuntimeRecord。

Storage 可以：

```text
bounded-block Handoff
```

直到：

```text
capacity available
```

或者：

```text
Handoff progress deadline
exceeded
```

如果 deadline 超过：

> fatal Storage failure。

完整 backpressure chain：

```text
Storage slows
    ↓
Runtime Dispatch slows
    ↓
Runtime dispatch capacity
becomes scarce
    ↓
Admission reservation
unavailable
    ↓
Acquisition sees
Reject / Wait
```

---

## 4.6 Handoff Bounded Progress

Record handoff：

> 不得无限等待 backend。

Storage Contract 必须保证：

```text
HandoffAccepted
within bounded progress
```

或者：

```text
FatalFailure / TimedOut
```

timeout：

> 不允许静默 drop Record 后继续 Ready。

如果 timeout 是 Runtime-side progress deadline 到期，而 Storage Handoff execution 仍可能在另一个 thread / task 中运行：

> Runtime 必须把 Storage 视为 failure-quiescence 未完成，不能因为 timeout 就立即回收 Payload。

---

## 4.7 Payload Access Lease

Runtime 调用 Record Handoff 时会提供只读：

```text
PayloadView
```

Handoff call 执行期间，Storage 可以进行 synchronous call-scoped read。

只有当 Handoff 达到：

```text
HandoffAccepted
```

以后，才建立跨 Handoff return 的：

> **Payload Access Lease**

因此：

```text
Handoff entry
    → synchronous call-scoped access only

HandoffAccepted
    → Storage responsibility established
    → asynchronous Payload Access Lease may continue

FatalFailure before HandoffAccepted
    → no retained access after return
```

对于已经 `HandoffAccepted` 的 Record，从该 boundary 开始直到：

```text
PayloadConsumed
```

为止，Storage 可以：

```text
read Payload bytes

copy Payload

compress Payload

perform backend encoding
```

不得：

```text
modify Runtime Payload bytes
```

也不得在 Plugin 已作出“`submit()` 必然返回 OK + 对应 axis 为 PENDING”的 irreversible handoff decision 之前，把 Runtime-provided PayloadView 发布给会跨 Handoff return 执行的 worker / deferred task。

---

## 4.8 PayloadConsumed

`PayloadConsumed` 表示：

> Storage Plugin 已永久结束对 Runtime-provided Payload bytes 的所有访问。

之后：

```text
no Storage thread

no Storage worker

no deferred task

no compression task

no backend callback
```

可以再次 dereference 旧 PayloadView。

因此 Runtime：

> 可以释放对应 Payload backing。

任何后续访问：

> 属于 Storage Contract violation / UAF risk。

---

## 4.9 Synchronous Payload Consumption

Storage 可以声明：

```text
successful Handoff return
==
PayloadConsumed
```

如果声明：

> Handoff 成功返回就是 Payload lease 的 terminal event。

此时：

- Runtime 不等待显式 PayloadConsumed callback；
- Storage 不得再次发送 PayloadConsumed；
- Handoff return 后不得读取原 PayloadView。

---

## 4.10 Asynchronous Payload Consumption

如果 Storage 未声明 synchronous consumption，

则：

```text
HandoffAccepted
```

后 Runtime 必须保持 Payload alive，

直到 Storage 显式报告：

```text
PayloadConsumed
```

Storage 可以：

```text
retain PayloadView

use async worker

copy later

write later
```

但所有原 Payload access：

> 必须在 PayloadConsumed 前结束。

---

## 4.11 Storage-private Copy

Storage 可以：

```text
copy Runtime Payload
into Storage-owned buffer
```

然后：

```text
PayloadConsumed
```

后续：

```text
compression
batching
backend write
```

使用 private copy。

因此：

```text
PayloadConsumed
```

可以早于：

```text
PersistenceSucceeded
```

Storage-private copy：

> 必须计入 Storage 自身 bounded resource budget。

不能通过提前 PayloadConsumed：

> 建立无界 private buffering。

---

## 4.12 PayloadConsumed 与 Persistence 独立

合法场景：

### Consumption first

```text
Handoff
    ↓
copy Payload
    ↓
PayloadConsumed
    ↓
backend write
    ↓
PersistenceSucceeded
```

### Together

```text
Handoff
    ↓
synchronous backend work
    ↓
PayloadConsumed
PersistenceSucceeded
```

### Persistence terminal first

如果 Storage 仍有合法原因访问 Runtime Payload，

可以出现：

```text
Persistence terminal
while
Payload lease remains active
```

但：

> Persistence outcome 不能替代 PayloadConsumed。

---

## 4.13 Same-stream Ordering

Storage 必须保证：

> **Artifact 可观察 / 可恢复的 same-Stream logical Record order 等于 AcceptanceSequence order。**

例如：

```text
seq 10
seq 11
seq 12
```

不得在任何声明为 ordered 的 backend representation 中变成：

```text
10
12
11
```

对于没有定义 physical row order 的 backend，例如关系数据库：

> 不要求依赖 physical row order 表达该语义。

Artifact 必须显式保存：

```text
AcceptanceSequence
```

并保证 Storage 的 logical query / reconstruction semantics：

```text
same Stream
ORDER BY AcceptanceSequence
```

可以无歧义恢复 canonical same-stream order。

---

### 4.13.1 Internal Parallelism

Storage 可以内部：

```text
parallel compression

parallel serialization

parallel checksum

parallel statement preparation
```

但不得产生与 AcceptanceSequence 冲突的 same-stream logical representation。

如果某 backend 的 order-sensitive publication 可能乱序：

> Storage 必须自行 serialize / reorder。

不得要求 Runtime：

> 事后修复 Storage 内部产生的乱序。

---

## 4.14 Cross-stream Ordering

不同 Stream：

> 不定义 Canonical total order。

Storage 可以任意 interleave 不同 Stream 的 backend work。

如果 backend physical format 存在全局写入顺序：

> 该顺序只是 Storage-specific artifact order。

不得自动解释为：

```text
Canonical global
acquisition order
```

V1 不引入：

```text
GlobalSequence
```

---

## 4.15 AcceptanceSequence Gaps

AcceptanceSequence：

> 允许 gaps。

例如：

```text
10
12
13
```

不自动表示：

```text
Storage corruption
```

因为：

```text
11
```

可能属于一个消耗 sequence 的：

```text
AdmissionRejected
```

Storage 不得因为 sequence 不连续而拒绝合法 Record。

---

## 4.16 Invalid Sequence Order

如果 order-sensitive handoff 中 Storage 收到同一 Stream：

```text
10
9
```

或者：

```text
10
10
```

则违反 Runtime → Storage ordering invariant。

Storage SHOULD：

```text
report ContractInvariantViolation
        ↓
fail closed
```

不得：

```text
silently fix duplicate
silently reorder regression
```

然后继续声明 Artifact conformant。

---

# 5. Completion、Batching、Backpressure 与 Resource

## 5.1 Storage Completion Axes

Runtime 对每个：

```text
HandoffAccepted Record
```

独立跟踪两个 axis。

### Payload axis

```text
PayloadPending
    ↓
PayloadConsumed
```

Stable ABI 对应：

```text
submit return:
    RODAQ_STORAGE_PAYLOAD_CONSUMED
    or
    RODAQ_STORAGE_PAYLOAD_PENDING

async terminal notification:
    rodaq.runtime.storage.v1::payload_consumed(operation)
```

如果 submit result 已经返回：

```text
PAYLOAD_CONSUMED
```

则不得再发送 callback。

如果返回：

```text
PAYLOAD_PENDING
```

则必须 exactly-once terminalize `payload_consumed()`。异步 worker callback 可以与 `submit()` return 竞态，但不得 same-stack synchronous reentrant；Runtime 在 return/reconcile 前只记录 early event，不提前释放当前 submit 调用仍可能使用的 Payload backing。

### Persistence axis

```text
PersistencePending
    ↓
PersistenceSucceeded
or
PersistenceFailed
or
PersistenceUnknown
```

Stable ABI 对应：

```text
submit return:
    RODAQ_STORAGE_PERSISTENCE_PENDING
    RODAQ_STORAGE_PERSISTENCE_SUCCEEDED
    RODAQ_STORAGE_PERSISTENCE_FAILED
    RODAQ_STORAGE_PERSISTENCE_UNKNOWN

async terminal notification:
    rodaq.runtime.storage.v1::persistence_completed(...)
```

如果 submit result 已经同步返回 terminal persistence state：

> 不得再发送该 axis 的 callback。

如果返回 `PERSISTENCE_PENDING`：

> 必须 exactly-once terminalize 为 Succeeded / Failed / Unknown。异步 notification 可以与 `submit()` return 竞态，但不得 same-stack synchronous reentrant；Runtime 统一在 return/reconcile 后提交可观察 terminal state。

两个 axis：

> **严格独立。**

因此以下状态完全合法，而且对大 Payload 场景非常重要：

```text
PayloadConsumed
+
PersistencePending
```

Runtime 此时可以释放大 Payload backing，只保留轻量 persistence bookkeeping。

---

## 5.2 PersistenceSucceeded

`PersistenceSucceeded` 表示：

> Storage Plugin 已完成该 Record 所需的 record-specific backend persistence work，并且在正常后续 Artifact finalization 成功的前提下，该 Record 将属于 Artifact。

它意味着：

```text
no future record-specific
backend operation
can make this Record
fail persistence
```

但不意味着：

```text
CrashDurable
```

也不意味着：

```text
Artifact normal Close
already succeeded
```

---

### 5.2.1 Monotonicity

一旦 Storage 报告：

```text
PersistenceSucceeded
```

不得随后改为：

```text
PersistenceFailed
PersistenceUnknown
```

因此 Plugin：

> 不能过早报告 success。

如果 Record 仍依赖：

```text
future transaction commit

future batch commit

future chunk publication
```

才能确定 persistence，

则继续保持：

```text
PersistencePending
```

---

## 5.3 PersistenceFailed

`PersistenceFailed` 表示：

> Storage 可以确定该 Record 没有按照 full-fidelity persistence contract 成功进入 Artifact。

例如：

```text
transaction rollback known

backend write failure known

representation failure
after Handoff
```

对于已经 HandoffAccepted 的 V1 Record：

```text
PersistenceFailed
⇒
fatal Storage failure
```

---

## 5.4 PersistenceUnknown

`PersistenceUnknown` 表示：

> Storage 无法确定该 Record 是否已经成功进入 Artifact。

例如：

```text
backend failure
after partial write

transaction outcome
indeterminate

backend state lost
```

`PersistenceUnknown`：

> 不是 success。

V1：

```text
PersistenceUnknown
⇒
fatal Storage failure
```

Runtime：

> 不得推断成 Succeeded。

也不得在 backend 事实上可能已经持久化时：

> 为方便 bookkeeping 强行改成 Failed。

---

## 5.5 Completion Exactly Once

每个 HandoffAccepted Record：

### Payload axis

恰好 terminalize 一次：

```text
PayloadConsumed
```

### Persistence axis

恰好 terminalize 一次：

```text
Succeeded
Failed
Unknown
```

禁止：

```text
duplicate PayloadConsumed

Succeeded → Failed

Failed → Succeeded

multiple persistence terminals
```

---

## 5.5.1 Handoff Decision / Return / Reconcile Linearization

V1 明确区分三个时点：

```text
1. HandoffCommit (Plugin-internal irreversible decision)
   Plugin 已决定 submit() 必然返回 OK，且相应 axis 为 PENDING
   → 从此可以向 async worker 发布 operation/Payload lease
   → 尚未对 ABI caller 建立 HandoffAccepted

2. submit() return
   ABI 对 handoff decision 和同步 axis state 的 confirmation

3. Runtime reconcile
   合并 synchronous submit result 与可能已经到达的 early completion event
   → 才执行可观察 terminalization / resource release
```

HandoffAccepted 仍由时点 2 的 `submit() return == OK` 对外建立。

在时点 1 之前：

```text
no async publication
no cross-return Payload access
```

在时点 1 到 3 之间：

```text
early worker completion may be recorded
but must not reclaim resources still usable by current submit call
```

如果最终函数级 return non-OK，而 Runtime 已观察到 async event，则属于 contract violation。

---

## 5.6 Immediate Completion

Stable ABI 的 `rodaq_storage_submit_result` 可以在 `submit()` return 中同步携带：

```text
PayloadConsumed

PersistenceSucceeded
PersistenceFailed
PersistenceUnknown
```

等 terminal state。

如果某 axis 已通过 return 表达 terminal：

> **不得再发送对应 callback。**

如果某 axis 返回 Pending：

> Plugin 不得在当前 `submit()` 调用栈内同步 reentrant 调用对应 callback。

跨线程 worker completion 可以与 `submit()` return 发生 race；Runtime 必须在调用 Storage 前预建 operation tracking，并在 return 后 reconcile 已到达的 completion event 与 submit result。

---

## 5.7 Completion Notification

Stable ABI 的异步通知分为：

```text
payload_consumed(operation)

persistence_completed(
    operation,
    terminal_state,
    error)
```

`error` is borrowed for the callback duration and is a complete
`rodaq_error` value. On `SUCCEEDED` it is `NULL`; on `FAILED` or `UNKNOWN` it
is non-NULL with a non-OK status. Runtime must copy the value before retaining
it.

而不是一个把 Payload 与 Persistence 混在一起的通用 `complete()`。

异步 completion 可以来自：

```text
Storage worker

backend callback

batch commit thread
```

Runtime 通过：

```text
Storage Completion Domain
```

串行处理 Runtime state mutation。

Storage worker 可以并行，

但 completion event 必须：

```text
well-formed

exactly-once

associated with
one valid outstanding Record
```

---

## 5.8 Completion Delivery 与 Outcome Determination

必须区分：

```text
Storage internal
outcome determination
```

和：

```text
Runtime observing
the completion notification
```

例如：

```text
backend transaction commits
        ↓
Storage internally knows:
PersistenceSucceeded
        ↓
completion event queued
        ↓
Runtime Completion Domain
later processes event
```

因此：

> **Persistence outcome 已经确定，不意味着 Runtime 已经观察到该 completion。**

这一规则对：

```text
Flush
Close
failure barrier
```

尤其重要。

---

## 5.9 Completion Callback Lifetime

Storage 只能在 Runtime 提供的：

```text
valid completion
callback lifetime
```

内发送 completion。

Runtime failure cleanup：

> 在 Storage quiescence 建立前不会提前销毁 callback target。

Storage Quiesced / Closed 后：

> 不得再发送 normal completion。

---

## 5.10 Completion Callback 行为

completion callback SHOULD：

```text
be short

not perform blocking
Storage I/O

not recursively invoke
Storage operations

not throw across
Runtime boundary
```

Plugin internal exception：

> 转换为 Storage failure reporting。

---

## 5.11 Batching

Storage 可以使用：

```text
record batch
transaction
chunk
page
group commit
```

这些都是：

> backend implementation detail。

不是 Canonical object。

---

## 5.12 Batch 与 Record Semantics

一个 batch 可以包含：

```text
multiple Records
multiple Streams
```

但每个 Record：

> 仍有独立 Payload / Persistence terminal state。

Storage 不得只报告：

```text
BatchSucceeded
```

而让 Runtime 无法确定每个 Record outcome。

可以由：

```text
one batch completion
```

逻辑展开为多个：

```text
per-Record completion
```

---

## 5.13 Successful Batch

如果 transaction / batch 成功，

可以一次性将其中所有 Record 标记为：

```text
PersistenceSucceeded
```

前提：

> backend semantics 能证明每条 Record 都已经完成其 record-specific persistence obligation。

---

## 5.14 Failed Batch

如果 batch 完整 rollback 且可证明：

```text
no affected Record persisted
```

则：

```text
PersistenceFailed
```

如果 failure 后：

```text
exact persisted subset
cannot be proven
```

则无法证明的 Record：

```text
PersistenceUnknown
```

不得为了简化 bookkeeping：

> 把 Unknown 改成 Failed 或 Succeeded。

---

## 5.15 Same-stream Batch Ordering

batching 不得改变：

```text
same Stream
AcceptanceSequence order
```

batch boundary：

> 不是 Canonical ordering boundary。

---

## 5.16 Storage Internal Resource Bounds

Storage 必须有界管理：

```text
pending Record objects

Storage-private Payload copies

batch buffers

compression buffers

backend staging buffers

completion events

pending Descriptor projection
```

至少应存在逻辑上的：

```text
max_outstanding_records

max_outstanding_bytes

max_batch_records

max_batch_bytes
```

或等价约束。

具体配置名：

> 不在本文冻结。

---

## 5.17 Aggregate Bound

不能仅保证：

```text
each queue individually bounded
```

同时允许：

```text
aggregate Storage-owned
memory effectively unbounded
```

Storage 必须具有：

> aggregate Storage-owned memory/resource bound。

---

## 5.18 Backpressure Chain

正常 Storage backpressure：

```text
backend slower
        ↓
Storage pending capacity
consumed
        ↓
Handoff slows
        ↓
Runtime dispatch
capacity consumed
        ↓
Admission capacity
unavailable
        ↓
Acquisition
Reject / Wait
```

Storage：

> 不直接控制 Acquisition Plugin。

---

## 5.19 No Accepted-record Drop Policy

V1 Storage 不允许正常配置：

```text
drop oldest
drop newest
drop random
best effort discard
```

来处理已经 Accepted 的 RuntimeRecord。

如果 Storage 无法继续：

> fail closed。

---

# 6. Flush、Close、Failure 与 Bounded Progress

## 6.1 Flush 的目的

`Flush` 建立：

> 对 flush barrier 之前已经 HandoffAccepted 的 Record 的 normal-path persistence barrier。

正常 Runtime Stop：

```text
close Admission Gate
        ↓
quiesce Acquisition
        ↓
resolve pre-cutoff Admission
        ↓
handoff all Accepted Records
        ↓
no future Storage handoff
        ↓
Storage Flush
        ↓
resolve persistence
completions
        ↓
Storage Close
```

---

## 6.2 Flush Barrier

Flush invocation 必须具有明确 barrier：

```text
all Record handoff
logically before Flush
```

属于该 flush set。

如果 Running 中支持显式 Flush：

> barrier 后的新 Record 属于后续 generation。

具体 generation representation：

> 不要求成为公共 API object。

---

## 6.3 Flush Success

`FlushSuccess` 必须保证：

1. 所有 flush-set Record 的 required backend write / transaction obligation 已完成；
2. 每个 flush-set Record 的 persistence outcome 已经在 Storage 内部确定；
3. 不再存在属于该 flush set 的未执行 batch / transaction；
4. backend state 已达到继续运行或 Close 所要求的一致状态。

因此 FlushSuccess 后，

Storage internal state 中不得仍存在：

```text
flush-set Record
whose persistence outcome
is genuinely undetermined
```

---

### 6.3.1 FlushSuccess 不要求 Completion 已到达 Runtime

必须严格区分：

```text
persistence outcome
determined by Storage
```

与：

```text
completion observed
by Runtime
```

例如：

```text
Record handoff
        ↓
PersistencePending
        ↓
Flush
        ↓
backend commit succeeds
        ↓
Storage internally determines:
PersistenceSucceeded
        ↓
FlushSuccess returns
        ↓
completion callback
later reaches Runtime
```

这是合法的。

因此：

> **FlushSuccess 只保证 flush-set Record 的 persistence outcome 已经在 Storage 内部确定，不保证所有 asynchronous completion notification 已经被 Runtime Completion Domain 处理。**

Runtime：

> 在 Flush 返回后仍必须继续处理 completion，直到所有 flush-set Record 的 terminal persistence outcome 都已经在 Runtime state 中可见。

如果 Storage implementation 选择：

> 在 Flush return 中同步表达所有 completion，

同样合法。

但不能要求所有 backend：

> 必须在 Flush return 前等待 Runtime callback delivery 完成。

---

## 6.4 Flush 与 PayloadConsumed

normal Stop 的 final Flush success：

> SHOULD 同时使 flush-set Record 的 Payload lease 都能够 boundedly terminalize。

正常情况：

```text
final Flush
        ↓
Persistence outcome fixed
        ↓
PayloadConsumed
        ↓
no Runtime Payload
access remains
```

如果特殊 backend 出现：

```text
Persistence terminal
but
Payload lease still active
```

Runtime：

> 不得提前 release Payload 或进入 final Close teardown。

Storage 必须继续使 Payload lease：

> boundedly reach `PayloadConsumed` 或建立 failure quiescence。

---

## 6.5 Flush ≠ Crash Durability

普通：

```text
FlushSuccess
```

不自动意味着：

```text
fsync complete

storage device persistence

power-loss durability

atomic Artifact publication
```

因此：

```text
FlushSuccess
≠
CrashDurable
```

如果 backend 支持：

```text
DurableFlush
```

可以作为 backend-specific capability。

不得把 durable semantics：

> 隐式混入 generic Flush。

---

## 6.6 Flush Failure

如果 Flush 因：

```text
backend error

timeout

transaction failure

outcome uncertainty
```

无法完成，

Storage：

```text
→ Failed
```

Runtime：

```text
→ failure shutdown
```

不得：

> 忽略 Flush failure 后继续返回 normal CloseSuccess。

---

## 6.7 Close

Close 表示：

> Storage 正在完成 Artifact 最终 backend finalization。

可能包括：

```text
final transaction

final chunk

index

summary

metadata tables

backend footer

schema/channel finalization

finalization-status marker

backend close
```

具体机制由 backend mapping 定义。

---

## 6.8 Close Preconditions

normal Close 前：

```text
no future Record handoff

no future Descriptor registration

all Accepted Records
that can reach Storage
have been handed off

final Flush completed
```

Storage SHOULD：

> 对违反 precondition 的调用 fail closed。

但 Close 仍必须防御：

> 不能因为 Runtime 漏调某一步就错误返回 success。

---

## 6.9 Close Success

`CloseSuccess` 必须保证：

```text
all Descriptor projections valid

all HandoffAccepted Records
have terminal persistence outcome

all required HandoffAccepted Records
PersistenceSucceeded

all Payload leases terminated

backend finalization successful

Artifact structurally complete

Artifact self-consistent

Artifact decodable

Artifact full-fidelity conformant

Artifact finalization status
indicates NormalComplete
```

只有同时满足：

> 才允许 CloseSuccess。

---

### 6.9.1 Completion Delivery at Close

CloseSuccess 要求：

> Storage 内部已经不存在 unresolved persistence outcome。

但是如果 asynchronous completion notification 已经确定并正在 Runtime Completion Domain 中传递，

Runtime 在把整个 Runtime transition 到：

```text
Stopped
```

以前：

> 仍必须等待这些 completion 被 Runtime state 观察。

因此：

```text
Storage CloseSuccess
```

与：

```text
Runtime has processed
every completion event
```

不是同一个瞬间。

Runtime final Stop barrier：

> 负责闭合后者。

---

## 6.10 Close Failure

如果：

```text
one Record Failed

one Record Unknown

final index failed

footer failed

final status marker failed

backend close failed

finalization timed out
```

则：

```text
CloseSuccess
```

不得返回。

结果：

```text
CloseFailed
```

或等价 failure。

Runtime：

> 最终 Failed。

---

## 6.11 Artifact Finalization Status

Storage Plugin MUST 使 Artifact 的：

```text
finalization status
```

能够被外部读取工具判定。

至少需要区分：

```text
Incomplete
NormalComplete
```

可选地 backend mapping 可以进一步细分：

```text
Failed
RecoverablePartial
CrashInterrupted
```

但 generic V1 只要求能够明确判定：

```text
NormalComplete
```

与：

```text
not NormalComplete
```

---

### 6.11.1 初始状态

Artifact 创建时：

> 不得默认标记为 `NormalComplete`。

其状态逻辑上必须是：

```text
Incomplete
```

或者：

> complete marker absent。

只有 finalization 成功的最后阶段：

> 才允许建立 `NormalComplete` 状态。

---

### 6.11.2 Backend Representation

finalization status 可以通过：

```text
backend-native metadata

footer

summary record

dedicated metadata table

sidecar file

completion marker
```

表达。

具体机制：

> 由 backend mapping 定义。

例如 mapping 必须说明：

- 什么状态表示 incomplete；
- 什么状态表示 normal complete；
- 下游 reader 如何检查；
- close failure 时如何确保不会错误声称 complete。

---

### 6.11.3 External Detectability

下游工具至少必须能够：

> 根据 Artifact 本身或 mapping 规定的组成部分判断该 Artifact 是否完成 normal Close。

不得依赖：

```text
the recording process
is still alive

operator memory

external log only
```

才能知道 Artifact 是否完整。

如果某个普通第三方工具：

> 不理解 RoDAQ-specific finalization metadata，

它可能仍然能够打开部分 backend 数据。

但 RoDAQ conforming reader / validation tool：

> 必须检查 finalization status，不能把 incomplete Artifact 报告为 normal conformant Artifact。

---

### 6.11.4 Finalization Status ≠ Crash Durability

`NormalComplete` 表示：

> normal Close contract 成功。

它不自动表示：

```text
power-loss durable

atomic under crash

fsync complete
```

这些仍属于独立 durability contract。

---

## 6.12 Partial Artifact

Close failure 后 backend 可能留下：

```text
partial file

partial database

recoverable prefix

backend-specific staging Artifact
```

这些可以用于：

```text
diagnostics

recovery tooling

manual inspection
```

但是必须：

```text
finalization status
!=
NormalComplete
```

不得把它标记为：

```text
normal conformant
complete Artifact
```

具体 crash recovery / partial Artifact promotion：

> 由后续 `persistence.md` 或 backend mapping 定义。

---

## 6.13 Fatal Storage Failure

Storage 在 Running 中发生 fatal error：

```text
Storage → Failed
```

并向 Runtime 报告：

```text
FatalStorageFailure
```

Runtime：

```text
closes Admission Gate
```

但已经 Gate-pass 的 healthy Admission Attempt：

> 仍按 Admission v0.5 完成 Accepted。

因此 Runtime dispatch path 中可能仍存在：

```text
Accepted
but not yet
HandoffAccepted
```

的 Record。

这些 Record：

> 不得重新解释为 Admission rejection。

Runtime v0.5 必须保留独立 Dispatch axis：

```text
Queued
HandoffAccepted
```

以便 failure cleanup 能区分 Storage responsibility 是否已经建立。

---

### 6.13.1 Accepted but Not Yet Handed-off

必须区分：

```text
Accepted by Admission
```

与：

```text
HandoffAccepted by Storage
```

只有完成：

```text
HandoffAccepted
```

Storage responsibility 才建立。

因此：

> **Accepted 但从未进入 `HandoffAccepted` 的 Record 不属于 Storage Plugin responsibility。**

Storage：

- 不需要为这些 Record 产生 PayloadConsumed；
- 不需要为这些 Record 产生 persistence completion；
- 不应在 failure cleanup 中假装自己持有这些 Record。

它们由：

> Runtime failure path

进行 terminal persistence classification。

---

### 6.13.2 Runtime Classification of Never-handed-off Records

如果 Runtime 可以确定：

```text
Record was never
HandoffAccepted
```

则该 Record：

> 明确没有通过当前 Storage Plugin 持久化。

因此其 persistence outcome SHOULD 为：

```text
PersistenceFailed
```

而不是：

```text
PersistenceUnknown
```

如果 Runtime 自身发生 synchronization / handoff-boundary failure，使它不能确定：

```text
whether HandoffAccepted happened
```

则：

```text
PersistenceUnknown
```

才是保守结果。

因此：

```text
known never-handed-off
    → Failed

handoff boundary uncertain
    → Unknown
```

这部分：

> 属于 Runtime failure contract，而不是 Storage Plugin completion responsibility。

---

Failure cutoff prohibits new normal submit entry. An already executing submit
retains its original ownership and reconciles its actual return before cleanup;
Runtime never calls close/destroy concurrently with an unreconciled submit.
Queued Accepted Records do not become Storage obligations. See
`runtime.md §11.6.3` for the shared cleanup/continuation algorithm.

## 6.14 Outstanding HandoffAccepted Records on Failure

对于已经：

```text
HandoffAccepted
```

但尚未 terminal 的 Record，

Storage failure cleanup 必须尽可能解决：

### Payload axis

```text
PayloadConsumed
```

或者：

> 保持 Payload lease，直到 quiescence 能证明所有访问结束。

### Persistence axis

必须最终分类：

```text
Succeeded
Failed
Unknown
```

不得永久保持：

```text
Pending
```

---

## 6.15 Failure Persistence Classification

Storage failure 时：

### Definitely persisted

如果 backend contract 可以证明 Record 已经完成 persistence commit point：

```text
PersistenceSucceeded
```

### Definitely not persisted

如果能够证明：

```text
transaction rolled back

write never occurred
```

则：

```text
PersistenceFailed
```

### Cannot prove

则：

```text
PersistenceUnknown
```

禁止：

```text
Unknown → guess Success
```

也禁止：

```text
possibly persisted
→ blindly mark Failed
```

---

## 6.16 Storage Failure Quiescence

fatal failure：

> 不自动意味着 Payload access 已结束。

Storage 必须建立：

```text
Storage Quiesced
```

保证：

```text
no Storage worker
can access Runtime Payload

no backend callback
can access Runtime Payload

no deferred task
can access Runtime Payload

no future normal completion
will be emitted
after quiescence barrier
```

---

## 6.17 Failure Quiescence Bounded Progress

Storage quiescence：

> 必须具有 bounded progress。

可以通过：

```text
worker cancellation

worker join

backend cancellation

task drain
```

建立。

如果 timeout：

> Runtime 不得为了 cleanup convenience 提前释放仍可能被访问的 state。

必须：

```text
retain / quarantine
```

相关：

```text
Payload
callback target
Storage-visible Runtime state
```

memory safety：

> 优先于 cleanup completeness。

---

## 6.18 Operation Progress Bounds

以下 operation 都必须存在 bounded progress：

```text
Descriptor readiness

Record Handoff

Flush

Close / finalization

failure quiescence
```

timeout：

> 必须产生确定 failure semantic。

不得：

```text
wait forever
```

也不得：

```text
timeout
then silently continue Ready
```

---

## 6.19 Failed State Cleanup

Storage Failed 后允许：

```text
resolve known completions

quiesce workers

best-effort backend close

release safe backend resources
```

但不允许：

```text
accept new normal Record

mark new Descriptor Ready

reopen same Artifact
and resume persistence
```

V1 recovery：

> 需要新的 Runtime activation / Storage instance / Artifact lifecycle。

---

# 7. Full Fidelity、Artifact 与实现约束

## 7.1 Full-Fidelity Definition

RoDAQ V1 conforming Storage 必须使 Artifact 准确保留：

```text
Canonical identity relationships

StreamId

StreamDescriptorId

StreamName

PayloadEncoding

SchemaId presence / identity

SchemaFormat

TypeIdentifier
presence / value

exact schema
representation bytes

ClockId

Clock resolution

Clock origin

TimestampKeySetId

Timestamp RoleIdentifier

TimestampKey → ClockId

per-Record
Present / Absent state

ClockReading exact value

SourceId presence

SourceId exact opaque bytes

SourceSequence
presence / value

AcceptanceSequence

Payload logical bytes
```

以及：

> 解释这些值所需的 Scope / Descriptor relationships。

---

## 7.2 Presence 必须保留

必须区别：

```text
SourceId absent
```

与：

```text
SourceId present
```

同样：

```text
SourceSequence absent
```

与：

```text
SourceSequence present(0)
```

必须不同。

Timestamp：

```text
Absent
```

与：

```text
Present(0)
```

必须不同。

Storage backend：

> 不得使用一个 numeric zero 同时代表 absent 和 present-zero。

### 7.2.1 V1 Scalar Domain Fidelity

Storage 必须完整保留 Canonical Profile v0.6 的 scalar domains：

```text
Runtime-allocated Canonical IDs
    uint64, 0 valid

AcceptanceSequence
    uint64, 0 valid

SourceSequence when present
    uint64, 0 valid

ClockReading
    int64 signed tick value

Clock resolution numerator / denominator
    positive uint64 reduced rational
```

因此 backend projection：

- 不得用 `0` 作为 ID / sequence absent sentinel；
- 不得把 signed ClockReading 强制投影为 unsigned；
- 不得因 backend column / field 较窄而 truncate；
- 不得把 `UINT64_MAX` 作为通用 SourceSequence absent sentinel；
- 必须保留 resolution numerator / denominator 的 exact value。

如果 backend 原生 scalar type 无法表达完整 V1 domain，Storage Mapping 必须：

```text
use reversible extension / encoding
or
reject unsupported configuration / descriptor
```

不得 silent narrowing。

---

## 7.3 Unknown Identifier

Storage 不得要求自己认识：

```text
all future RoleIdentifier

all PayloadEncoding identifiers

all SchemaFormat identifiers
```

只要 Canonical representation 合法，

full-fidelity Storage SHOULD：

> opaque preserve 未知 identifier。

如果 backend mapping 无法做到：

```text
Descriptor Unsupported
```

不得：

```text
unknown
→ empty
→ default
→ drop
```

---

## 7.4 Canonical ID Mapping

Backend 可以使用 surrogate ID。

但 Artifact 必须存在：

```text
Canonical ID
↔
backend representation
```

的无歧义 mapping。

因此：

```text
Canonical ID 10
Canonical ID 11
```

即使 content 相同，

也不能因为 backend dedup：

> 丢失两者 identity distinction。

---

## 7.5 Payload Exactness

Storage 持久化 Payload：

> 必须恢复成完全相同的 logical byte string。

允许：

```text
compression
chunking
page splitting
database BLOB encoding
```

前提：

```text
decode
→ exact original bytes
```

默认不得：

```text
deserialize
→ re-serialize
```

替代 exact bytes。

---

## 7.6 Schema Exactness

Schema full fidelity：

> 要求 exact representation bytes。

可以额外保存：

```text
parsed schema
normalized schema
type cache
```

但 original bytes：

> 必须仍然存在或 exact-restorable。

---

## 7.7 Timestamp Exactness

Storage 不得只保存：

```text
converted wall time
```

然后丢弃：

```text
ClockId
raw ClockReading
RoleIdentifier
Present / Absent
```

可以增加 convenience timestamp，

但 Canonical timestamp：

> 必须保存。

---

## 7.8 Source Identity Exactness

SourceId：

> 是 opaque bytes。

Storage 不得：

```text
interpret as UTF-8

trim zeros

change byte order

normalize text and
discard original
```

除非 representation：

> 能 exact restore original bytes。

---

## 7.9 AcceptanceSequence

Storage 必须显式持久化：

```text
AcceptanceSequence
```

不得仅依赖：

```text
physical row order
file offset
message order
```

隐式表示。

原因是：

```text
cross-stream order
not Canonical

sequence gaps legal

backend physical order
may vary
```

---

## 7.10 Artifact

Artifact 是：

> 一个 Runtime activation 的逻辑持久化输出。

可以是：

```text
one MCAP file

one SQLite database

one directory

one main file + sidecar

backend-specific file set
```

mapping 必须明确：

```text
what constitutes
one Artifact
```

以及：

```text
how finalization status
is determined
```

---

## 7.11 Artifact Scope

一个 V1 Artifact 对应：

> 一个 active Runtime Identity Scope。

V1 不在同一个 Artifact 中执行：

```text
generic cross-scope merge
```

Storage：

> 不自行跨 activation 合并 Scope。

---

## 7.12 Artifact Completeness

Normal Close success 的 Artifact 必须：

```text
self-consistent

descriptor-complete

record-decodable

full-fidelity

NormalComplete

free of unresolved
persistence state
```

其中：

> **self-consistent**

具体表示：

- 所有 Record 引用的 StreamDescriptor 都存在并可解析；
- 所有 StreamDescriptor 引用的 Schema、TimestampKeySet 都存在；
- 所有 TimestampKeySet 引用的 ClockId 都存在；
- 所有 Canonical ID mapping 无悬空引用；
- backend-specific index / summary / footer 等 normal finalization structure 已完整建立；
- Artifact finalization status 与实际 Close outcome 一致；
- 不存在 mutually contradictory projection metadata；
- 如果当前 backend/config 声明了 external interoperability contract，则所有 required typed projection 均已验证并成功投影。

self-consistent：

> 不意味着 crash durable。

---

## 7.13 Finalization Metadata

Storage MUST 使：

```text
Artifact finalization status
```

成为 Artifact 可判定的一部分。

它可以位于：

```text
main backend metadata

metadata table

footer

summary

sidecar

completion marker
```

但：

> 不能只存在于 Runtime process memory 或 log 中。

Backend mapping 必须定义：

```text
how to mark Incomplete

how to mark NormalComplete

how a reader checks status
```

正常创建 Artifact：

```text
starts as Incomplete
```

只有 successful finalization：

```text
→ NormalComplete
```

---

## 7.14 Backend-native Metadata

Storage 可以增加：

```text
compression statistics

backend version

chunk size

transaction info

creation time

storage diagnostics
```

这些 metadata：

> 不得与 Canonical semantics 混淆。

reader 必须能够区分：

```text
Canonical information

Storage-specific information
```

---

## 7.15 Explicit Lossy Mode

未来 backend 可能支持：

```text
Lossy
SemanticOnly
```

模式。

但：

> **RoDAQ V1 normal recording 不使用这些模式。**

V1 activation：

```text
requires FullFidelity
```

backend 如果只能 lossy：

> activation / Descriptor readiness 必须失败。

不得 silent degradation。

---

## 7.16 Crash Durability

本文冻结：

```text
normal persistence semantics
```

与：

```text
normal Close completeness
```

以下内容：

```text
fsync policy

power-loss guarantee

atomic rename

journal recovery

partial Artifact recovery

manifest protocol

crash consistency
```

如果需要跨 backend 统一 contract：

> 由后续 `persistence.md` 定义。

Backend mapping 可以定义 backend-specific durability option，

但不得：

> 把它偷换成 generic `PersistenceSucceeded` / `FlushSuccess` / `CloseSuccess`。

---

## 7.17 Stable ABI 与 Runtime-facing Storage Capability Mapping

本规范不重新定义 Plugin Binary ABI；Stable ABI 由：

```text
docs/design/rodaq_plugin_system.md
```

冻结。

Storage Plugin core vtable 只包含：

```text
create
bind
configure
open
submit
flush
close
query_interface
destroy
```

Plugin → Runtime 通过 Service Registry 获取：

```text
rodaq.runtime.common.v1
rodaq.runtime.canonical.v1
rodaq.runtime.storage.v1
```

本文中的领域 capability：

```text
Prepare
EnsureDescriptorReady
HandoffRecord
Deliver PayloadConsumed
Deliver Persistence outcome
Report Fatal Failure
Quiesce after Failure
```

映射为：

```text
open/submit/flush/close 内部流程
+
required rodaq.storage.control.v1
+
Runtime Storage Service callbacks
+
Canonical read-only service
+
optional versioned Extensions
```

Capability negotiation 和 Descriptor readiness 不得只存在于 Plugin private implementation。

`HandoffRecord` 对应 Stable ABI `submit()`：

```text
function return == OK
    → HandoffAccepted

function return != OK
    → FatalFailure before HandoffAccepted
```

领域 Close 对应：

```text
close(rodaq_storage_close_request)
```

并显式携带：

```text
NORMAL
ACTIVATION_ABORT
FAILURE
```

只有 `NORMAL` 且 generic/backend finalization contract 全部满足时才允许形成 `NormalComplete` Artifact；Activation abort / Failure close 只负责安全收敛和 incomplete/failure artifact semantics。

`rodaq_storage_submit_result` 独立给出：

```text
Payload axis:
    Pending / Consumed

Persistence axis:
    Pending / Succeeded / Failed / Unknown
```

异步 terminalization 通过：

```text
payload_consumed()
persistence_completed()
```

分别通知。

配置通过 Runtime-owned `ConfigView` 传入；Plugin ABI 不暴露 `YAML::Node`、MCAP、SQLite3 或其他 backend-specific C++ 类型。

实现内部仍可以由一个 Storage Plugin class 完成全部逻辑，不要求拆分：

```text
DescriptorManager
RecordWriter
BatchManager
PersistenceManager
FlushManager
```

---

## 7.18 Control Plane 与 Data Plane

### Storage Control Plane

```text
Configure

Prepare

Descriptor readiness

Flush

Close

failure / quiescence
```

### Storage Data Plane

```text
Record handoff

Payload access

batching

backend write

completion
```

Runtime Storage Dispatch Lane：

> 是 Record handoff 的唯一逻辑入口。

---

## 7.19 Thread Safety

Storage 必须支持：

```text
one logical
Record Dispatch Lane
```

同时可能存在：

```text
Control Plane operation

Storage workers

completion callbacks
```

Plugin：

> 自己处理 backend thread-safety。

Runtime 不保证：

> backend library 本身 thread-safe。

---

## 7.20 Locking

Storage 不应在 completion callback 期间：

> 持有 backend global lock 并等待 Runtime 反向执行 Storage operation。

否则容易形成：

```text
Runtime waits Storage
Storage waits Runtime
```

循环等待。

Control operation：

```text
Flush
Close
Quiesce
```

也不能依赖：

> 只有被该 control operation 阻塞的 worker 才能完成的锁或 condition。

---

## 7.21 Implementation Restraint

本文中的：

```text
Descriptor Projection

Payload Lease

Persistence Tracking

Batching

Flush

Close

Quiescence
```

是逻辑职责。

不要求实现：

```text
DescriptorProjectionManager
PayloadLeaseManager
BatchManager
PersistenceManager
FlushManager
CloseManager
```

V1 implementation 可以保持为：

```cpp
class McapStoragePlugin;
class SqliteStoragePlugin;
```

配合：

```text
small projection maps

bounded queue

bounded batch buffer

backend writer

completion state
```

实现全部语义。

这些 class：

> 只是 V1 backend 示例，不是 Contract 对类型集合的限制。

未来增加：

```text
ParquetStoragePlugin
Hdf5StoragePlugin
RemoteStoragePlugin
```

等实现：

> 不改变本文 Contract。

---

# 8. Invariants 与 Conformance

## 8.1 Storage Invariants

### Canonical Authority

```text
Storage Plugin
never allocates or redefines
Canonical identity
```

---

### Full Fidelity

```text
V1 normal Storage
must be full-fidelity
or reject
```

不得 silent lossy。

---

### Effective Payload Limit

```text
Runtime Admission
MUST use
effective_payload_limit
```

正常 Handoff：

```text
payload_size
<=
effective_payload_limit
```

违反：

```text
⇒ ContractInvariantViolation
⇒ fail closed
```

---

### Capability Stability

```text
Storage Ready
⇒
accepted capability
does not become
silently stricter
```

---

### Descriptor-before-Record

```text
Record handoff
⇒
referenced Descriptor graph
Storage Ready
```

---

### Descriptor Identity

```text
same DescriptorId
⇒
same immutable content
```

不同 ID：

> 不得因 content 相同而丢失 identity distinction。

---

### Handoff

```text
HandoffAccepted
⇒
Storage responsibility
established
```

---

### Never-handed-off Record

```text
Accepted
but never HandoffAccepted
```

不属于：

```text
Storage completion responsibility
```

---

### No Accepted Drop

```text
Accepted RuntimeRecord
cannot be dropped
as ordinary Storage
backpressure policy
```

---

### Payload Access

```text
Storage may read Payload
only before PayloadConsumed
```

---

### PayloadConsumed

```text
PayloadConsumed
⇒
no future Storage access
to Runtime-provided bytes
```

---

### Persistence

```text
PersistenceSucceeded
PersistenceFailed
PersistenceUnknown
```

互斥且 terminal。

---

### Persistence Monotonicity

```text
Succeeded
cannot later become
Failed / Unknown
```

---

### Unknown

```text
PersistenceUnknown
≠
Succeeded
```

---

### Completion Determination vs Delivery

```text
Storage outcome determined
```

不意味着：

```text
Runtime already observed
completion notification
```

---

### Same-stream Ordering

```text
same Stream
logical Artifact order
==
AcceptanceSequence order
```

---

### Sequence Gaps

```text
AcceptanceSequence gaps
are legal
```

---

### Cross-stream Ordering

```text
no Canonical
global Record order
```

---

### Batching

```text
batching
does not replace
per-Record completion
```

---

### Resources

```text
all Storage-owned
buffering bounded
```

包括 aggregate bound。

---

### Flush

```text
FlushSuccess
⇒
all pre-barrier Record
persistence outcomes
determined inside Storage
```

但：

```text
does not imply
all completion notifications
already observed by Runtime
```

---

### Close

```text
CloseSuccess
⇒
complete
self-consistent
decodable
full-fidelity
NormalComplete Artifact
```

---

### Artifact Finalization

```text
Artifact creation
→ Incomplete
```

只有 successful finalization：

```text
→ NormalComplete
```

---

### Crash Durability

```text
PersistenceSucceeded
FlushSuccess
CloseSuccess
```

均不自动意味着：

```text
power-loss durability
```

---

### Fatal Failure

```text
fatal Storage failure
⇒
Storage Failed
⇒
Runtime failure shutdown
```

但：

```text
does not rewrite
Admission history
```

---

### Failure Quiescence

```text
Storage Quiesced
⇒
no future Runtime Payload access
and
no future normal completion
```

---

### Memory Safety

```text
cannot prove quiescence
⇒
retain / quarantine
```

优先于 cleanup completeness。

---

## 8.2 Required Conformance Scenarios

V1 Storage implementation 至少覆盖以下测试。

### Configure / Prepare

合法配置：

```text
Configured
→ Prepared
→ Ready
```

非法配置：

> Running 前失败。

---

### Capability Negotiation

Storage 声明：

```text
maximum Payload size

resource bounds

consumption mode

full-fidelity capability
```

Runtime 建立：

```text
effective_payload_limit
```

---

### Effective Payload Limit Used by Admission

Storage limit：

```text
1 MiB
```

Runtime 原始 limit：

```text
4 MiB
```

验证 Admission 使用：

```text
1 MiB
```

作为 effective limit。

超过 1 MiB 的 submission：

```text
PreAdmissionRejected
```

不得进入 Storage Handoff。

---

### Oversize Handoff Contract Violation

通过 test-only faulty Runtime 绕过 Admission limit，

向 Storage handoff 一个：

```text
payload_size
>
effective_payload_limit
```

的 Accepted Record。

验证：

```text
ContractInvariantViolation
→ FatalFailure
→ Storage Failed
```

不得：

```text
ordinary reject
and continue Ready
```

---

### Capability Shrink after Ready

模拟 Storage Ready 后 backend 发现 limit 必须缩小。

验证：

> Storage Failed，而不是动态缩小 capability 并继续运行。

---

### V1 Full-Fidelity Requirement

配置只能 lossy projection 的 mode。

验证：

```text
activation rejected
```

不得 silent fallback。

---

### Descriptor Ready

提交合法 Descriptor graph。

验证：

```text
Ready
```

---

### Descriptor Unsupported

提交 Canonical-valid 但 backend 无法 full-fidelity 表示的 Descriptor。

验证：

```text
Unsupported
```

Storage：

```text
remains Ready
for other Streams
```

---

### Required Descriptor Unsupported

Required startup Binding 的 Descriptor Unsupported。

验证：

> Runtime activation 失败。

---

### Dynamic Descriptor Unsupported

Running 中 dynamic Descriptor Unsupported。

验证：

> 只影响该 Stream。

---

### Descriptor I/O Failure

backend I/O failure。

验证：

```text
Storage Failed
Runtime failure shutdown
```

---

### Descriptor Timeout

验证：

> bounded timeout，不无限等待。

---

### Same Descriptor ID / Same Content

重复请求。

验证：

> idempotent Ready。

---

### Same Descriptor ID / Different Content

验证：

> fail closed。

---

### Different IDs / Same Content

验证 Artifact 可以 exact restore：

```text
two distinct identities
```

---

### Exact Schema Bytes

两个 semantic equivalent、byte different schema。

验证：

> exact bytes 分别保存。

---

### Unknown Timestamp Role

未知但合法 RoleIdentifier。

验证：

> opaque preserve。

---

### Descriptor-before-Record

尝试 handoff NotReady Descriptor 的 Record。

验证：

> Contract invariant violation / fail closed。

---

### Normal Record Handoff

验证 Stable ABI：

```text
submit() returns OK
    → HandoffAccepted
```

并分别验证两个 axis 最终 terminalize：

```text
PayloadPending / PayloadConsumed

PersistencePending
    → PersistenceSucceeded / Failed / Unknown
```

最常见正常路径可以是：

```text
submit result:
    PayloadConsumed
    PersistencePending
        ↓
later persistence_completed(Succeeded)
```

---

### Synchronous Payload Consumption

声明：

```text
submit result.payload_state
    = PAYLOAD_CONSUMED
```

验证：

- `submit()` return 后 Runtime 可释放 Payload；
- 不再发送 `payload_consumed()`；
- Storage 不再访问旧 PayloadView；
- Persistence axis 可以仍为 Pending。

---

### Asynchronous Payload Consumption

worker 延迟读取。

`submit()` 必须返回：

```text
payload_state = PAYLOAD_PENDING
```

验证：

- `submit()` return 前不得 inline 调用 `payload_consumed()`；
- explicit `payload_consumed(operation)` 前 Runtime 保持 Payload alive；
- callback exactly once；
- callback 后 Storage 不再访问旧 PayloadView。

---

### Storage-private Copy

验证：

```text
copy to bounded Storage-owned memory
        ↓
submit result:
    PAYLOAD_CONSUMED
    PERSISTENCE_PENDING
        ↓
later persistence_completed(Succeeded)
```

private copy：

> 计入 Storage resource budget。

---

### Access after PayloadConsumed

故意在 PayloadConsumed 后读取。

验证：

> Contract violation。

---

### Duplicate PayloadConsumed

`submit()` 返回 `PAYLOAD_PENDING`，随后发送两次：

```text
payload_consumed(operation)
```

验证：

> duplicate axis completion violation。

---

### Synchronous + Callback Duplicate

`submit()` result 已表示：

```text
PAYLOAD_CONSUMED
```

随后又调用：

```text
payload_consumed(operation)
```

验证：

> Contract violation。

同理，如果 `submit()` 已同步返回 terminal Persistence state，后续 `persistence_completed()` 同样是 duplicate violation。

---

### Same-stack Reentrant Completion

Plugin 在当前 `submit()` 调用栈内直接同步调用对应 callback。

验证：

> Contract violation；Runtime 不支持 submit-stack synchronous reentrancy。

另设 cross-thread race case：worker callback 可以在线程调度上先于 caller 观察到 `submit()` return；只要 Plugin 已不可逆地决定返回 `OK + matching PENDING`，Runtime 必须正确 reconcile，而不是误报 violation。

---

### Persistence Success

backend commit point 完成。

如果 `submit()` 时为：

```text
PERSISTENCE_PENDING
```

验证 exactly once：

```text
persistence_completed(
    operation,
    PERSISTENCE_SUCCEEDED,
    NULL)
```

如果 persistence 已能在 `submit()` 内同步确定成功，则允许 result 直接返回 `PERSISTENCE_SUCCEEDED`，并验证之后没有 callback。

---

### Premature Persistence Success

Record 仍处于：

```text
uncommitted transaction
```

时尝试 Success。

验证：

> conformance failure。

---

### Persistence Failed

确定性 write / transaction failure。

若此前为 `PERSISTENCE_PENDING`，验证 exactly once：

```text
persistence_completed(
    operation,
    PERSISTENCE_FAILED,
    &error)
```

并验证：

```text
Storage Failed
```

如果该 failure 已在 `submit()` 内同步确定，允许 submit result 直接返回 `PERSISTENCE_FAILED`，此后不得 callback。

---

### Persistence Unknown

transaction / backend outcome indeterminate。

若此前为 `PERSISTENCE_PENDING`，验证 exactly once：

```text
persistence_completed(
    operation,
    PERSISTENCE_UNKNOWN,
    &error)
```

并验证：

```text
Storage Failed
```

不得猜 Success，也不得为了 bookkeeping 方便强行改成 Failed。

如果 Unknown 已在 `submit()` 内同步确定，允许 result 直接返回 `PERSISTENCE_UNKNOWN`，此后不得 callback。

---

### Same-stream Ordering

输入：

```text
10
11
12
```

internal workers 故意乱序完成。

验证 logical Artifact order：

```text
10
11
12
```

---

### Relational Backend Ordering

SQLite 等 backend 不依赖 physical row order。

验证：

- AcceptanceSequence 被显式持久化；
- 按 Stream + AcceptanceSequence 可以唯一重建 same-stream order；
- 不存在与该 order 冲突的 Storage-level语义。

---

### Sequence Gap

```text
10
12
13
```

验证：

> 合法。

---

### Duplicate Sequence

```text
10
10
```

验证：

> fail closed。

---

### Sequence Regression

```text
10
9
```

验证：

> fail closed。

---

### Cross-stream Interleaving

验证：

> cross-stream 可自由 interleave，per-stream order 保持。

---

### Batch Success

多个 Record 一个 transaction。

成功后：

> 每 Record PersistenceSucceeded。

---

### Batch Rollback

完整 rollback。

验证：

> affected Records PersistenceFailed。

---

### Uncertain Batch Failure

部分 write 状态无法证明。

验证：

> 对应 Record PersistenceUnknown。

---

### Internal Queue Pressure

填满 Storage queue。

验证：

> 不 drop Accepted Record。

Handoff bounded wait。

---

### Handoff Timeout

capacity 不恢复。

验证：

```text
timeout
→ Storage Failed
```

---

### Aggregate Resource Bound

同时填充：

```text
Record queue
batch buffer
private Payload copy
compression buffer
```

验证 aggregate bound。

---

### Backpressure Propagation

降低 backend throughput。

验证：

```text
Storage slows
→ Runtime dispatch fills
→ Admission pressure
→ Acquisition Reject / Wait
```

---

### Flush Success

多个 handed-off Record。

Flush 后验证：

> 所有 flush-set persistence outcome 已在 Storage 内部确定。

---

### Flush Completion Delivery Lag

构造：

```text
backend outcome fixed
```

但 completion callback 尚在队列中。

让：

```text
FlushSuccess
```

先返回。

验证：

- FlushSuccess 合法；
- Runtime 继续处理后续 completion；
- Runtime 不因为 Flush 已返回而忽略 callback；
- 最终所有 flush-set Record 在 Runtime 中 terminal。

---

### Flush while Running

若 implementation 支持 running Flush，

验证 barrier 前后 Record 正确分代。

---

### Flush Failure

验证：

```text
Storage Failed
Close cannot
later return success
```

---

### Flush Timeout

验证：

> bounded timeout。

---

### Flush Not Equal Durability

关闭 durable option。

通过：

```text
fault injection
```

或：

```text
backend durability
status query
```

验证 FlushSuccess 后 durability 状态仍可能未建立。

如果 backend implementation 无法提供可测试的 durability observation：

> 该场景可以记录为 backend-specific known limitation，但不得因此把普通 Flush 定义成 durable。

---

### Starting Activation Abort

Storage 已完成 Prepare，并已创建 / 打开 staging Artifact，但 Runtime 尚未 Activation Commit。触发 activation abort。验证：

```text
no normal Record handoff
pending Descriptor readiness settled / cancelled
Storage workers stop safely
backend resources close
Artifact remains Incomplete / Aborted
no NormalComplete marker
```

cleanup success：

> 不得伪装成 normal `CloseSuccess`。

cleanup failure / timeout：

> Runtime integration 最终进入 failure disposition。

---

### Normal Close

完成 final Flush 和 completions。

Close。

验证：

```text
CloseSuccess
Storage Closed
```

Artifact：

```text
NormalComplete
complete
self-consistent
decodable
full-fidelity
```

---

### Completion Delivery after Flush before Close

FlushSuccess 后仍有 completion 通知等待 Runtime processing。

验证：

> Runtime 在 final Stop barrier 前继续处理，Storage 不错误重复发 completion。

---

### Close with Pending Internal Outcome

Storage 内部仍有真正 undetermined persistence outcome。

验证：

> 不得 CloseSuccess。

---

### Close with Determined but Undelivered Completion

Storage internal outcome 已确定，

但 Runtime 尚未处理 completion。

验证：

- Storage 可以继续完成 finalization；
- Runtime final Stop barrier仍等待 Completion Domain；
- 不把“通知未到达 Runtime”误认为 Storage outcome 未确定。

---

### Close Finalization Failure

注入：

```text
index
footer
final transaction
completion marker
```

failure。

验证：

```text
CloseFailed
Runtime Failed
```

---

### Close Timeout

验证：

> bounded timeout。

---

### Initial Artifact Status

Artifact Prepare 完成但尚未 Close。

验证：

```text
finalization status
!= NormalComplete
```

---

### Normal-complete Marker

successful Close。

验证外部 RoDAQ reader 可以判定：

```text
NormalComplete
```

---

### Failed-close Marker

Close finalization 中途失败。

验证 Artifact：

```text
not NormalComplete
```

不得留下错误 complete marker。

---

### Partial Artifact Detection

直接打开 CloseFailed 后的 output。

验证 conforming reader：

> 报告 partial / incomplete，而不是 normal conformant Artifact。

---

### Third-party Readability

如果 backend third-party reader 可以打开 partial Artifact，

验证：

> RoDAQ finalization metadata 仍能让 RoDAQ-aware validation tool 判断其 incomplete。

不要求通用第三方工具理解 RoDAQ completeness semantics。

---

### Handoff Responsibility / Payload Access Boundary

分别验证：

```text
A. Handoff entry
   → synchronous call-scoped PayloadView read only

B. HandoffAccepted
   → async retention allowed
   → lease ends at PayloadConsumed

C. FatalFailure before HandoffAccepted
   → return 后 no retained PayloadView / worker access

D. Runtime-side Handoff timeout
   while Storage execution may still run
   → timeout does not imply PayloadConsumed
   → Runtime waits failure quiescence / quarantines
```

---

### Fatal Storage Failure with Runtime Queued Records — Integration Test

让 Storage Failed 时 Runtime dispatch queue 中仍存在：

```text
Accepted
not HandoffAccepted
```

Record。

该测试属于：

> **Runtime / Storage integration conformance**

而不是单独的 Storage Plugin unit conformance。

验证：

- Storage 不为未 HandoffAccepted Record 产生 completion；
- Runtime 不把它们改写为 AdmissionRejected；
- Runtime 对已知 never-handed-off Record 使用 `PersistenceFailed`；
- 如果 handoff boundary 本身不可判定，才允许 `PersistenceUnknown`。

---

### Failure with Outstanding Handoff

Record 已：

```text
HandoffAccepted
```

随后 fatal failure。

验证：

- Persistence 最终 Succeeded / Failed / Unknown；
- 不永久 Pending；
- Payload lease consumed 或保持到 safe quiescence。

---

### Failure Payload Quiescence

worker 仍可能访问 Payload时触发 failure。

验证：

> Runtime 不提前 release Payload。

---

### Quiescence Timeout

Storage worker无法停止。

验证：

```text
retain / quarantine
```

Payload 和 callback-visible state。

---

### Completion after Quiescence

Storage 已报告 Quiesced，

随后再发 completion。

验证：

> Contract violation。

---

### Persistence Completion Reordering

多个 Record 的 completion callback 顺序随机。

验证：

> Runtime Completion Domain 正确处理。

completion order：

> 不改变 Artifact same-stream order。

---

### Profile v0.5 Scalar Domain Extremes

构造并 round-trip：

```text
Canonical ID = 0
Canonical ID near uint64 max
AcceptanceSequence = 0 / high uint64
SourceSequence absent
SourceSequence present(0)
SourceSequence present(high uint64)
ClockReading = INT64_MIN / -1 / 0 / INT64_MAX
large positive reduced resolution numerator / denominator
```

验证：

- exact values preserved；
- presence 不由 numeric sentinel 推断；
- 无 signedness narrowing / truncation / wrap。

---

### Exact SourceId Bytes

包含：

```text
zero byte
non-text bytes
```

的 SourceId。

验证 exact round-trip。

---

### Source Presence

验证：

```text
absent
```

与：

```text
present
```

保持区别。

---

### SourceSequence Presence

验证：

```text
absent
```

与：

```text
present(0)
```

保持区别。

---

### Timestamp Presence

验证：

```text
Absent
```

与：

```text
Present(0)
```

保持区别。

---

### Clock Preservation

两个不同 ClockId：

```text
same resolution
same origin
```

验证 identity 仍 distinct。

---

### Canonical ID Surrogate Mapping

backend 使用自己的 row/channel ID。

验证：

> exact restore original Canonical IDs。

---

### Payload Compression

启用 compression。

随机 Payload round-trip。

验证：

```text
decoded bytes
==
original logical bytes
```

---

### No Payload Re-serialization

对 byte representation 非唯一的数据类型，

验证：

> Storage 不通过 deserialize/re-serialize 替代 exact Payload。

---

### No Direct Acquisition Interaction

验证：

> Storage 不直接调用 Acquisition Plugin。

---

### No Canonical Mutation

验证 Storage-facing API：

> 不允许修改 Descriptor / RuntimeRecord Canonical semantics。

---

## 8.3 V1 Storage Plugin Baseline

RoDAQ V1 Storage path：

```text
Canonical Descriptor Graph
        ↓
Storage Projection
        ↓
Descriptor Ready
        ↓
Binding Active

Accepted RuntimeRecord
        ↓
single logical
Storage Dispatch Lane
        ↓
Storage Handoff
        ↓
synchronous call-scoped Payload access only
        ↓
HandoffAccepted
        ↓
Storage responsibility
+ async-retention authorization
        ↓
Payload Access Lease
        │
        ├── PayloadConsumed
        │
        └── PersistencePending
                 ↓
        Succeeded / Failed / Unknown
```

Descriptor contract：

```text
Canonical-valid Descriptor
        ↓
Storage Projection
        │
        ├── Ready
        │
        ├── Unsupported
        │      ↓
        │   Stream-local
        │   activation failure
        │
        └── Failed
               ↓
           Storage Failed
```

因此 backend 无法 full-fidelity 表示：

```text
one dynamic Descriptor
```

时：

> 不自动拖垮其他已工作的 Stream。

但是：

```text
backend I/O failure
consistency failure
bounded-progress failure
```

会使：

```text
Storage → Failed
```

并触发 Runtime-wide failure shutdown。

Storage capability：

```text
Runtime configured limit
        +
Storage advertised limit
        ↓
effective_payload_limit
```

Runtime：

> MUST 在 Admission 使用该 limit。

正常情况下：

```text
oversize Record
```

永远不会进入 Storage。

如果违反：

```text
ContractInvariantViolation
        ↓
fail closed
```

而不是：

```text
ordinary Handoff rejection
```

Record responsibility boundary：

```text
Admission Accepted
        ↓
Runtime dispatch ownership
        ↓
HandoffAccepted
        ↓
Storage responsibility
```

因此：

```text
Accepted
but never HandoffAccepted
```

不属于 Storage completion domain。

known never-handed-off：

```text
Runtime persistence result
→ Failed
```

只有：

```text
handoff boundary uncertain
```

时：

```text
→ Unknown
```

Payload lifetime：

```text
Runtime owns Payload
        ↓
Handoff call-scoped read
        ↓
HandoffAccepted
        ↓
Storage gets async
read-only lease
        ↓
PayloadConsumed
        ↓
Runtime may release
```

其中：

```text
PayloadConsumed
≠
PersistenceSucceeded
```

Storage-private copy：

```text
copy
        ↓
PayloadConsumed
        ↓
later persistence
```

合法，

但 private copy：

> 必须计入 Storage bounded resource。

Persistence：

```text
Pending
    ↓
Succeeded
or
Failed
or
Unknown
```

`Succeeded`：

> 单调、不可撤回。

Batching：

```text
transaction
chunk
group commit
```

只是 implementation。

每个 Record：

> 仍保留独立 persistence outcome。

Flush：

```text
all pre-barrier
Storage outcomes determined
```

但：

```text
FlushSuccess
```

不意味着：

```text
Runtime already processed
every completion callback
```

正确模型：

```text
Storage outcome fixed
        ↓
FlushSuccess
        ↓
completion notification
may still arrive
        ↓
Runtime Completion Domain
        ↓
Runtime state terminal
```

因此 Runtime Stop：

> 必须在 Flush 后继续处理 completion。

Artifact lifecycle：

```text
Prepare
        ↓
Artifact Incomplete
        ↓
Record / Descriptor writes
        ↓
Flush
        ↓
Close finalization
        ↓
finalization marker
        ↓
NormalComplete
```

只有：

```text
complete
self-consistent
decodable
full-fidelity
NormalComplete
```

Artifact 才允许：

```text
CloseSuccess
```

Close failure：

```text
Artifact remains
not NormalComplete
```

即使 backend physical file：

> 仍然可以被某些工具部分读取。

full fidelity 要求精确保留 Profile v0.5 scalar domain 与 Canonical state：

```text
Canonical IDs as exact uint64 values
AcceptanceSequence as exact uint64
SourceSequence optional exact uint64
ClockReading exact int64
Clock resolution exact positive uint64 rational

Descriptor identities

Descriptor identities

Schema bytes

SourceId bytes / presence

SourceSequence presence / value

Timestamp keys

Clock identity

Present / Absent

PayloadEncoding

Payload bytes
```

backend 可以使用：

```text
surrogate IDs
compression
chunks
transactions
tables
sidecars
```

但必须：

> full-fidelity reversible。

V1 不允许：

```text
silent lossy projection
```

因此：

```text
cannot preserve
    → reject

not

cannot preserve
    → silently degrade
```

Crash durability：

> 与 normal persistence、Flush、Close completeness 分离。

如果后续需要统一：

```text
fsync

power-loss consistency

atomic Artifact publication

manifest

partial Artifact recovery
```

再由独立：

```text
persistence.md
```

定义。

一个合理 C++17 implementation 可以保持为：

```cpp
class McapStoragePlugin;
class SqliteStoragePlugin;
```

配合：

```text
small projection maps

bounded queue

bounded batch buffer

backend writer

completion state
```

实现全文 Contract，

而无需增加一系列 Manager。

最终形成：

> **支持 MCAP、SQLite3 等主流 Storage backend，具备 Descriptor-before-Record、effective capability enforcement、bounded backpressure、异步 Payload lifetime、batching、per-Record persistence completion、full-fidelity projection、显式 Artifact completion state、bounded Flush/Close、failure uncertainty 和 safe quiescence，同时保持 Runtime/Core plugin-independent 与 C++17 实现克制的 RoDAQ V1 Storage Plugin 模型。**

---

## M2 Convergence Storage Submit Matrix

Storage submit output is validated as one semantic tuple:

```text
struct_size and initialized prefix
reserved fields
Payload state = Pending | Consumed
Persistence state = Pending | Succeeded | Failed | Unknown
diagnostic status/flags/sizes/UTF-8/reserved fields
success/failure consistency
synchronous-terminal plus callback duplication
```

Malformed output fails closed and is never converted into an Accepted no-drop
success. Payload consumption, Release and Persistence remain independent.
Storage completion callbacks only stage events; they do not execute
Plugin-origin Payload release inline.
