# RoDAQ Admission Design Specification

**Version:** v0.5
**Status:** Design Specification  
**Scope:** RoDAQ V1 Admission, Resource Reservation, Acceptance Ordering and Backpressure

**Normative Dependencies:**

- RoDAQ Canonical Data Model Design Specification v0.9
- RoDAQ V1 Canonical Profile v0.6
- RoDAQ System Architecture Design Specification v0.6
- RoDAQ Runtime Design Specification v0.6
- RoDAQ Payload Design Specification v0.5

---

# 1. 目标与设计边界

## 1.1 设计目标

RoDAQ Admission 负责把 Acquisition Plugin 的一次 submission 转换为以下三种确定结果之一：

```text
PreAdmissionRejected

AdmissionRejected

Accepted
```

核心数据路径为：

```text
Submission
    ↓
Structural Validation
    ↓
Ingress Timestamp Capture
    ↓
Bounded Resource Reservation
    ↓
Payload Materialization
    ↓
Per-stream Admission Commit
    ↓
Admission Gate Pass
    ↓
AcceptanceSequence
    ↓
AcceptedRecord Construction
    ↓
Per-stream Ordered Enqueue
    ↓
Accepted
```

Admission 必须保证：

- Submission call lifetime 与 Admission Attempt lifetime 明确区分；
- Runtime 对整个 synchronous `Submit` 使用独立 active-submission accounting；
- Gate pass 是 admission-attempt domain 的唯一入口；
- Gate pass 前不消耗 AcceptanceSequence；
- Gate pass 后 AcceptanceSequence 唯一、不可回滚、不可复用；
- Gate pass 前已经保证正常 Accepted path 所需 bounded resource；
- Payload size 在 Gate pass 前满足 activation-wide `effective_payload_limit`；
- ordinary queue pressure 不在 Gate pass 后才被发现；
- Payload 在 Gate pass 前已经成为稳定 `PayloadRef`；
- Gate pass 后正常路径不再等待普通资源；
- accepted AcceptedRecord 在 publication 前已经完整构造；
- 同一 Stream 的 accepted enqueue 顺序与 AcceptanceSequence 顺序一致；
- Storage backpressure 通过 Runtime bounded resource 向上游传播；
- Stop / failure cutoff 与 concurrent submission 具有唯一 linearization；
- fatal Storage failure 不改写已经进入 admission domain 的 history；
- Admission correctness failure 与 downstream Storage failure 明确区分；
- pre-gate wait / interruption / rollback 不遗留 partial resource；
- Runtime 不依赖 unbounded queue 或 unbounded shutdown wait；
- Admission hot path 不执行 Canonical registration 或 source-specific interpretation；
- Profile v0.5 scalar values 不使用 numeric sentinel 表示 absent。

---

## 1.2 设计范围

本文定义：

```text
Submission

active submission call accounting

pre-admission validation

ingress timestamp capture

bounded resource reservation

backpressure

Admission Gate

gate-pass linearization

AcceptanceSequence allocation

Accepted / AdmissionRejected outcome

AcceptedRecord construction

per-stream ordered enqueue

admission in-flight accounting

normal cutoff

failure cutoff

Binding / Stream / Source interaction

normal-stop admission barrier

failure-shutdown admission barrier

conformance requirements
```

---

## 1.3 非设计范围

本文不重新定义：

```text
Canonical Record semantics
Stream identity
Source identity
Descriptor semantics
Payload ownership semantics
Storage persistence semantics
Plugin lifecycle
```

其中：

- Payload ownership / lifetime 由 `payload.md` 定义；
- Runtime lifecycle / Binding / Stream State 由 `runtime.md` 定义；
- Acquisition Plugin native behavior 由 `acquisition_plugin.md` 定义；
- Storage completion / persistence 由 `storage_plugin.md` 定义。

---

## 1.4 Submission 与 Admission Attempt

必须严格区分：

```text
Submission
```

和：

```text
Admission Attempt
```

### Submission

Submission 是 Acquisition Plugin 向 Runtime 提交一次 source sample 的运行时操作。

Submission 本身：

> **不自动构成 Canonical admission attempt。**

它可能因为：

```text
Runtime not Running
Gate closed
invalid Binding
Stream unavailable
Source unavailable
invalid Payload
resource unavailable
sequence exhausted
```

在 Gate pass 前结束。

---

### Admission Attempt

只有 submission 成功完成：

> **Admission Gate Pass**

以后，才存在 Admission Attempt。

因此：

```text
Submission
    ≠
Admission Attempt
```

Gate pass 之后：

> Canonical admission-attempt semantics 生效。

---

## 1.5 V1 Admission 原则

V1 采用：

```text
synchronous admission decision

all ordinary bounded resources
guaranteed before Gate pass

Payload stable before Gate pass

per-stream admission commit serialization

sequence availability check
and sequence allocation
in the same per-stream commit

Gate pass + in-flight entry
as one logical admission-entry operation

no ordinary blocking after Gate pass

accepted AcceptedRecord fully constructed
before publication

same-Stream sequence-ordered enqueue

single bounded dispatch path
```

作为实现基线。

Admission hot path 应尽量收敛为：

```text
validate
    ↓
timestamp
    ↓
reserve
    ↓
materialize
    ↓
commit
    ↓
enqueue
```

而不是建立复杂事务对象体系。

---

## 1.6 Synchronous Admission Operation

V1 Acquisition-facing submission：

> **在 admission decision 层面是同步操作。**

一次 submit operation 从进入 Runtime 开始，到 synchronous return boundary 结束，整个期间都属于 Runtime `active_submission_calls` accounting domain。

它与 Admission Attempt accounting 不同：

```text
Submit entered but not Gate-passed
    → active submission
    → not an Admission Attempt
```

一次 submit operation 返回前，必须已经确定：

```text
PreAdmissionRejected

or

AdmissionRejected

or

Accepted
```

如果返回：

```text
Accepted
```

则在返回前必须已经完成：

```text
AcceptedRecord construction
+
ordered enqueue publication
```

即：

> submit 返回 `Accepted` 时，已经越过 §4.14 定义的 Accepted linearization point。

这不意味着：

```text
Storage handoff
PayloadConsumed
PersistenceSucceeded
```

也是同步的。

这些 Storage path 可以异步执行。

该规则同时保证普通 Copy Payload 可以在：

```text
submit synchronous input lifetime
```

内完成 materialization。

---

# 2. Admission Processing Model

## 2.1 Submission Input

一次 submission 逻辑上至少携带：

```text
BindingHandle

optional SourceHandle
optional SourceSequence

Plugin-owned TimestampState values

Payload input
```

Payload input 为：

```text
Copy
Adopt
Retain
```

之一。

Admission 不重新解释：

```text
SourceId
PayloadEncoding
TimestampKey
Schema
```

这些信息已经通过 Runtime Binding 和 SourceHandle 建立。

---

## 2.2 Processing Phases

V1 Admission 使用以下逻辑阶段：

```text
Phase 1
Structural Validation
        ↓
Phase 2
Ingress Timestamp Capture
        ↓
Phase 3
Bounded Resource Reservation
        ↓
Phase 4
Payload Materialization
        ↓
Phase 5
Per-stream Admission Commit
        ↓
Phase 6
Ordered Enqueue / Terminalization
```

这些是规范上的责任阶段。

不要求实现为：

```text
six classes
six managers
six state objects
```

实际实现可以由一个紧凑的：

```text
DAQ::submit(...)
```

完成。

---

### 2.2.1 Pre-gate Failure Cleanup

Phase 3 以后、Gate pass 以前发生的任何无法继续 admission 的事件，都必须释放已经取得的临时 Runtime responsibility。

这里的事件不仅包括：

```text
validation failure
allocation failure
Payload materialization failure
```

也包括：

```text
Stop interruption
failure cutoff
Gate close
Binding invalidation
Stream unavailable
Source unavailable
Wait timeout
```

例如：

```text
resource A reserved
        ↓
resource B wait interrupted
```

则：

```text
rollback resource A
        ↓
PreAdmissionRejected
```

Phase 3 的 ordinary resource wait 发生在 Phase 4 Payload materialization 之前，因此这种 wait-interruption 场景正常情况下尚未取得 Adopt / Retain Payload responsibility。

如果 failure 发生在 Phase 4 已完成之后、Gate pass 之前，例如 final revalidation 观察到 cutoff / Binding invalidation：

```text
release / destroy stable PayloadRef
+
rollback still-local reservations
+
PreAdmissionRejected
```

因此：

> **所有 pre-gate failure 都必须按实际已经取得的 responsibility 精确 cleanup；不能假定 resource waiter 已经持有 Payload ownership。**

Rollback 必须：

```text
noexcept
non-blocking
not fail
```

并且不得产生：

```text
orphan reservation
double release
resource leak
```

如果 Runtime 自身无法正确 rollback：

> 属于 Runtime implementation invariant violation，必须 fail closed。

---

## 2.3 Phase 1：Structural Validation

Gate pass 前必须完成所有可以确定的 cheap structural validation。

至少包括：

```text
activation context valid

BindingHandle valid

Binding belongs to current Runtime

SourceHandle valid when present

SourceSequence ⇒ SourceHandle present

Payload size representable

Payload size <= activation effective_payload_limit

Payload data / anchor structurally valid

timestamp slot mask valid

Plugin does not write ingress slot

timestamp slots belong to Binding

no unsupported timestamp slot

SourceSequence, when present, is a canonical uint64 value

ClockReading values are canonical int64 ticks
```

Profile v0.5 中：

```text
SourceSequence: uint64, 0 is valid
ClockReading: int64, 0 and negative values are valid
```

Admission 不得使用 `0`、`UINT64_MAX`、负数等 numeric value 作为通用 absent sentinel。Plugin-native unsupported / unknown sentinel 必须在 Acquisition Mapping 进入 Runtime 前转换为 explicit absence。

所有进入 Runtime 的 scalar conversion 必须已经 checked，禁止 truncation / signedness reinterpretation / wrap。

这些检查失败：

```text
PreAdmissionRejected
```

不分配 AcceptanceSequence。

如果 Plugin：

```text
writes rodaq/acquisition_ingress slot
uses an undefined timestamp slot
```

则除了 PreAdmissionRejected 外：

> 属于 Acquisition Plugin Contract violation，Runtime SHOULD 产生 operational diagnostic。

---

## 2.4 Early Operational Eligibility Check

在进入可能等待的 resource reservation 前，Runtime SHOULD 执行一次 cheap operational check：

```text
Runtime Running

Global Gate appears Open

Binding Active

Binding current for Stream

Stream Available

Source valid / available
where required

Payload size <= effective_payload_limit

AcceptanceSequence not exhausted
```

这是：

> **early rejection optimization**

而不是最终 admission linearization。

因为检查完成后，以下事件仍可能并发发生：

```text
Stop
failure cutoff
Binding switch
Source failure
Stream availability change
```

因此 Gate pass 前必须再次执行 final revalidation。

---

## 2.5 Phase 2：Ingress Timestamp Capture

Runtime 在：

> **完成 Phase 1 structural validation 后、进入任何可能阻塞的 resource wait 之前**

读取 Runtime ingress Clock，得到该 submission 的：

```text
rodaq/acquisition_ingress
```

reading。

精确顺序为：

```text
submit() called
        ↓
Phase 1 structural validation
        ↓
read Runtime ingress Clock
        ↓
resource reservation / wait
```

这里的 ingress timestamp 表示：

> sample 到达并通过 Runtime acquisition-facing structural boundary 的 Runtime-local observation time。

---

### 2.5.1 为什么在 Resource Wait 前捕获

如果 Runtime 因：

```text
Storage backpressure
queue capacity
Payload budget
```

等待数毫秒甚至更长，

`rodaq/acquisition_ingress` 仍应表达：

> sample 到达 Runtime admission boundary 的时间，

而不是：

> resource pressure 解除的时间。

---

### 2.5.2 Rejected Submission

如果 timestamp 已捕获，但 submission 最终：

```text
PreAdmissionRejected
```

该 reading 被丢弃。

不存在 Canonical Record，因此不存在需要持久化的 TimestampStateMap。

---

### 2.5.3 Ingress Timestamp 与 AcceptanceSequence

Ingress timestamp 表示：

```text
Runtime ingress observation time
```

AcceptanceSequence 表示：

```text
Admission Commit ordering
```

二者语义不同。

同一 Stream 多线程并发时，允许：

```text
Submission A
ingress = 100

Submission B
ingress = 101

B commits first
```

从而：

```text
B sequence = N
A sequence = N+1
```

因此：

> **AcceptanceSequence order 不要求与 ingress timestamp order 相同。**

---

## 2.6 Phase 3：Bounded Resource Reservation

Gate pass 前，Runtime 必须保证：

> 如果 submission 随后成功 Gate pass，则普通 Accepted path 所需的 bounded Runtime resources 已经可用。

其中 Payload logical size 已经通过：

```text
payload_size <= effective_payload_limit
```

验证。`effective_payload_limit` 由 Runtime v0.5 在 Starting 阶段根据 Runtime limit 与 Storage capability 建立，并在 activation 内保持稳定。

至少包括逻辑上的：

```text
Payload byte budget

Payload object / control-block capacity

AcceptedRecord capacity

bounded dispatch capacity

Storage completion bookkeeping capacity
```

具体 physical resource 可以合并。

例如：

```text
AcceptedRecord slot
+
completion tracking state
```

可以来自一个统一 pool。

---

### 2.6.1 Reservation Failure

如果任一 required resource 无法取得，submission：

```text
Reject policy
    → PreAdmissionRejected

Wait policy
    → wait before Gate pass
```

不得：

```text
Gate pass
    ↓
later discover ordinary
capacity unavailable
```

---

### 2.6.2 Reservation Rollback

如果资源采用 sequential reservation：

```text
reserve A
reserve B
reserve C
```

在全部 guarantee 建立以前因为：

```text
failure
interruption
timeout
cutoff
state invalidation
```

无法继续，

必须反向释放已经取得的 reservation。

因此：

```text
Wait interrupted
```

明确适用本节 rollback 规则。

Rollback 必须：

```text
noexcept
non-blocking
cannot fail
```

如果 rollback 本身可能失败：

> Runtime implementation 不满足 Admission Contract。

---

## 2.7 Resource Reservation 不决定 Ordering

Resource reservation：

> 只建立 capacity guarantee。

它本身不决定：

```text
AcceptanceSequence
accepted enqueue position
```

例如 submission 较早取得：

```text
Payload budget
dispatch capacity
```

不意味着它一定得到较小 AcceptanceSequence。

Sequence 只由：

```text
per-stream Admission Commit
```

决定。

---

## 2.8 Phase 4：Payload Materialization

在 Gate pass 前，Runtime 建立稳定：

```text
PayloadRef
```

具体规则来自 `payload.md`。

### Copy

```text
allocate Runtime backing
copy exact logical bytes
```

并且所有 borrowed bytes 读取必须：

```text
finish before submit() returns
```

### Adopt

```text
structural validation
        ↓
required Payload budget / object capacity already reserved in Phase 3
        ↓
ownership transfer
```

### Retain

```text
structural validation
        ↓
required Payload budget / object capacity already reserved in Phase 3
        ↓
acquire independent lifetime anchor
```

---

### 2.8.1 Materialization Failure

Payload materialization failure 发生在 Gate pass 前时：

```text
PreAdmissionRejected
```

并执行：

```text
Payload cleanup
+
Phase 3 reservation rollback
```

不得消耗 AcceptanceSequence。

---

## 2.9 Payload Materialization Before Gate Pass

V1 冻结：

> **Payload materialization 必须在 Admission Gate pass 前完成。**

因此普通：

```text
Copy allocation failure
invalid Adopt anchor
invalid Retain anchor
Payload object capacity failure
```

不应在 Gate pass 后首次出现。

Gate pass 后不再依赖：

```text
Payload allocation
Payload copy
external anchor acquisition
```

才能完成正常 Accepted path。

如果 Gate pass 后仍出现所谓：

```text
Payload materialization failure
```

说明实现违反了本节 contract，属于 Runtime internal correctness failure，而不是合法 Payload error path。

---

## 2.10 Phase 5：Per-stream Admission Commit

在进入 Admission Commit 前，submission 已拥有：

```text
stable PayloadRef

reserved bounded resources

captured ingress timestamp

validated handles
```

Admission Commit 必须：

```text
short
bounded
free of ordinary allocation
free of Storage I/O
free of Plugin callbacks
```

该阶段负责：

```text
final revalidation

sequence availability check

Gate pass

in-flight accounting entry

AcceptanceSequence allocation

ordered enqueue ordering decision
```

以及为 AcceptedRecord publication 建立正确顺序。

---

# 3. Resource Reservation 与 Backpressure

## 3.1 Admission Resource Set

V1 Admission 在 Gate pass 前逻辑上保证：

### Payload Resource

```text
logical Payload byte budget
PayloadRef / control capacity
```

### Record Resource

```text
AcceptedRecord capacity
```

### Dispatch Resource

```text
bounded dispatch capacity
```

### Completion Resource

```text
Storage completion bookkeeping capacity
```

实现可以合并这些 physical resource。

规范不要求：

```text
PayloadReservation class
QueuePermit class
AdmissionToken class
```

---

## 3.2 All-or-Nothing Guarantee

Gate pass 前必须达到：

```text
all ordinary resources required
for Accepted path
are guaranteed
```

实现可以：

```text
reserve atomically
```

或：

```text
reserve sequentially
with rollback
```

但必须满足：

```text
deadlock-free
bounded
interruptible before Gate pass
rollback cannot fail
```

---

## 3.3 Reservation Ordering

不同 resource 的具体 reservation 顺序：

> 不在本文冻结。

例如：

```text
Payload budget
→ AcceptedRecord slot
→ dispatch capacity
```

或者其他顺序均可。

但所有 submission 必须遵守一致的 reservation discipline，避免：

```text
cross-resource deadlock
```

---

## 3.4 Backpressure Policy

required resource 暂时不可用时，V1 支持两种基本语义：

```text
Reject
Wait
```

### Reject

立即：

```text
PreAdmissionRejected
```

适用于：

```text
high-rate telemetry
best-effort sensors
non-blocking callbacks
```

### Wait

submission 在 Gate pass 前等待 resource。

等待必须可被以下事件中断：

```text
Runtime Stop
failure cutoff
Gate close
Binding invalidation
Stream unavailable
Source unavailable
configured timeout / deadline
```

中断结果：

```text
PreAdmissionRejected
```

不消耗 AcceptanceSequence。

如果 Wait 开始前或 Wait 过程中已经取得部分 reservation：

> **所有 partial reservation 必须按照 §2.6.2 rollback。**

如果已经取得 Payload lifetime responsibility：

> 同时按照 §2.2.1 和 `payload.md` 执行对应 cleanup。

Wait interruption：

> 明确属于 pre-gate failure cleanup。

---

## 3.5 Wait 必须可终止

Runtime 不允许：

```text
pre-admission resource wait
```

导致 shutdown 永久阻塞。

Gate close / failure cutoff 必须能够：

```text
wake
or
cancel
```

所有尚未 Gate pass 的 waiter。

被中断的 waiter：

1. 停止等待新的 resource；
2. rollback 已取得的 partial reservation；
3. cleanup 已取得的 temporary Payload responsibility；
4. 返回 `PreAdmissionRejected`。

---

## 3.6 Deadline

Wait 可以配置：

```text
deadline
timeout
```

具体配置方式不在本文冻结。

timeout 且尚未 Gate pass：

```text
PreAdmissionRejected
```

并且：

> timeout 与 Stop / failure interruption 使用同一 pre-gate rollback 语义。

---

## 3.7 Storage Backpressure

Storage 变慢时：

```text
Storage drain rate decreases
        ↓
dispatch capacity decreases
        ↓
Admission reservation unavailable
        ↓
Reject or Wait
        ↓
Acquisition observes backpressure
```

Storage Plugin：

> 不直接控制 Acquisition Plugin。

Backpressure 通过 Runtime bounded resources 传播。

---

## 3.8 Queue Pressure 与 Accepted Record

V1 不允许：

```text
Record Accepted
        ↓
queue capacity problem discovered
        ↓
drop Accepted Record
```

因为 dispatch capacity 已在 Gate pass 前保证。

因此 ordinary queue pressure 必须通过：

```text
PreAdmission Reject
or
PreAdmission Wait
```

处理。

V1 同样不提供：

```text
drop-oldest Accepted Record
```

作为正常 queue policy。

如果 Storage failure 导致 accepted Record 最终未持久化：

> 这是 persistence outcome，不是 queue-drop semantics。

---

## 3.9 Reservation Lifetime

Pre-admission reservation 最终只能被：

```text
released because submission
did not Gate pass

transferred into
Accepted AcceptedRecord /
Storage tracking state

reconciled through
AdmissionRejected failure cleanup
```

不得存在：

```text
orphan reservation
```

### PreAdmissionRejected

所有 reservation：

> 必须正常 rollback / release。

### Accepted

对应 resource responsibility：

> 转移到 AcceptedRecord、dispatch path 或 Storage tracking state。

### AdmissionRejected

由于 AdmissionRejected 表示：

> Runtime / Admission correctness failure，

不能简单假设所有 reservation 都仍处于普通 rollback 状态。

因此 AdmissionRejected path 必须执行：

> **failure cleanup reconciliation**

规则为：

- 能够证明仍由 local admission path 安全持有的 reservation：释放；
- 已经明确转移到其他 Runtime tracking state 的 resource：由对应 owner 接管；
- ownership / accessibility 已无法安全判定的 resource：不得伪装成已释放；
- 必要时按照 Runtime / Payload failure-cleanup 规则 quarantine 到安全 lifetime boundary。

failure cleanup 中：

> memory safety 和 ownership correctness 优先于资源完全回收。

---

# 4. Gate、Sequence 与 Acceptance

## 4.1 Final Pre-gate Revalidation

进入 per-stream Admission Commit 后，Runtime 必须重新读取并验证：

```text
Runtime == Running

Global Gate == Open

Storage ready

Binding == Stream.current_binding

Binding == Active

Stream == Available

required Source still eligible

AcceptanceSequence still available
```

这次检查必须使用：

> 与对应 Control Plane state publication 具有正确 happens-before 关系的最新 runtime state。

特别是 Binding switch：

```text
old → Draining
new → Active
```

提交后，后续 final revalidation 必须能够观察到该 commit。

如果 Runtime 因同步错误仍把已经 Draining / non-current 的 Binding 当作 current Active：

> 属于 Runtime synchronization invariant violation，必须 fail closed。

---

### 4.1.1 Runtime Running 与 Gate Open

正常 lifecycle invariant 下：

```text
Global Gate Open
```

意味着 Runtime 已经处于可进行 normal admission 的 Running state。

因此：

```text
Runtime == Running
```

与：

```text
Global Gate == Open
```

在 normal path 中具有 defensive redundancy。

V1 仍要求 final revalidation 同时检查二者，原因是：

> 生命周期状态和 Gate 状态属于不同 operational state，双重检查可以捕获 Runtime internal publication / lifecycle invariant violation。

如果：

```text
Gate Open
+
Runtime not Running
```

则：

> 不是普通 PreAdmissionRejected，而是 Runtime lifecycle invariant violation，必须 fail closed。

---

### 4.1.2 Storage Ready

正常情况下：

```text
Global Gate == Open
```

已经隐含：

```text
Storage ready
```

因此 final revalidation 中再次检查 Storage ready：

> 是 defensive invariant check。

如果出现：

```text
Global Gate Open
+
Storage not ready
```

则属于：

> Runtime lifecycle invariant violation，

而不是普通 Data Plane rejection。

Runtime 必须 fail closed。

---

## 4.2 Per-stream Commit Serialization

同一 Stream 的以下操作必须处于同一个：

> **per-stream Admission Commit serialization domain**

中：

```text
final sequence availability check

Gate pass

Admission in-flight entry

AcceptanceSequence allocation

ordered enqueue ordering decision
```

V1 最简单实现可以是：

```text
per-stream short mutex
```

但不强制使用 mutex。

---

## 4.3 Sequence Availability 与 Allocation

AcceptanceSequence：

> 是 per-Stream monotonic non-reusable sequence。

Sequence availability check 和 sequence allocation：

> **必须在同一个 per-stream Admission Commit critical section 中完成。**

其间不允许其他同 Stream submission 的 commit 插入。

正确关系为：

```text
enter per-stream commit
        ↓
check next sequence exists
        ↓
final Gate / state revalidation
        ↓
Gate pass
        ↓
in-flight entry
        ↓
allocate exactly that next sequence
        ↓
establish ordered enqueue decision
```

因此不会出现：

```text
A checks N available
B checks N available
A allocates N
B allocates N
```

---

## 4.4 Admission Gate

Admission Gate 是：

> Runtime lifecycle 与 submission 之间的 admission-domain boundary。

逻辑状态：

```text
Open
Closed
```

Gate：

- 不是 Canonical object；
- 不持久化；
- 不要求独立 class。

---

## 4.5 Gate-pass Linearization

Gate pass 必须与：

```text
Gate Close
```

具有唯一 linearization relation。

任一 racing submission 必须唯一属于：

```text
Gate passed before cutoff
```

或：

```text
did not Gate pass before cutoff
```

不存在：

```text
partially admitted
```

状态。

---

## 4.6 Gate Enter 与 In-flight Accounting

一次成功 Gate pass 逻辑上同时建立：

```text
Admission Attempt exists

global admission in-flight +1

per-Binding in-flight +1
where required

per-Stream in-flight +1
where required
```

这些状态变化必须组成一个：

> **logical admission-entry transaction**

对其他 Runtime synchronization domain 不得暴露：

```text
attempt exists
but in-flight accounting
does not yet include it
```

这样的中间状态。

实现可以用：

```text
one lock-protected critical section
atomic state transition
carefully ordered atomic operations
```

完成，但必须保证 Stop / Binding retirement barrier 不会漏掉已 Gate-pass attempt。

---

## 4.7 Gate Close

Stop 或 Runtime-wide failure 建立 cutoff 时：

```text
Gate → Closed
```

从该 linearization point 后：

> 不允许创建新的 Admission Attempt。

已经 Gate pass 的 attempt：

> 继续 terminalization。

Gate close 本身：

> 不修改已经存在的 Admission Attempt history。

---

## 4.8 AcceptanceSequence Allocation

Gate pass 后，Runtime 立即分配已在同一 per-stream commit 中确认可用的：

```text
next AcceptanceSequence
```

逻辑关系为：

```text
sequence availability confirmed
while holding per-stream commit
        ↓
Gate pass
        ↓
Admission Attempt exists
        ↓
in-flight entry
        ↓
AcceptanceSequence allocated
```

不得：

```text
allocate sequence
        ↓
later discover Gate was closed
        ↓
rollback sequence
```

---

## 4.9 Sequence Exhaustion

如果当前 Stream 已无合法 next sequence：

```text
Stream admission eligibility closes
        ↓
Stream → Unavailable
```

submission：

```text
PreAdmissionRejected
```

其他 Stream 可以继续工作。

Sequence 不允许：

```text
wrap
reset
reuse
```

并且按照 Runtime v0.5 的 sequence-exhaustion contract：

> **该 Stream 在当前 active Identity Scope 中不能通过 new Binding、Plugin recovery、Source recovery 或其他运行时恢复操作重置 AcceptanceSequence。**

Sequence exhaustion：

> 是当前 Scope 内该 Stream 的 terminal admission condition。

如果需要继续该逻辑采集：

> 必须进入新的 Runtime activation / Identity Scope。

---

## 4.10 Sequence 不回滚

AcceptanceSequence 一旦分配：

> 永不 rollback。

例如：

```text
seq 10 → Accepted
seq 11 → AdmissionRejected
```

即使 Runtime 随后 fail closed：

```text
11
```

仍然保持 consumed。

Canonical history 不会把它重新解释为 pre-admission rejection。

---

## 4.11 Gate-pass 后正常路径

Gate pass 前已经完成：

```text
bounded resource guarantee
Payload materialization
structural validation
```

因此 Gate pass 后的 normal path 必须：

> **bounded、non-blocking，并且不执行普通可能失败的 resource acquisition。**

典型路径：

```text
Gate pass
    ↓
AcceptanceSequence
    ↓
AcceptedRecord finalize
    ↓
ordered enqueue publication
    ↓
Accepted
```

---

## 4.12 Gate-pass 后不得进行普通 Backpressure Wait

禁止：

```text
Gate pass
        ↓
wait for queue capacity
```

或者：

```text
Gate pass
        ↓
wait for Payload budget
```

这些普通 resource condition 必须在 Gate pass 前解决。

---

## 4.13 AcceptedRecord Construction

accepted candidate AcceptedRecord 至少包含：

```text
BindingHandle
AcceptanceSequence
optional SourceHandle
optional SourceSequence
TimestampStateSlots
PayloadRef
```

Runtime 写入：

```text
rodaq/acquisition_ingress
```

对应 slot：

```text
Present(captured_ingress_reading)
```

---

### 4.13.1 Plugin Timestamp Merge

Plugin 只提供：

```text
non-ingress timestamp slots
```

Runtime：

1. 根据 Binding slot mapping 验证；
2. 保留 Plugin 提供的 Present / Absent；
3. 写入 mandatory ingress slot。

Plugin 不得覆盖 ingress slot。

---

## 4.14 Accepted Linearization

V1 定义：

> **AcceptedRecord 成功完成 ordered enqueue publication 的时刻，是该 Admission Attempt 的 Accepted linearization point。**

在此之前：

> downstream consumer 不得把该 Record 作为 Accepted AcceptedRecord 观察。

在此之后：

```text
Accepted
```

不可撤销。

Storage failure、Runtime shutdown 或 persistence failure：

> 均不得将已经 Accepted 的 Record 重写为 AdmissionRejected。

---

## 4.15 AdmissionRejected

由于 Gate pass 前已经完成正常资源准备，Gate pass 后：

> **AdmissionRejected 不是正常 backpressure、Storage failure 或 resource-exhaustion 路径。**

它只允许发生在：

> Admission / Runtime commit path 本身已经无法可信完成 Accepted publication

的 Runtime internal failure，例如：

```text
AcceptedRecord invariant failure

ordered enqueue publication
cannot complete despite
reserved capacity

in-flight / sequence /
ordering invariant failure

Admission commit health
cannot be established

other Admission correctness failure
before Accepted publication
```

出现上述情况：

```text
AcceptanceSequence remains consumed

attempt terminal outcome
= AdmissionRejected
```

并且 Runtime 必须：

> **确保处于 fail-closed / failure-shutdown 状态。**

如果 Runtime 尚未进入 failure path：

```text
close Admission Gate
        ↓
enter Runtime failure path
```

如果 Runtime 已经处于：

```text
fail-closed
or
failure-shutdown
```

则：

> 不重复触发新的 lifecycle transition，只需完成该 attempt 的 terminalization 和 failure cleanup。

---

### 4.15.1 Storage Failure 不是 AdmissionRejected 原因

fatal Storage failure：

> **本身不是 AdmissionRejected 的原因。**

原因是已经 Gate-pass 的 Admission Attempt 完成 Accepted commit 所依赖的是：

```text
reserved Runtime resources
AcceptedRecord construction state
ordered enqueue path
```

而不是 Storage persistence success。

因此，只要 Admission subsystem 自身仍可证明健康：

```text
fatal Storage failure occurs
        ↓
Gate already passed attempt
        ↓
finish ordered enqueue
        ↓
Accepted
```

是强制要求。

即：

> **当 Admission commit path 可证明健康时，Gate-passed attempt MUST Accepted。**

不得因为：

```text
Runtime already Failed
Storage already Failed
shutdown already started
```

本身而把正常 Gate-passed attempt 标记为 AdmissionRejected。

随后该 Record 可能：

```text
never be handed off successfully
or
PersistenceFailed
or
PersistenceUnknown
```

这些都属于：

> Storage / persistence outcome。

不得改写 Admission history。

---

## 4.16 Terminal Accounting

Admission Attempt 达到 terminal outcome 时：

```text
Accepted
or
AdmissionRejected
```

必须从 admission in-flight set 中移除。

---

### Accepted

Accepted path 的逻辑顺序为：

```text
ordered enqueue publication
    ← Accepted linearization point
        ↓
accepted count +1
        ↓
per-Binding / per-Stream
in-flight decrement
        ↓
global in-flight decrement
```

Runtime 必须保证：

> global in-flight decrement 的可见性 happens-after accepted publication 和 accepted count 更新的可见性。

C++17 实现中必须建立等价的 happens-before relation。

观察 Stop / failure barrier 的线程通过 acquire 观察到：

```text
global in-flight == 0
```

时，必须同时能够观察到：

```text
all corresponding
Accepted AcceptedRecords published

accepted count fully updated
```

---

### AdmissionRejected

AdmissionRejected path：

```text
terminal rejection established
        ↓
admission rejected count +1
        ↓
failure-cleanup ownership reconciliation
        ↓
per-Binding / per-Stream
in-flight decrement
        ↓
global in-flight decrement
        ↓
ensure Runtime is in
fail-closed / failure-shutdown state
```

如果 Runtime 已经处于 failure-shutdown path：

> 不重复触发新的 fail-closed lifecycle transition。

同样不得：

```text
decrement in-flight
        ↓
later establish terminal outcome
```

---

# 5. Per-stream Ordering 与 Queue Publication

## 5.1 Ordering Requirement

同一 Stream 必须满足：

```text
AcceptanceSequence order
        ==
accepted enqueue order
        ==
Storage observed handoff order
```

Admission 负责：

```text
AcceptanceSequence
        ↓
accepted enqueue order
```

Runtime Storage Dispatch 负责保持：

```text
accepted enqueue
        ↓
Storage handoff
```

---

## 5.2 Concurrent Same-stream Submission

同一 Stream 可以由多个 callback thread 并发 submit。

它们可以并发执行：

```text
validation
timestamp capture
resource wait
Payload materialization
```

但最终：

```text
Admission Commit
```

必须按 per-stream serialization 进入。

---

## 5.3 Ordered Commit

例如：

```text
Submission A
Submission B
Submission C
```

并发准备完成。

per-stream Admission Commit 可以产生：

```text
B → seq 20
A → seq 21
C → seq 22
```

这是合法的。

accepted enqueue 必须随后为：

```text
20
21
22
```

不能：

```text
20
22
21
```

---

## 5.4 AcceptanceSequence 表示 Admission Commit Order

AcceptanceSequence 表示：

> per-stream admission ordering。

它不表示：

```text
native callback start order
SourceSequence
ingress timestamp order
Payload materialization completion order
```

本文中的：

> **successful Admission Commit**

特指 submission 在 §4.3 定义的 per-stream serialization domain 中成功完成：

```text
final revalidation
+
Gate pass
+
in-flight entry
+
AcceptanceSequence allocation
```

并建立对应 ordered enqueue decision。

因此：

> 哪个 submission 先完成上述 successful Admission Commit，哪个获得较小 AcceptanceSequence。

后续 AcceptedRecord publication 必须保持该 sequence order。

---

## 5.5 Resource Reservation 不决定 AcceptanceSequence

较早取得：

```text
Payload budget
AcceptedRecord capacity
dispatch capacity
```

不意味着必须取得较小 AcceptanceSequence。

Sequence 只在：

```text
per-stream Admission Commit
```

中确定。

---

## 5.6 Payload Materialization Completion 不决定 Sequence

例如：

```text
A copy completes first
B copy completes second
```

但如果：

```text
B enters and completes
Admission Commit first
```

则：

```text
B sequence < A sequence
```

合法。

---

## 5.7 Queue Reservation 与 Ordered Publication

这是 V1 实现必须特别注意的边界：

> **capacity reservation 不能隐式破坏 per-stream sequence order。**

推荐的 V1 实现路径是：

```text
capacity reservation
    only decrements /
    reserves capacity

queue position
    allocated at ordered commit
```

即：

> capacity 与 visible queue position 分离。

这样 per-stream Admission Commit 可以直接建立：

```text
sequence order
==
publication order
```

---

### 5.7.1 其他允许的实现方式

如果 queue implementation 本身需要提前分配 physical ticket / slot，也可以使用：

#### Per-stream ordered staging

```text
capacity reserved
        ↓
Record enters per-stream ordered staging
        ↓
single dispatch lane consumes
```

#### Reservation + reorder barrier

```text
physical position reserved
        ↓
sequence assigned later
        ↓
publication held until
earlier same-Stream sequence
is publishable
```

#### Deferred publication

```text
reservation creates
non-visible pending entry
        ↓
per-stream commit determines order
        ↓
entry becomes visible
```

任何实现方式都必须证明：

```text
same-Stream visible enqueue order
==
AcceptanceSequence order
```

不得依赖：

```text
single Storage dispatch lane
```

来“事后修复”已经乱序的 enqueue。

如果提前分配 physical position 的 queue：

> 无法证明上述 ordering invariant，

则实现必须退回：

> **capacity reservation 与 visible queue position 分离**

这一推荐模型，或者更换 queue implementation。

不得通过弱化 per-stream ordering guarantee 解决实现困难。

---

## 5.8 Cross-stream Ordering

不同 Stream 之间：

> 不定义 global admission order。

例如：

```text
Stream A seq 100
Stream B seq 5
```

之间不存在 Runtime global ordering relation。

V1 不引入：

```text
GlobalSequence
```

---

## 5.9 Publication Safety

AcceptedRecord 必须在：

```text
fully initialized
```

之后才能 publish 到 dispatch path。

需要至少等价于：

```text
release publication
acquire consumption
```

的 happens-before guarantee。

Storage Dispatch 不得观察：

```text
partially constructed AcceptedRecord
```

---

# 6. Lifecycle、Cutoff 与 Failure

## 6.1 Normal Runtime Eligibility

Normal admission 至少需要：

```text
Runtime Running

Global Gate Open

Storage ready

Stream Available

Binding Active/current

Source eligible

Sequence available
```

任一条件不满足：

> Gate pass 不得发生。

---

## 6.2 Stop Cutoff

Application 请求 Stop 时：

```text
Running
    ↓
Stopping
```

Runtime 关闭 Admission Gate 并建立：

```text
final admission cutoff
```

Gate close 与 concurrent Gate pass 必须具有唯一 linearization。

---

## 6.3 Gate-passed Attempt in Normal Stop

如果 submission：

```text
successfully Gate passes
before normal Stop cutoff
```

则正常设计下：

> **该 attempt 必须最终 Accepted。**

原因是 Gate pass 前已经完成：

```text
ordinary resource guarantee
Payload materialization
validation
```

而 Gate pass 后正常路径：

```text
bounded
non-blocking
allocation-free
```

因此 normal Stop 本身：

> 不会把一个合法 Gate-passed attempt 转化为 AdmissionRejected。

如果某个 Gate-passed attempt 在 Stop 期间达到：

```text
AdmissionRejected
```

则说明发生了：

> Runtime internal correctness failure。

此时：

```text
Runtime must fail closed
```

normal Stop 不得继续报告：

```text
Stopped
```

success。

---

## 6.4 Submission After Cutoff

如果 submission 未在 cutoff 前成功 Gate pass：

```text
PreAdmissionRejected
```

不分配 AcceptanceSequence。

---

## 6.5 Pre-admission Waiters

Stop / failure cutoff 后：

> 所有仍在 resource wait 且尚未 Gate pass 的 submission 必须被唤醒或取消。

结果：

```text
PreAdmissionRejected
```

如果 waiter 已取得 partial reservation：

> 必须按照 §2.6.2 rollback。

ordinary resource waiter 按当前 phase ordering 正常情况下尚未完成 Payload materialization；如果某个 implementation 在等价语义下已经建立 temporary PayloadRef，则仍必须按 §2.2.1 精确 cleanup，但不得改变“wait 发生在 Gate pass 前”的语义。

### 6.5.1 Active Submission Calls

Admission 必须与 Runtime v0.5 的：

```text
active_submission_calls
```

集成。

它覆盖：

```text
Submit entry
    ↓
structural validation
    ↓
ingress capture
    ↓
resource wait / reservation
    ↓
Payload materialization
    ↓
optional Gate pass / commit
    ↓
terminal admission result
    ↓
Submit return
```

因此：

```text
active_submission_calls
!=
global admission in-flight
```

Stop / failure cleanup 在释放 Submit-path Runtime state 以前必须同时证明：

```text
no new Submit can begin
+
active_submission_calls == 0
```

该 accounting 是 Runtime API lifetime safety，不改变 Canonical Admission Attempt 语义，也不消耗 AcceptanceSequence。

---

## 6.6 Global Admission In-flight

Runtime 维护：

```text
global admission in-flight count
```

定义为：

> 已 Gate pass，但尚未达到 Accepted 或 AdmissionRejected terminal outcome 的 attempt 数量。

增加时机：

```text
Gate Enter
```

减少时机：

```text
terminal outcome publication
```

具体 terminal accounting 规则见 §4.16。

---

## 6.7 Per-Binding / Per-Stream In-flight

Runtime 在 Binding retirement 和 Stream barrier 需要时维护：

```text
per-Binding in-flight count

per-Stream in-flight count
```

它们与 global in-flight 使用同样 terminalization 规则：

```text
Gate pass
    → increment

terminal outcome
    → decrement
```

Binding：

```text
Active → Draining
```

后：

- 不允许新的 Gate pass；
- 已 Gate-pass attempt 继续完成；
- per-Binding in-flight == 0 后才有资格 Retired。

---

## 6.8 Admission Shutdown Barriers

Normal Stop 与 failure shutdown：

> 必须使用不同的 admission barrier 语义。

---

### 6.8.1 Normal Stop Barrier

Normal Stop 必须满足：

```text
Gate Closed
        ↓
all pre-gate waiters cancelled / returned
        ↓
Acquisition quiescence prevents new Submit
        ↓
active_submission_calls == 0
        ↓
all Gate-passed attempts Accepted
        ↓
global admission in-flight == 0
```

之后才能确认：

> Admission domain 已正常收敛。

如果出现：

```text
AdmissionRejected
```

则 normal Stop 已经升级为 failure path。

---

### 6.8.2 Failure Shutdown Barrier

Failure shutdown 必须满足：

```text
Gate Closed
        ↓
all pre-gate waiters cancelled / returned
        ↓
no new Submit can begin
        ↓
active_submission_calls == 0
        ↓
all Gate-passed attempts terminal
        │
        ├── Accepted
        └── AdmissionRejected
        ↓
global admission in-flight == 0
```

随后 Runtime：

```text
Stopping → Failed
```

或保持：

```text
Failed
```

terminal state。

Failure shutdown 允许：

```text
AdmissionRejected
```

作为 terminal outcome，

但仅限于：

> Admission / Runtime commit path 已无法可信完成 Accepted publication。

fatal Storage failure 本身：

> 不使 Gate-passed attempt AdmissionRejected。

---

## 6.9 为什么 Gate-pass 后不能普通阻塞

Runtime shutdown 必须等待：

```text
all Gate-passed attempts terminal
```

因此 Gate pass 后如果仍允许：

```text
unbounded queue wait
unbounded Payload allocation wait
Storage I/O wait
```

会导致 shutdown 无法可靠收敛。

所以 V1 把 ordinary resource wait：

> 放在 Gate pass 前。

---

## 6.10 Binding Switch

Binding switch commit：

```text
old Binding → Draining
new Binding → Active
```

与 final revalidation 必须具有正确 memory-order / synchronization relation。

### 已 Gate-pass old-Binding attempt

只要 Admission commit path 可证明健康：

```text
MUST Accepted
```

### 尚未 Gate-pass old-Binding submission

final revalidation 必须观察到：

```text
old Binding not current
or
old Binding not Active
```

因此：

```text
PreAdmissionRejected
```

如果 switch 已经完成，但 final revalidation 仍错误地把 old Binding 视为 current Active：

> 属于 Runtime synchronization invariant violation，必须 fail closed。

---

## 6.11 Source / Stream Availability Change

如果 Source / Stream 在 resource wait 期间变为：

```text
Unavailable
```

final pre-gate revalidation 失败：

```text
PreAdmissionRejected
```

如果 availability change 发生在 Gate pass 后：

> 在 Admission commit path 可证明健康时，该 attempt MUST Accepted。

如果无法安全完成 Accepted：

> 只能因为 Admission / Runtime commit invariant 已不可信，此时按 AdmissionRejected + failure shutdown 处理。

---

## 6.12 AcceptanceSequence Exhaustion

Sequence exhaustion 必须在 Gate pass 前阻止新 attempt。

因此：

```text
sequence exhausted
        ↓
Stream Unavailable
        ↓
future submission
        ↓
PreAdmissionRejected
```

该状态在当前 active Identity Scope 内：

> 不可通过 Binding / Plugin / Source recovery 重置。

继续该 Stream 的采集需要：

> 新的 Runtime activation / Identity Scope。

---

## 6.13 Failure Cutoff 分类

Runtime-wide failure cutoff 必须区分：

```text
fatal downstream Storage failure
```

和：

```text
Runtime / Admission invariant failure
```

以及：

```text
Runtime failure outside
Admission commit path
```

因为三者对已经 Gate-pass attempt 的影响不同。

---

### 6.13.1 Fatal Storage Failure

fatal Storage failure：

```text
Storage reports fatal failure
        ↓
Control Plane closes Admission Gate
        ↓
establish failure cutoff
```

此时：

> Storage failure 不自动破坏 Admission commit path。

对于已经 Gate pass、但尚未 Accepted publication 的 attempt：

```text
Admission commit path
provably healthy
        ↓
MUST complete ordered enqueue
        ↓
Accepted
```

随后 Record 可能：

```text
remain queued

handoff fail

PersistenceFailed

PersistenceUnknown
```

但：

> 这些都是 Storage / persistence outcome。

不得：

```text
Accepted history
    → AdmissionRejected
```

也不得因为 Storage 已失败而取消一个已经 Gate-pass 且 Admission path 健康的 attempt。

---

### 6.13.2 Runtime / Admission Invariant Failure

如果 failure 来源于：

```text
Admission ordering corruption

in-flight accounting corruption

AcceptedRecord invariant failure

ordered enqueue publication failure

other Runtime invariant
that prevents trustworthy
Admission commit
```

则已经 Gate-pass、但尚未达到 Accepted linearization point 的 attempt：

> 可能无法安全完成 Accepted。

这种 attempt 可以：

```text
AdmissionRejected
```

并保持：

```text
AcceptanceSequence consumed
```

Runtime 已经处于 fail-closed / failure-shutdown path时：

> 不需要重复触发新的 fail-closed lifecycle transition。

只需：

```text
terminalize remaining attempts
        ↓
global admission in-flight == 0
        ↓
Failed
```

---

### 6.13.3 Runtime Failure Outside Admission Commit

如果某个 Runtime failure：

> 不影响 Admission commit path 的正确性，

并且 Runtime 可以证明：

```text
reserved resources valid
ordering state valid
in-flight accounting valid
AcceptedRecord construction valid
queue publication path valid
```

则已经 Gate-pass 的 attempt：

> **MUST 完成 Accepted。**

不能因为：

```text
Runtime entered Failed
shutdown already started
another subsystem failed
```

本身而把 Gate-passed attempt 标为 AdmissionRejected。

只有以下两种情况才允许偏离 `MUST Accepted`：

1. Admission commit path 已明确不可信；
2. Runtime failure 导致实现无法建立 Admission commit path 仍可信的证明。

第二种情况在语义上：

> 也必须把 Admission path 视为不可信。

因此可以：

```text
AdmissionRejected
```

并进入 / 保持 failure shutdown。

也就是说：

> `AdmissionRejected` 的判断依据始终是 Admission commit 是否还能可信完成，而不是 Runtime 是否已经进入 Failed 状态。

---

## 6.14 Failure After Gate Pass

如果 Gate pass 后出现 Admission-related Runtime correctness failure，且 AcceptedRecord 尚未完成 Accepted publication：

```text
AdmissionRejected

AcceptanceSequence remains consumed
```

随后：

```text
failure shutdown barrier
```

等待该 attempt 和其他 Gate-passed attempt terminalize。

如果 Record 已达到：

```text
Accepted linearization point
```

则：

> 永远不能重新解释为 Rejected。

后续问题属于 Storage / persistence failure。

---

# 7. Interface 与实现边界

## 7.1 Acquisition-facing Submission

本文不冻结具体 C++ API。

逻辑上 Acquisition Plugin 调用：

```text
Submit(
    BindingHandle,
    optional SourceHandle,
    optional SourceSequence,
    TimestampState,
    PayloadInput
)
```

Runtime 返回 terminal admission result。

整个调用从 Runtime entry 到 return 都必须处于 `active_submission_calls` lifetime accounting 中；该 bookkeeping 可以是 Runtime internal RAII / scope guard，不要求暴露给 Plugin API。

---

## 7.2 Logical Admission Outcomes

V1 至少区分：

```text
PreAdmissionRejected
AdmissionRejected
Accepted
```

### PreAdmissionRejected

```text
Gate pass never happened
AcceptanceSequence not consumed
AcceptedRecord not Accepted
```

### AdmissionRejected

```text
Gate pass happened
AcceptanceSequence consumed

AcceptedRecord not Accepted

Admission commit path
not trustworthy
```

AdmissionRejected：

> 不是普通 runtime pressure、Storage failure 或 expected Plugin failure 的结果。

### Accepted

```text
Gate pass happened
AcceptanceSequence consumed
AcceptedRecord ordered-enqueue published
```

---

## 7.3 Rejection Reason

Runtime SHOULD 提供 operational rejection reason。

典型类别：

```text
InvalidInput

RuntimeNotRunning

GateClosed

StaleHandle

BindingNotActive
BindingNotCurrent

StreamUnavailable
SourceUnavailable

SequenceExhausted

PayloadInvalid
PayloadTooLarge

ResourceUnavailable
WaitTimeout

RuntimeFailure
```

Payload materialization failure：

### Gate pass 前

如果因为：

```text
invalid Payload / anchor
```

归入：

```text
PayloadInvalid
```

如果因为：

```text
allocation / capacity unavailable
```

归入：

```text
ResourceUnavailable
```

### Gate pass 后

V1 正常设计中：

> 不应再发生 Payload materialization。

如果 Gate pass 后仍发生所谓：

```text
Payload materialization failure
```

说明实现违反 §2.9 的 contract。

必须归入：

```text
RuntimeFailure
```

并把 Admission path 视为不可信。

这些 reason：

> 不属于 Canonical Record metadata。

---

## 7.4 Runtime Responsibilities

Runtime Admission 至少负责：

```text
structural validation

ingress timestamp capture

resource reservation

reservation rollback

Payload integration

Gate linearization

AcceptanceSequence

AcceptedRecord construction

ordered enqueue

terminal accounting

cutoff wakeup

normal/failure barrier distinction
```

---

## 7.5 Acquisition Plugin Responsibilities

Acquisition Plugin 负责：

```text
provide correct Binding

provide registered SourceHandle
when needed

provide valid SourceSequence relation

provide valid Plugin timestamp slots

provide Payload input

respect submit result

handle PreAdmissionRejected

respect Binding switch Contract
```

如果 Plugin 不希望 callback 阻塞：

> 使用 Reject-style backpressure。

如果 native source 支持 backpressure：

> Plugin 可以把 Wait 进一步映射到 source-native mechanism。

---

## 7.6 Payload Boundary

Admission 必须遵守 `payload.md`：

```text
Copy materialization
within synchronous input lifetime

Adopt / Retain validation
before ownership acquisition

Payload stable
before Gate pass

exactly-once release
```

Admission 不重新定义 Payload ownership。

---

## 7.7 Storage Boundary

Admission 只保证：

```text
Accepted AcceptedRecord
has reserved dispatch capacity
and completion bookkeeping capacity
```

它不保证：

```text
successful Storage handoff

PayloadConsumed

PersistenceSucceeded

CrashDurability
```

fatal Storage failure：

> 不回写 admission result。

特别是：

```text
Gate-passed attempt
+
provably healthy Admission path
```

必须：

```text
Accepted
```

---

## 7.8 Testing Hooks

为验证精确 linearization 和 timestamp 时序，Runtime implementation SHOULD 提供 test-only instrumentation，例如：

```text
injectable ingress Clock

ingress capture hook

Gate-pass hook

sequence allocation hook

queue publication hook

allocator fault injection

resource reservation fault injection
```

这些 hook：

> 不属于 production API contract。

---

## 7.9 Implementation Restraint

本文中的：

```text
resource reservation
Gate
commit
in-flight accounting
ordered enqueue
```

都是逻辑职责。

V1 implementation 不应机械实现为：

```text
AdmissionManager
ReservationManager
AdmissionPermit
AdmissionToken
CommitCoordinator
SequenceCoordinator
BackpressureManager
```

一个合理实现可以只是：

```text
DAQ::submit(...)
```

配合：

```text
ActivationState::submit(...) orchestration
AdmissionState
BindingState
bounded ResourceBudget / PayloadPool / StoragePipeline
```

实现全部语义。

`AdmissionState` 必须真正拥有 Global Gate、active submission calls、admission
in-flight accounting、eligibility generation、Admission condition variable 与
AcceptanceSequence allocator。`ActivationState` 可以编排跨 Canonical、Binding、
Payload 与 Storage 的完整 Submit，但不得继续直接持有 Admission Gate internals
或 AcceptanceSequence allocator。

---

# 8. Invariants 与 Conformance

## 8.1 Admission Invariants

### Submission / Attempt

```text
Submission
does not become
Admission Attempt
until Gate pass
```

---

### Gate

```text
Gate pass
and
Gate close
have one unique
linearization order
```

---

### Gate Enter

```text
Admission Attempt existence

global in-flight entry

required per-Binding /
per-Stream in-flight entry
```

构成一个 logical admission-entry transaction。

---

### Sequence

```text
no Gate pass
⇒
no AcceptanceSequence

sequence availability check
and allocation occur
inside same per-stream commit

Gate pass
⇒
exactly one AcceptanceSequence

AcceptanceSequence
never rollback
never wrap
never reuse
```

Sequence exhaustion：

```text
terminal within
current active Identity Scope
```

不得通过运行时恢复重置。

---

### Resources

```text
ordinary Accepted-path
bounded resources
guaranteed before Gate pass
```

---

### Rollback

```text
pre-gate failure
or interruption

⇒

partial reservation rollback

must be noexcept
must not fail
must not leak resources
```

---

### Payload

```text
stable PayloadRef
exists before Gate pass
```

---

### Post-gate Progress

```text
normal Gate-passed path
does not block on:

Payload allocation
queue capacity
Storage I/O
ordinary resource wait
```

---

### Healthy Gate-passed Outcome

```text
provably healthy
Admission commit path
+
Gate-passed attempt
⇒
MUST Accepted
```

---

### Storage Failure

```text
fatal Storage failure
does not by itself cause
AdmissionRejected
```

Already Gate-passed attempts：

```text
MUST complete Accepted
when Admission path
is provably healthy
```

---

### AdmissionRejected

```text
AdmissionRejected
means Admission commit path
is not trustworthy
```

原因可以是：

```text
known invariant failure

or

Runtime can no longer
establish commit-path health
```

并且：

```text
AcceptanceSequence
remains consumed
```

---

### Accepted

```text
Accepted
only after
fully initialized AcceptedRecord
is ordered-enqueue published
```

---

### Terminal Accounting

```text
attempt leaves in-flight set
only after terminal outcome
is fully published
```

Accepted path：

```text
accepted publication
and accepted count visibility
happen-before
global in-flight decrement
visibility
```

AdmissionRejected path：

```text
terminal rejection
and failure cleanup state
established before
in-flight removal
```

---

### Reservation Failure Cleanup

```text
AdmissionRejected resources
must be reconciled
through failure cleanup

not blindly assumed
to be normally released
```

---

### Ordering

```text
same-Stream
AcceptanceSequence order
==
accepted enqueue order
```

---

### Storage Handoff

Runtime dispatch preserves：

```text
same-Stream
accepted enqueue order
```

---

### Normal Stop Barrier

```text
Gate Closed
+
all pre-admission waiters returned
+
all Gate-passed attempts Accepted
+
global in-flight == 0
```

---

### Failure Shutdown Barrier

```text
Gate Closed
+
all pre-admission waiters returned
+
all Gate-passed attempts terminal
+
global in-flight == 0
```

其中 terminal 可以是：

```text
Accepted
or
AdmissionRejected
```

---

### Cutoff

```text
Gate-passed before cutoff
cannot become
post-cutoff submission

not Gate-passed before cutoff
cannot become
Admission Attempt
```

---

### Storage Ready

```text
Global Gate Open
normally implies
Storage Ready
```

如果不成立：

> Runtime lifecycle invariant violation。

---

## 8.2 Required Conformance Scenarios

V1 implementation 至少必须覆盖以下测试。

### Runtime Not Running

验证：

```text
PreAdmissionRejected
no sequence
no AcceptedRecord
```

---

### Gate Closed

验证：

```text
PreAdmissionRejected
```

---

### Invalid Binding

分别测试：

```text
stale BindingHandle
Prepared Binding
Draining Binding
Retired Binding
non-current Binding
```

均不得 Gate pass。

---

### Stream Unavailable

验证：

```text
PreAdmissionRejected
no sequence
```

---

### Source Unavailable

验证：

```text
PreAdmissionRejected
```

---

### SourceSequence without Source

提交：

```text
SourceSequence present
SourceHandle absent
```

验证：

```text
PreAdmissionRejected
```

---

### Invalid Timestamp Slot

Plugin：

```text
writes ingress slot
```

或使用 undefined slot。

验证：

```text
PreAdmissionRejected
```

并产生 Acquisition Contract violation diagnostic。

---

### Ingress Timestamp Before Wait

通过 injectable ingress Clock / test hook：

```text
Phase 1 completes
        ↓
ingress captured
        ↓
resource wait
        ↓
Accepted
```

验证 AcceptedRecord 的 TimestampStateMap 中：

```text
rodaq/acquisition_ingress
```

reading 对应：

> resource wait 之前的 capture point。

---

### Ingress Timestamp vs Sequence — Both Accepted

构造：

```text
A captures earlier ingress
A waits

B captures later ingress
B commits first

A later commits
```

验证两条 AcceptedRecord 的 TimestampStateMap：

```text
A ingress < B ingress
```

同时：

```text
B AcceptanceSequence
<
A AcceptanceSequence
```

证明：

> timestamp 保留 capture order，而 AcceptanceSequence 表达 commit order。

---

### Ingress Timestamp vs Sequence — A Rejected

构造：

```text
A captures earlier ingress
A waits and times out

B captures later ingress
B Accepted
```

验证：

```text
A → PreAdmissionRejected
B → Accepted
```

只有 B 的 timestamp 进入 Canonical Record。

---

### Effective Payload Limit

配置：

```text
runtime_payload_limit = 4 MiB
storage_payload_limit = 1 MiB
effective_payload_limit = 1 MiB
```

提交 2 MiB Payload。验证：

```text
PreAdmissionRejected
no Gate pass
no AcceptanceSequence
no Storage handoff
```

不得等到 Storage Handoff 才发现 oversize。

---

### Scalar Domain / Native Sentinel

验证 Runtime-facing submission：

```text
SourceSequence present(0)
ClockReading = 0
ClockReading < 0
```

均按 Profile v0.5 的合法值处理。

对于 ROS2 / DDS 等 native unsupported sentinel：

> Acquisition Mapping 必须在调用 Runtime 前转换为 absent。

Admission 不得把 generic `UINT64_MAX` 自动解释为 absent。

---

### Immediate Resource Reject

耗尽 dispatch capacity，使用 Reject policy。

验证：

```text
PreAdmissionRejected
no sequence
```

---

### Resource Wait

耗尽 capacity，使用 Wait policy。

随后释放 capacity。

验证 submission 正常继续。

---

### Wait Interrupted with Partial Reservation

构造 sequential reservation：

```text
resource A reserved
resource B unavailable
submission waits
```

随后触发：

```text
Stop
or
failure cutoff
or
Binding invalidation
or
timeout
```

验证：

```text
wait interrupted
        ↓
resource A rolled back
        ↓
PreAdmissionRejected
```

不得：

```text
resource leak
orphan reservation
AcceptanceSequence allocation
```

---

### Payload Materialized then Final Revalidation Fails

构造：

```text
Phase 3 resources reserved
        ↓
Phase 4 Adopt / Retain materialized
        ↓
Runtime now owns Payload responsibility
        ↓
Stop / Binding invalidation happens
        ↓
final pre-gate revalidation fails
```

验证：

```text
no Gate pass
no AcceptanceSequence

Payload responsibility
released exactly once

remaining local reservations
rolled back

PreAdmissionRejected
```

该测试替代旧的“resource Wait 时已经持有 Payload responsibility”场景，因为 V1 v0.5 ordinary resource wait 明确位于 Payload materialization 之前。

---

### Active Submission Barrier

构造 submission A 已进入 `DAQ::submit`，仍处于 pre-gate validation / Copy materialization；此时：

```text
global admission in-flight == 0
```

触发 Stop，并让 Acquisition Plugin 完成 quiescence。验证：

- Runtime 不得因为 `global admission in-flight == 0` 提前 teardown；
- 必须等待 A 返回；
- 最终 `active_submission_calls == 0` 后才允许释放 Submit-path state；
- A 未 Gate-pass 时不消耗 AcceptanceSequence。

---

### Wait Interrupted by Stop

submission 正在 pre-admission wait。

Stop 后验证：

```text
wait wakes
PreAdmissionRejected
no sequence
```

---

### Wait Timeout

验证：

```text
partial reservations rolled back
PreAdmissionRejected
no sequence
```

---

### Reservation Rollback

分别在以下阶段 fault injection：

```text
after Payload budget reservation

after AcceptedRecord slot reservation

after dispatch capacity reservation

during Payload materialization
```

验证：

```text
all acquired reservations released

no resource leak

no sequence
```

---

### Rollback Failure Detection

通过 test-only faulty resource implementation 模拟 rollback failure。

验证：

> Runtime 识别 implementation invariant violation 并 fail closed。

正常 production resource implementation 不允许 rollback failure。

---

### Copy Failure

在 synchronous input lifetime 内注入 allocation failure。

验证：

```text
PreAdmissionRejected
```

并且 submit 返回后不读取 BorrowedInput。

---

### Adopt then Gate Close

Runtime 已取得合法 Adopt ownership。

final revalidation 发现 Gate Closed。

验证：

```text
PreAdmissionRejected
no sequence
Runtime releases anchor exactly once
```

---

### Gate / Stop Race

大量线程 concurrent submit，同时关闭 Gate。

每个 submission 必须唯一分类：

```text
Gate passed before cutoff
```

或：

```text
did not Gate pass before cutoff
```

不存在 ambiguity。

---

### Sequence Availability / Allocation Atomicity

把 sequence 推到最后一个可用值。

两个线程同时 submit。

验证：

```text
only one thread
can allocate final sequence
```

另一个：

```text
PreAdmissionRejected
```

不得：

```text
duplicate sequence
wrap
reuse
```

---

### Sequence Allocation

正常连续 Accepted：

```text
N
N+1
N+2
```

验证：

```text
unique
monotonic
no reuse
```

---

### Sequence Exhaustion Cannot Recover in Same Scope

耗尽某 Stream 的 AcceptanceSequence。

随后尝试：

```text
new Binding
Source recovery
Plugin recovery
```

验证：

```text
Stream remains
admission-unavailable
```

不得重置 sequence。

只有新的：

```text
Runtime activation
/
Identity Scope
```

可以重新建立该 Stream 的 sequence domain。

---

### AdmissionRejected + Fail Closed

注入 Gate pass 后的 Admission internal failure：

```text
ordered enqueue publication failure
or
AcceptedRecord invariant failure
```

验证：

```text
seq N
    → Accepted

seq N+1
    → AdmissionRejected
```

随后：

```text
Runtime fail closed
Gate Closed
```

后续 submission：

```text
PreAdmissionRejected
```

不得出现：

```text
seq N+2 → Accepted
```

---

### AdmissionRejected while Already Failing

Runtime 已经处于 failure-shutdown。

某 Gate-passed attempt 因 Admission invariant failure：

```text
→ AdmissionRejected
```

验证：

- 不重复触发新的 lifecycle failure transition；
- rejection count 更新；
- failure cleanup reconciliation 完成或进入 quarantine；
- in-flight 正确减少。

---

### AdmissionRejected Reservation Reconciliation

注入 commit failure，并模拟：

```text
some resources still local
some resources already transferred
```

验证：

- local safe reservation 被释放；
- 已转移 resource 由实际 owner 接管；
- 无法证明安全释放的 resource 不被伪装成 released；
- 必要时进入 quarantine；
- 不发生 double release / UAF。

---

### Sequence Gap Persistence

在上述 failure 场景中验证：

```text
N+1 remains consumed
```

不会因为 Runtime failure 而 rollback。

---

### Same-stream Concurrent Submit

多个线程同时向一个 Stream submit。

故意打乱：

```text
validation completion
Payload materialization completion
```

最终验证：

```text
AcceptanceSequence:
10
11
12

accepted enqueue:
10
11
12
```

---

### Successful Admission Commit Definition

多个同 Stream submission 并发进入 commit。

验证 AcceptanceSequence 顺序由：

```text
final revalidation
+
Gate pass
+
in-flight entry
+
sequence allocation
```

这一 serialized commit 顺序决定，

而不是由：

```text
callback arrival
Payload materialization completion
resource reservation order
```

决定。

---

### Queue Reservation Without Position Allocation

推荐 V1 queue 模式下：

```text
capacity reservation
```

不提前分配 visible position。

验证 commit 顺序直接决定 publication order。

---

### Queue Reservation with Physical Ticket

如果实现使用提前 physical ticket：

> 必须测试其 reorder / deferred-publication mechanism。

验证：

```text
visible same-Stream enqueue
==
AcceptanceSequence order
```

如果实现无法证明这一点：

> 测试必须失败，且实现应退回 capacity/position 分离模型。

不得把 single dispatch lane 当作修复乱序的机制。

---

### Cross-stream Parallelism

多个 Stream 同时高速 submit。

验证：

- 不依赖 global per-record Admission Commit lock；
- 每个 Stream 独立保持 sequence/order；
- 不要求 cross-stream total order。

---

### AcceptedRecord Publication

consumer 并发 dequeue。

验证：

> 永远看不到 partial AcceptedRecord。

---

### Binding Switch Race

旧 Binding submission 正在 pre-admission wait。

Binding switch：

```text
old → Draining
new → Active
```

后，旧 submission final revalidation：

```text
PreAdmissionRejected
```

已 Gate-pass old-Binding attempt，且 Admission path 健康：

```text
MUST Accepted
```

---

### Binding Switch Happens-before

通过 test synchronization 强制 switch commit 先完成，再执行 final revalidation。

验证：

> revalidation 必须看到 old Binding 已 Draining / non-current。

若仍看到旧 Active/current：

> 测试失败，属于 synchronization bug。

---

### Source Failure Race

Gate pass 前：

```text
Source → Unavailable
```

验证：

```text
PreAdmissionRejected
```

Gate pass 后：

```text
Source → Unavailable
```

且 Admission subsystem 可证明健康时：

```text
MUST Accepted
```

---

### Normal Stop with Gate-passed Attempt

制造 attempt 已 Gate pass、尚未 publish。

执行 normal Stop。

验证：

```text
attempt → Accepted
```

之后：

```text
in-flight == 0
```

Stop 可以继续。

---

### Gate-passed Internal Failure During Stop

Gate pass 后注入 Admission internal commit failure。

验证：

```text
AdmissionRejected
Runtime fail closed
```

normal Stop：

> 不得最终报告 `Stopped` success。

---

### Fatal Storage Failure with Gate-passed Attempt

构造：

```text
Admission Attempt
already Gate-passed

AcceptedRecord not yet
ordered-enqueue published
```

此时触发：

```text
fatal Storage failure
```

验证：

```text
failure cutoff closes Gate

Admission path remains healthy

existing Gate-passed attempt
MUST continue Admission commit

attempt → Accepted
```

随后：

```text
Storage handoff /
persistence may fail
```

但：

```text
Accepted history remains Accepted
```

Runtime 最终：

```text
Failed
```

failure cutoff 后的新 submission：

```text
PreAdmissionRejected
```

---

### Runtime Failure Outside Admission Commit — Healthy Path

触发一个：

```text
Runtime subsystem failure
```

但通过 test instrumentation 证明：

```text
Admission ordering valid
in-flight accounting valid
queue publication valid
reserved resources valid
```

验证已经 Gate-pass 的 attempt：

```text
MUST Accepted
```

不得仅因为 Runtime 已进入 failure state 而 AdmissionRejected。

---

### Runtime Failure Outside Admission Commit — Health Unknown

触发 Runtime failure，使实现无法再证明：

```text
Admission commit path healthy
```

验证：

> Runtime 可以把尚未 Accepted 的 Gate-passed attempt terminalize 为 AdmissionRejected。

同时：

```text
AcceptanceSequence remains consumed
Runtime remains in failure shutdown
```

---

### Fatal Storage Failure Admission Barrier

构造多个：

```text
Gate-passed healthy attempts
```

并触发 fatal Storage failure。

验证 failure shutdown admission barrier：

```text
Gate Closed

all pre-admission waiters returned

all healthy Gate-passed attempts
Accepted

global in-flight == 0
```

随后进入 Storage failure shutdown。

---

### Runtime Admission Failure Barrier

构造：

```text
one Gate-passed attempt
hits Admission invariant failure
```

并同时存在其他 Gate-passed attempt。

验证：

```text
Gate Closed

all pre-admission waiters returned

each Gate-passed attempt
reaches terminal outcome

Accepted or AdmissionRejected

global in-flight == 0
```

Runtime 最终：

```text
Failed
```

---

### In-flight Terminal Accounting

对 Accepted attempt 验证：

当 Stop / failure observer 通过 acquire 看到：

```text
global in-flight == 0
```

时：

- AcceptedRecord 已 publish；
- accepted count 已包含该 Record。

对 AdmissionRejected 验证：

- rejection result 已建立；
- rejection count 已更新；
- failure cleanup ownership reconciliation 已建立；
- Runtime 已处于或保持 failure path。

---

### Per-Binding In-flight Retirement

Binding → Draining。

验证：

> 只有所有已 Gate-pass attempt terminal 后，per-Binding in-flight 才变 0 并允许 Retired。

---

### No Post-gate Queue Wait

instrument queue。

验证：

> Gate pass 后不等待 ordinary queue capacity。

---

### No Accepted Queue Drop

Storage 极慢并填满 dispatch path。

验证新 submission：

```text
Reject
or
Wait
before Gate pass
```

而不是：

```text
drop existing Accepted Record
```

---

### Storage-ready Invariant

故意构造非法 Runtime state：

```text
Gate Open
Storage not ready
```

验证：

> 识别为 Runtime lifecycle invariant violation，并 fail closed。

不得当作普通 ResourceUnavailable rejection。

---

### Runtime-running / Gate Invariant

故意构造：

```text
Gate Open
Runtime not Running
```

验证：

> 识别为 Runtime lifecycle invariant violation，并 fail closed。

---

### Failure Cutoff

触发 fatal Storage failure 或 Runtime failure。

并发 submission 必须按 cutoff 唯一分类。

cutoff 后：

```text
no new Gate pass
no new AcceptanceSequence
```

---

### Accepted Resource Transfer

成功 Accepted 后验证：

- dispatch reservation 转为 queued Record capacity；
- Payload lifetime 由 AcceptedRecord 持有；
- completion bookkeeping 保持到 Storage tracking terminal；
- 不发生重复释放。

---

## 8.3 V1 Admission Design Baseline

RoDAQ V1 Admission 主路径为：

```text
Acquisition Submission
        ↓
active_submission_calls +1
        ↓
Structural Validation
        ↓
Ingress Timestamp Capture
        ↓
Bounded Resource Reservation
        ↓
Payload Materialization
        ↓
Per-stream Admission Commit
        │
        ├── Final Revalidation
        ├── Sequence Availability Check
        ├── Gate Pass
        ├── In-flight Entry
        └── AcceptanceSequence
        ↓
AcceptedRecord Finalization
        ↓
Per-stream Ordered Enqueue
        ↓
Accepted
```

其中 Gate pass 前可以发生：

```text
validation

resource wait

backpressure

Payload allocation

Copy

Adopt / Retain acquisition
after Payload resource reservation

reservation rollback
```

任何 pre-gate：

```text
failure
interruption
timeout
cutoff
state invalidation
```

都必须：

```text
rollback partial reservation
        ↓
cleanup Payload responsibility
if materialization already occurred
        ↓
PreAdmissionRejected
        ↓
Submit return / active_submission_calls -1
```

Gate pass 后：

```text
Admission Attempt exists

AcceptanceSequence consumed
```

健康 normal path：

```text
bounded
non-blocking
allocation-free
```

并且：

```text
provably healthy
Admission commit path
+
Gate-passed attempt
    → MUST Accepted
```

如果 Gate pass 后 Admission commit path 本身已经不可信，或者 Runtime 无法再建立其可信性：

```text
AdmissionRejected
        ↓
AcceptanceSequence remains consumed
        ↓
ensure Runtime is fail closed
```

因此：

> AdmissionRejected 不是普通 backpressure、Storage failure 或 expected runtime pressure 的结果。

V1 backpressure 只采用：

```text
Reject
or
Wait
```

并发生在 Gate pass 前。

Runtime 不采用：

```text
drop-oldest Accepted Record
```

作为正常 queue-pressure policy。

同一 Stream 的顺序保证链为：

```text
per-stream Admission Commit
        ↓
AcceptanceSequence
        ↓
ordered accepted enqueue
        ↓
bounded dispatch path
        ↓
single Storage Dispatch Lane
        ↓
Storage observed handoff
```

满足：

```text
AcceptanceSequence order
==
accepted enqueue order
==
same-Stream Storage handoff order
```

`rodaq/acquisition_ingress` 在：

```text
Phase 1 validation complete
```

之后、任何可能阻塞的 resource wait 之前捕获。

因此：

```text
ingress timestamp order
```

与：

```text
AcceptanceSequence order
```

彼此独立。

Normal Stop 使用：

```text
close Gate
        ↓
wake pre-gate waiters
        ↓
Acquisition quiescence prevents new Submit
        ↓
rollback partial reservations / pre-gate Payload cleanup
        ↓
active_submission_calls == 0
        ↓
finish healthy Gate-passed attempts
        ↓
all Gate-passed attempts Accepted
        ↓
global admission in-flight == 0
```

进行收敛。

如果 normal Stop 中出现 AdmissionRejected：

> Runtime 已发生 Admission-related correctness failure，Stop 升级为 failure shutdown。

Failure shutdown 使用：

```text
close Gate
        ↓
wake pre-gate waiters
        ↓
prevent new Submit
        ↓
rollback pre-gate reservations / Payload cleanup
        ↓
active_submission_calls == 0
        ↓
terminalize every
Gate-passed attempt
        │
        ├── Accepted
        └── AdmissionRejected
        ↓
in-flight == 0
        ↓
Failed
```

其中必须特别区分：

```text
fatal Storage failure
```

与：

```text
Admission commit
invariant failure
```

以及：

```text
Runtime failure outside
Admission commit
```

fatal Storage failure：

> 不破坏 Admission history。

只要 Admission path 可证明健康：

```text
already Gate-passed
    → MUST Accepted
```

Runtime 其他 subsystem failure：

> 同样不能仅因为 Runtime 已进入 failure state 而拒绝 Gate-passed attempt。

如果 Admission path 可证明健康：

```text
MUST Accepted
```

只有 Admission commit：

```text
known broken
```

或者：

```text
cannot be proven trustworthy
```

时，才允许：

```text
AdmissionRejected
```

Sequence exhaustion：

```text
Stream-local terminal
within current active Scope
```

不能通过 Binding / Plugin / Source recovery 重置。

V1 queue capacity reservation 必须对应真实、已经可用的 physical enqueue capacity，而不仅是逻辑 counter。

也就是说，在 Gate pass 前取得 `dispatch capacity` permit 时，Runtime 必须已经能够证明后续 ordered enqueue 不再需要可能失败的普通动态分配。

允许的实现例如：

```text
preallocated fixed-capacity ring / slot array
preallocated node pool + intrusive/index queue
other transport with mechanically proven no-allocation enqueue after reservation
```

不允许：

```text
reserve logical dispatch counter
    ↓
Gate pass
    ↓
underlying queue push allocates a new page/node
    ↓
allocation fails
```

这种实现会破坏 Accepted no-drop contract。

oneTBB 或其他第三方 queue 只有在能够证明：

```text
all post-reservation enqueue resources are already physically reserved
```

时才能承担 correctness-critical dispatch transport；否则只能作为 benchmark/reference，不作为 V1 Admission correctness baseline。

V1 推荐 queue capacity reservation：

> 与 visible queue position 分离。

即：

```text
reserve capacity
        ↓
sequence assigned in
per-stream commit
        ↓
publish queue position
in sequence order
```

这样不需要额外复杂 reorder subsystem。

最终，一个紧凑 C++17 implementation 可以保持为：

```text
Submit()
    ↓
active submission accounting
    ↓
validate effective_payload_limit / scalar structure
    ↓
validate
    ↓
capture ingress time
    ↓
reserve
    ↓
materialize Payload
    ↓
per-stream short commit
    ↓
enqueue
```

配合：

```text
StreamState
Admission Gate
bounded resource counters
Payload subsystem
bounded dispatch queue
```

即可实现本文全部语义。

这形成一个：

> **具备主流 Acquisition / Storage Plugin 所需完整并发、backpressure、ordering、bounded-resource、cutoff 和 failure 能力，同时以明确的 pre-gate rollback、严格的 Gate-passed outcome 规则和最小实现抽象直接落地为 C++17 的 RoDAQ V1 Admission 模型。**

---

## M2 Convergence Admission Amendment

Ingress timestamp capture occurs after structural, handle, Binding, availability
and payload-limit validation but before any pre-Gate resource wait. A
PreAdmission rejection discards the captured timestamp state.

For Copy, the Pre-Gate resource set is:

```text
dispatch capacity
operation slot
completion bookkeeping capacity
logical Payload bytes
selected Payload size-class block
Payload release work
```

The copy is completed into the reserved block before final eligibility
revalidation and Gate Pass. For Adopt/Retain, ownership transfer occurs only
after the same non-Payload-block resources and release work are reserved.
Rollback is complete and non-failing before Gate Pass.

Eligibility generation changes from Source, Stream or Binding invalidation
wake resource waiters; final eligibility revalidation remains authoritative.
