# RoDAQ Payload Design Specification

**Version:** v0.3  
**Status:** Design Specification  
**Scope:** RoDAQ V1 Payload Representation, Ownership and Lifetime

**Normative Dependencies:**

- RoDAQ Canonical Data Model Design Specification v0.9
- RoDAQ V1 Canonical Profile v0.4
- RoDAQ System Architecture Design Specification v0.4
- RoDAQ Runtime Design Specification v0.4

---

# 1. 目标与设计边界

## 1.1 设计目标

RoDAQ Payload 设计定义 Payload 在：

```text
Acquisition Plugin
        ↓
Runtime
        ↓
Storage Plugin
```

之间传递时的：

```text
representation
ownership
lifetime
resource accounting
release
failure cleanup
```

规则。

Canonical Payload 在语义上始终是：

> **immutable opaque logical byte string**

Payload 设计必须保证：

- logical bytes 不发生静默修改或转换；
- RuntimeRecord 中的 Payload 在所有合法 consumer 使用期间始终有效；
- ownership 不存在歧义；
- release exactly once；
- asynchronous Storage 不产生 use-after-free；
- zero-copy 不能绕过 Runtime resource limit；
- failure path 不因提前释放 Payload 破坏 memory safety；
- Runtime Payload resource 始终有界。

Payload 主路径为：

```text
Native Payload
        ↓
Acquisition Plugin
        ↓
Payload Ingress
        ↓
PayloadRef
        ↓
RuntimeRecord
        ↓
Storage Payload Access
        ↓
PayloadConsumed
        ↓
Release
```

---

## 1.2 设计范围

本文定义：

```text
Canonical Payload 与 Runtime Payload 的关系

PayloadRef

V1 physical representation

Copy / Adopt / Retain

Acquisition → Runtime ownership boundary

Payload 与 Admission 的关系

Payload resource accounting

RuntimeRecord Payload lifetime

Runtime → Storage access

PayloadConsumed

zero-copy constraints

concurrency

normal stop

failure cleanup

conformance requirements
```

---

## 1.3 非设计范围

以下内容不由本文定义：

```text
AcceptanceSequence linearization
Admission queue algorithm
backpressure policy
Record dispatch queue implementation

Storage persistence-success semantics
Storage transaction model
Storage flush / close semantics

Artifact format
compression format

Schema interpretation
PayloadEncoding interpretation
```

这些分别由：

```text
admission.md
storage_plugin.md
```

等下游设计定义。

---

## 1.4 术语

### Synchronous Input Lifetime

`synchronous input lifetime` 指：

> **从 Runtime 的 submit operation 开始读取对应 input，到该 submit operation 返回之前的同步调用期间。**

对于没有独立 lifetime anchor 的 `BorrowedInput`：

> Runtime 不得在 submit operation 返回后继续读取 input bytes。

---

### Structurally Valid Anchor

`structurally valid anchor` 指 external ownership/lifetime anchor 至少满足：

```text
anchor object exists

release capability exists when release is required

referenced byte range is valid

size/data relationship is valid

anchor can keep backing storage alive

release does not depend on already-destroyed Runtime state
```

同时还必须满足本文第 3.5 节规定的 lifetime 和 release contract。

---

### Payload Materialization

Payload materialization 指：

> **将 Acquisition-facing Payload input 建立为 Runtime 可以安全持有的 PayloadRef 的过程。**

不同 input mode 分别为：

```text
Copy
    → allocate + copy

Adopt
    → validate + acquire ownership

Retain
    → validate + acquire independent lifetime anchor
```

---

### Payload Access Lease

Payload Access Lease 指：

> Runtime 向 Storage 提供 read-only Payload access 的有效期间。

从 Storage 获得访问权限开始，到：

```text
PayloadConsumed
```

结束。

---

## 1.5 核心原则

### Immutable Logical Bytes

Payload logical bytes 一旦建立为 Runtime Payload：

> 在其完整 lifetime 内保持 immutable。

---

### Ownership 与 Admission 分离

必须严格区分：

```text
Payload ownership
```

和：

```text
Record admission result
```

例如：

```text
Adopt ownership transferred
        ↓
submission later rejected
```

Runtime 仍然负责释放 Payload。

不能因为 Record 未 accepted 而把 ownership 隐式退回 Plugin。

---

### Lifetime 与 Persistence 分离

Payload 是否可以释放取决于：

> 是否仍可能有 consumer 访问 Payload bytes。

而不取决于：

```text
PersistenceSucceeded
PersistenceFailed
CrashDurability
```

---

### Bounded Resource

所有 Runtime 持有的 Payload，包括 external zero-copy Payload：

> 都必须进入有界 resource accounting。

---

### Correctness before Zero-copy

如果 native source 无法提供安全稳定的 lifetime：

> 必须 Copy。

不得为了避免复制而引入模糊 ownership。

---

# 2. Payload 数据模型与 Runtime 表示

## 2.1 Canonical Payload

Canonical Payload 是有限长度 byte string：

```text
B = b[0], b[1], ... b[N-1]
```

其中：

```text
N >= 0
```

Payload logical identity 由：

```text
length
+
ordered logical bytes
```

确定。

以下内容不是 Canonical semantics：

```text
memory address
allocation source
buffer capacity
ownership kind
reference count
physical layout
```

---

## 2.2 Payload Equality

两个 Payload 逻辑相等，当且仅当：

```text
size(A) == size(B)
```

且：

```text
A[i] == B[i]
```

对全部 logical byte 成立。

---

## 2.3 Immutability

Payload publication 后禁止：

```text
modify byte
resize
replace backing bytes
change logical size
```

直到所有 Runtime lifetime obligations 完成。

Runtime 和 Storage Plugin 均不得修改 Runtime Payload。

---

## 2.4 No Silent Transformation

Runtime 不得对 Payload 执行：

```text
deserialize
reserialize
normalize
compress
decompress
prepend metadata
append metadata
reinterpret content
```

并继续把结果当成原 Payload。

如果 native physical representation 是 fragmented：

```text
segment A
segment B
segment C
```

而 logical Payload 定义为：

```text
A || B || C
```

则 flatten 为相同 contiguous logical bytes：

> 属于 physical materialization，不属于 semantic transformation。

---

## 2.5 V1 Physical Representation

V1 Runtime Payload baseline 为：

> **one contiguous immutable byte range**

逻辑表示：

```text
[data, data + size)
```

V1 不要求 RuntimeRecord 支持 scatter/gather。

未来如果 profiling 证明 scatter/gather 有必要，可以扩展 physical representation，但不得改变：

```text
logical bytes
immutability
ownership
lifetime
PayloadConsumed semantics
```

---

## 2.6 Empty Payload

V1 允许：

```text
size == 0
```

此时：

```text
data == nullptr
```

合法。

如果：

```text
size > 0
```

则必须：

```text
data != nullptr
```

---

## 2.7 PayloadRef

`PayloadRef` 是：

> Runtime 内部用于引用 immutable logical Payload，并保证其 lifetime 的 ownership-bearing runtime reference。

逻辑结构至少包含：

```text
PayloadRef
├── data
├── size
└── lifetime anchor
```

其中 lifetime anchor 必须保证：

```text
[data, data + size)
```

在 PayloadRef 有效期间：

```text
alive
stable
immutable
```

---

### 2.7.1 PayloadRef Move Semantics

V1 PayloadRef 的语义基线为：

> **move-only ownership handle**

move 后：

```text
source PayloadRef
    → empty / non-owning moved-from state

destination PayloadRef
    → owns original lifetime responsibility
```

moved-from PayloadRef：

- 不得继续提供有效 Payload access；
- 不得再次 release 原 anchor；
- 必须可以安全销毁。

---

### 2.7.2 PayloadRef Copy

V1 不要求 PayloadRef 隐式 copyable。

如果实现需要复制 PayloadRef：

> 必须显式建立新的 lifetime anchor reference。

例如：

```text
intrusive retain
reference-count increment
shared owner token duplication
```

不能：

```text
copy raw pointer
+
copy size
```

而不增加 lifetime guarantee。

任何 PayloadRef duplication 都必须保持：

> exactly-once physical release。

---

## 2.8 PayloadRef 不是 Raw View

以下形式不足以作为 V1 PayloadRef：

```text
const uint8_t*
+
size
```

因为它本身不提供 lifetime guarantee。

PayloadRef 必须绑定：

```text
Runtime-owned allocation

or

adopted external owner

or

retained external owner
```

---

## 2.9 Stable Address

一旦 Payload bytes 已经提供给可能异步使用的 consumer：

> backing address 必须保持稳定。

不得因为：

```text
reallocation
buffer compaction
RuntimeRecord move
```

使已经暴露的 pointer 失效。

---

# 3. Payload Ingress 与 Ownership

## 3.1 V1 Ingress Modes

V1 Acquisition Plugin 向 Runtime 提交 Payload 时支持三种 ownership mode：

```text
Copy
Adopt
Retain
```

Storage Plugin 不需要知道原始 Payload 属于哪种 mode。

---

## 3.2 Copy

`Copy` 表示：

> Plugin 提供临时只读 byte view，Runtime 创建自己的 backing storage 并复制 exact logical bytes。

输入逻辑为：

```text
BorrowedInput
{
    data
    size
}
```

BorrowedInput 只保证在：

> `synchronous input lifetime`

内有效。

---

### 3.2.1 Copy Materialization Deadline

Runtime 必须在：

> **BorrowedInput synchronous input lifetime 结束之前完成所有对 borrowed bytes 的读取。**

因此：

```text
submit()
    begins
        ↓
Runtime validates input
        ↓
Runtime copies required logical bytes
        ↓
submit() returns
```

之后：

> Runtime 不得再次读取原 BorrowedInput。

禁止：

```text
submit() returns
        ↓
background task later copies
BorrowedInput.data
```

这种实现。

---

### 3.2.2 Delayed Materialization

如果 Runtime / Admission implementation 希望把真正的 Payload materialization 延迟到：

```text
gate pass
```

之后，且该 materialization 可能发生在 synchronous input lifetime 之外：

> Plugin 不能使用普通 Copy/BorrowedInput。

此时必须：

```text
Adopt

or

Retain
```

或者 Plugin 提供一个能够覆盖 materialization 完成之前完整 lifetime 的独立 anchor。

这种带 anchor 的输入在语义上应按：

```text
Retain
```

处理，而不是 naked Copy borrow。

---

### 3.2.3 Copy Ownership

Copy 完成后：

```text
Plugin retains native ownership

Runtime owns independent copied Payload
```

Plugin 在 submit contract 允许后可以：

```text
release
reuse
mutate
```

原 native buffer。

Runtime copy 不受影响。

---

### 3.2.4 Copy Accuracy

复制必须满足：

```text
runtime.size == input.size
```

并且：

```text
runtime[i] == input[i]
```

对全部 logical byte 成立。

---

### 3.2.5 Naked Borrow

V1 不支持：

> raw borrowed Payload 跨越 synchronous input lifetime。

accepted RuntimeRecord 不得直接引用 callback-local 或其他短生命周期 raw buffer。

如果 Plugin 无法提供 Adopt 或 Retain：

> 必须在 synchronous input lifetime 内完成 Copy。

---

## 3.3 Adopt

`Adopt` 表示：

> Plugin 将 Payload backing storage 的 release responsibility 单向转移给 Runtime。

逻辑输入：

```text
AdoptedPayload
{
    data
    size
    ownership anchor
}
```

---

### 3.3.1 Adopt Validation Before Transfer

Runtime 在取得 Adopt ownership responsibility 之前：

> **必须先完成 structural validation。**

至少验证：

```text
size/data relationship valid

anchor present where required

release capability valid

backing range representable

anchor structurally satisfies
the external lifetime contract
```

只有 validation 成功后：

> Runtime 才取得 ownership responsibility。

---

### 3.3.2 Ownership Transfer Point

Adopt transfer 的精确语义为：

```text
Plugin calls Runtime with Adopt input
        ↓
Runtime validates input and anchor
        ↓
validation succeeds
        ↓
Runtime atomically accepts
ownership responsibility
```

这一时刻称为：

> **Adopt Transfer Point**

Adopt Transfer Point：

- 位于 admission result 之前；
- 不等于 gate pass；
- 不等于 Record accepted；
- 必须发生在 Runtime 能安全管理该 anchor 之后。

---

### 3.3.3 Invalid Adopt Input

如果 Adopt validation 失败：

```text
Runtime does not acquire ownership
```

submission：

> 按 pre-admission rejection 处理。

此时：

```text
Plugin retains original ownership
```

Plugin 提供 structurally invalid anchor：

> 属于 Acquisition Plugin Contract violation。

Runtime SHOULD 产生 operational diagnostic。

---

### 3.3.4 No Ownership Rollback

一旦越过 Adopt Transfer Point：

> ownership 不退回 Plugin。

后续无论：

```text
pre-admission rejection
admission rejection
accepted Record
Runtime failure
```

Runtime 都承担 release responsibility。

禁止：

```text
Runtime adopted successfully
        ↓
later operation failed
        ↓
Plugin expected to release again
```

---

### 3.3.5 Plugin Access

真正的 unique adoption 完成后：

> Plugin 不得继续访问该 backing storage。

如果 Plugin 需要继续访问：

> 应使用 Retain/shared ownership，而不是 Adopt。

---

## 3.4 Retain

`Retain` 表示：

> Payload backing storage 由 external owner 管理，但 Runtime 获得独立 lifetime anchor。

逻辑输入：

```text
RetainedPayload
{
    data
    size
    lifetime anchor
}
```

---

### 3.4.1 Retain Validation Before Acquisition

Runtime 在接受 Retain lifetime responsibility 之前：

> 必须验证 anchor 结构有效。

至少验证：

```text
data/size relationship valid

anchor exists

retain ownership relation valid

release capability available

range representable
```

只有 validation 成功后，Runtime 才把该 anchor 纳入：

```text
PayloadRef
resource accounting
release responsibility
```

---

### 3.4.2 Invalid Retain Input

validation 失败时：

```text
Runtime does not acquire lifetime responsibility
```

submission 按：

> pre-admission rejection

处理。

原 external ownership：

> 保持在 Plugin / native owner。

invalid anchor 属于 Acquisition Contract violation，并 SHOULD 产生 operational diagnostic。

---

### 3.4.3 Retain ≠ Borrow

Borrow：

```text
pointer valid only while caller remains active
```

Retain：

```text
Runtime independently holds
lifetime guarantee
```

因此 Retain 可以安全支持 asynchronous zero-copy。

---

### 3.4.4 Retain Fallback

如果 native API 无法提供：

```text
independent
safe
bounded
releaseable
```

lifetime anchor：

> 必须 Copy。

---

## 3.5 External Lifetime Anchor

Adopt / Retain 的 external anchor 必须满足：

```text
bytes remain alive
bytes remain stable
bytes remain immutable
until release
```

release operation 必须：

```text
exactly-once safe
not depend on released Runtime state
bounded
non-blocking or short
```

---

### 3.5.1 Invalid Release Capability

如果 Runtime 在 ownership acquisition 前已经能够判断：

```text
release callback missing
release target invalid
required owner token absent
```

则：

> validation 必须失败，Runtime 不得取得 ownership。

---

### 3.5.2 Release Contract Violation

如果 anchor 已经成功被 Runtime 接受，但实际 release 时出现：

```text
blocking indefinitely
exception
fatal callback failure
reentrant corruption
invalid owner state
```

则属于：

> **Acquisition / external ownership Contract violation**

Runtime：

- 不得通过重复调用 release 试图修复；
- 不得产生 double release；
- 不得牺牲 memory safety；
- 必须产生 operational diagnostic；
- 如果无法安全确认资源已经释放，可以进入 quarantine / terminal failure cleanup。

C++ V1 实现中的 release path：

> SHOULD NOT allow exceptions escape across Runtime ownership boundary。

如果 external release implementation 违反这一要求：

> Runtime 必须将其视为 Contract violation，而不是正常 error-return path。

---

## 3.6 Thread-affine Native Buffer

如果 native buffer：

```text
must be released
only on original callback thread
```

Plugin 不应直接把该复杂语义暴露给 Runtime。

Plugin 应：

- 封装出安全可释放的 owner；或
- 使用 Copy。

V1 不要求 Runtime 建立通用 thread-affine deleter routing subsystem。

---

## 3.7 Zero-copy

本文中的 zero-copy 表示：

> native Payload backing storage 被 Runtime 直接作为 Runtime Payload 使用，而不复制 logical bytes。

对应：

```text
Adopt
Retain
```

只有同时满足以下条件时才允许：

```text
contiguous bytes
stable address
known size
immutability
valid lifetime anchor
safe release
bounded accounting
```

zero-copy：

> 是优化能力，不是正确性要求。

---

# 4. Payload 与 Admission / RuntimeRecord

## 4.1 Ownership Transfer 与 Admission

Payload ownership transfer 与：

```text
Admission Gate
AcceptanceSequence
Accepted / Rejected
```

相互独立。

例如：

```text
Runtime passes Adopt Transfer Point
        ↓
Gate later found closed
        ↓
pre-admission rejection
```

结果仍然是：

```text
no AcceptanceSequence
no RuntimeRecord

Runtime releases
adopted Payload
```

---

## 4.2 Pre-admission Validation

以下 Payload validation 可以在进入 admission domain 之前执行：

```text
size representable

size <= max_single_payload_size

data != nullptr
when size > 0

Adopt / Retain anchor
structurally valid
```

这些检查失败：

> 属于 pre-admission rejection。

不消耗 AcceptanceSequence。

对于 invalid Adopt / Retain input：

> Runtime 必须确保 validation failure 发生在 ownership/lifetime responsibility acquisition 之前。

---

## 4.3 Payload Materialization

Payload materialization 表示：

> 建立一个能够安全进入 RuntimeRecord 的 PayloadRef。

不同 mode：

```text
Copy
    → allocate + exact copy

Adopt
    → validate
    → acquire ownership

Retain
    → validate
    → acquire independent anchor
```

---

### 4.3.1 Copy Timing

对于普通 BorrowedInput：

> Copy materialization 必须不晚于 synchronous input lifetime 结束。

也就是说：

```text
all reads from BorrowedInput
must finish
before submit returns
```

Admission implementation 不得设计为：

```text
store naked BorrowedInput pointer
        ↓
submit returns
        ↓
later materialize
```

---

## 4.4 Stable Payload Before Accepted Enqueue

accepted RuntimeRecord 进入 Runtime bounded dispatch path 前：

> 必须已经存在完整稳定的 PayloadRef。

禁止：

```text
enqueue accepted RuntimeRecord
        ↓
later materialize Payload
```

---

## 4.5 Materialization Failure

如果 failure 在 gate pass 前已经确定：

```text
invalid input
oversize
payload capacity unavailable
invalid ownership anchor
```

可以：

```text
pre-admission rejection
```

不消耗 AcceptanceSequence。

如果 submission 已进入 admission domain 后才发生：

```text
Runtime-owned Copy allocation failure

Retain control-object allocation failure

other materialization failure
after valid ownership acquisition
```

则：

```text
Admission → Rejected
```

AcceptanceSequence 保持 consumed。

---

### 4.5.1 Copy Failure Lifetime Rule

对于 Copy：

> allocation 和 byte copy 必须全部在 BorrowedInput synchronous input lifetime 内完成。

因此即使：

```text
gate pass
```

发生在 copy 完成之前，Runtime 仍必须保证：

```text
copy completion
<
BorrowedInput lifetime end
```

否则 Admission design 不合法。

如果无法保证这一点：

> 必须改变 operation ordering，或者要求 Plugin 使用 Adopt / Retain。

---

## 4.6 Rejected Payload

### Pre-admission Rejection

Copy：

- 如果尚未建立 Runtime copy，没有 Runtime Payload 需要释放；
- 如果临时 copy 已建立，则释放 Runtime copy。

Adopt / Retain：

- 如果 validation 失败且 ownership 尚未取得，Runtime 不 release external anchor；
- 如果 ownership/lifetime responsibility 已经成功取得，Runtime 必须 release。

---

### Admission Rejection

如果 attempt 已进入 admission domain，但最终 Rejected：

```text
AcceptanceSequence consumed
no accepted RuntimeRecord
```

Runtime 在 admission logic 不再需要 Payload 后：

> release PayloadRef。

Rejected Payload：

> 不进入 Storage Record path。

---

## 4.7 Accepted RuntimeRecord

accepted RuntimeRecord 携带：

```text
PayloadRef
```

逻辑结构保持：

```text
RuntimeRecord
{
    BindingHandle
    AcceptanceSequence

    optional SourceHandle
    optional SourceSequence

    TimestampStateSlots

    PayloadRef
}
```

RuntimeRecord 可以 move。

move：

> 转移 PayloadRef lifetime responsibility，不改变 Payload bytes 或 backing address。

---

### 4.7.1 RuntimeRecord Move

RuntimeRecord move 后：

```text
source RuntimeRecord
    no longer owns PayloadRef

destination RuntimeRecord
    owns PayloadRef
```

moved-from RuntimeRecord：

> 不得在后续 pipeline 中再次参与 Payload release。

---

### 4.7.2 RuntimeRecord Copy

V1 不要求 RuntimeRecord copyable。

如果实现支持 RuntimeRecord copy：

> PayloadRef 必须显式 duplicate lifetime anchor。

不得造成：

```text
double release
unanchored alias
```

---

## 4.8 Exactly-once Release

所有 ingress mode 都必须满足：

```text
Runtime acquires lifetime responsibility
        ↓
exactly one final physical release
```

禁止：

```text
double release
lost release
implicit rollback
ambiguous owner
```

---

# 5. Runtime → Storage Payload Contract

## 5.1 Ownership Model

V1 Storage handoff 采用：

> **Runtime retains Payload ownership; Storage receives read-only access.**

即：

```text
Runtime
    owns PayloadRef / anchor

Storage
    receives immutable access
```

Record handoff 不自动把 backing storage ownership 转交给 Storage。

---

## 5.2 PayloadView

Storage-facing Payload access 逻辑上为：

```text
PayloadView
{
    const data
    size
}
```

`PayloadView`：

> 是 non-owning read-only view。

其 lifetime 由 Runtime 提供的 access lease 保证。

---

## 5.3 Storage Payload Access Lease

Storage handoff 建立：

> **Payload Access Lease**

lease 期间：

```text
Runtime keeps Payload alive
Storage may read Payload
```

lease 结束于：

```text
PayloadConsumed
```

---

## 5.4 Asynchronous Access

Storage Plugin 可以：

```text
retain PayloadView internally
read from worker thread
copy asynchronously
```

前提是：

> 尚未报告 `PayloadConsumed`。

Runtime 在此期间必须保持 PayloadRef 有效。

---

## 5.5 PayloadConsumed

`PayloadConsumed` 表示：

> **Storage Plugin 已永久结束对 Runtime-provided Payload bytes 的所有访问。**

报告后：

```text
no Storage thread
no Storage worker
no deferred task
```

允许再次读取该 PayloadView。

---

## 5.6 PayloadConsumed 与 Persistence

必须严格区分：

```text
PayloadConsumed
```

```text
PersistenceSucceeded
```

以及：

```text
CrashDurability
```

典型合法路径：

```text
Storage copies Payload
        ↓
PayloadConsumed
        ↓
Runtime releases original Payload
        ↓
compression / transaction / write
        ↓
PersistenceSucceeded
```

因此：

```text
PayloadConsumed
!=
PersistenceSucceeded
```

---

### 5.6.1 Persistence Success 不能替代 PayloadConsumed

如果：

```text
PersistenceSucceeded
```

但 Storage worker 仍可能访问 Runtime Payload：

> Runtime 不能释放。

---

### 5.6.2 Persistence Failure 不能替代 PayloadConsumed

如果：

```text
PersistenceFailed
```

也不能自动认为：

```text
PayloadConsumed
```

因为 Storage worker 可能仍持有 PayloadView。

---

## 5.7 Synchronous Storage

Storage Contract 可以明确声明：

```text
successful Record handoff return
    ⇒
PayloadConsumed
```

此时：

> **handoff return 本身构成该 lease 的一次 terminal PayloadConsumed completion。**

Runtime 据此：

```text
end Payload Access Lease
release Payload lifetime when no other consumer exists
```

Storage：

> 不再额外发送 PayloadConsumed callback。

否则将构成 duplicate completion。

如果 Storage Contract 没有明确声明：

```text
handoff return == PayloadConsumed
```

Runtime 必须继续等待显式 completion。

---

## 5.8 Completion Exactly Once

每个 Payload access lease：

> 恰好以一种 Contract-defined 方式结束。

可以是：

```text
explicit PayloadConsumed callback
```

或：

```text
synchronous handoff return
declared as PayloadConsumed
```

但不能两者同时重复完成同一个 lease。

duplicate completion：

> 属于 Storage Contract violation。

Runtime 不得执行第二次 release。

---

## 5.9 Storage Failure

fatal Storage failure：

> 不自动终止 Payload lifetime。

Runtime 必须先证明：

```text
no Storage execution context
can access Payload
```

之后才能 release。

如果无法建立该 quiescence guarantee：

> 按第 6.14 节进入 quarantine。

---

# 6. 资源、并发与 Failure Lifetime

## 6.1 Bounded Payload Resource

V1 Runtime 至少必须配置：

```text
max_single_payload_size
max_payload_bytes_in_flight
max_payload_objects_in_flight
```

具体字段名不冻结。

---

## 6.2 Payload Bytes In Flight

`payload_bytes_in_flight` 表示：

> Runtime 当前仍承担 lifetime responsibility 的 Payload logical bytes 总量。

必须覆盖：

```text
Copy
Adopt
Retain
```

---

## 6.3 External Buffer 也必须计费

Retain / Adopt 使用 external physical memory 时：

> 只要 Runtime lifetime anchor 阻止该 memory 被释放，就必须进入 Payload budget。

否则 zero-copy 会成为：

> 绕过 Runtime bounded-resource policy 的路径。

---

## 6.4 Logical-byte Accounting

V1 默认按：

```text
PayloadRef.size
```

计入 retention budget。

即使多个 PayloadRef 物理共享同一 backing allocation：

> 也允许分别按 logical size 计费。

例如：

```text
Payload A = 100 bytes
Payload B = 100 bytes
```

即使二者共享一个较大的 external allocation：

```text
budget charge = 200 bytes
```

是合法的 V1 accounting。

该模型：

> **可能高估真实 physical memory usage，但能够保证 external zero-copy 无法绕过 bounded-resource policy。**

V1 不要求：

```text
physical backing allocation dedup accounting
```

---

## 6.5 Budget Release

Payload budget 在 Runtime 不再承担对应 lifetime responsibility 后归还。

正常 accepted Record：

```text
PayloadConsumed
        ↓
release Runtime Payload anchor
        ↓
return Payload budget
```

即使：

```text
Persistence = Pending
```

也可以归还 Runtime Payload budget，只要 Storage 已结束对原 Payload 的访问。

---

## 6.6 Runtime-owned Allocation

Copy 可以使用：

```text
fixed pool
size-class allocator
bounded heap wrapper
slab
arena/free-list
```

等方式。

本文不冻结 allocator。

但禁止：

```text
bounded pool exhausted
        ↓
silently fallback to unbounded heap
```

如果系统配置要求 bounded Runtime。

---

## 6.7 Checked Arithmetic

所有：

```text
Payload size
budget accumulation
offset calculation
capacity calculation
```

必须使用 checked arithmetic。

overflow：

> 必须失败。

不得 wrap。

---

## 6.8 Concurrent Read

Payload logical bytes immutable，因此可以被多个 consumer 并发读取。

如果 lifetime anchor 可能跨线程 retain / release：

> ownership metadata 必须 thread-safe。

本文不冻结：

```text
atomic refcount
mutex
intrusive ref
serialized release
```

等实现。

---

## 6.9 Completion Concurrency

Storage `PayloadConsumed` 可以由 Storage internal worker 发出。

Runtime-side lifetime state mutation：

> 必须经过 Runtime Storage Completion Domain。

因此并发 completion 不能导致：

```text
double release
lost release
race on Payload lifetime
```

---

## 6.10 Normal Stop

Stop 时：

```text
accepted
+
not yet PayloadConsumed
```

的 Payload 必须继续保持有效。

Admission Gate closed：

> 不构成释放 accepted Payload 的理由。

---

## 6.11 Accepted but Not Handed Off

如果 Record 已 accepted 但尚未 Storage handoff：

```text
Runtime owns PayloadRef
```

正常 Stop：

> 继续 dispatch。

如果 failure 后确定该 Record 永远不会再被 consumer 访问：

> 可以 release Payload。

Record 的 Canonical history仍然保持：

```text
Accepted
```

---

## 6.12 Handed Off but Not Consumed

如果：

```text
Record handed off
PayloadConsumed not received
```

Runtime 必须继续保留 Payload。

即使 Runtime 已进入：

```text
Failed
```

也不能直接释放。

下一步必须按照：

```text
§6.13 Failure Quiescence
```

建立 no-future-access guarantee。

如果无法建立：

> 进入 §6.14 Quarantine。

---

## 6.13 Failure Quiescence

failure cleanup 必须首先建立：

```text
no consumer can access Payload
```

的 quiescence guarantee。

之后才能释放 Payload。

Quiescence 可以来自：

```text
explicit Storage shutdown completion

worker join

backend cancellation completion

contract-defined terminal failure barrier
```

具体机制由 Storage Contract 定义。

---

## 6.14 Quarantine

如果 failure 后无法证明：

```text
no future access
```

Runtime 可以：

```text
quarantine Payload anchor
```

直到：

```text
consumer quiescence
process teardown
or equivalent safe lifetime boundary
```

memory safety 优先于 cleanup completeness。

quarantined Payload：

> 仍必须保留在独立 observability/accounting 中，不能被伪装成正常释放。

---

## 6.15 Release Function Requirements

External anchor release 必须：

```text
exactly-once safe

bounded

non-blocking or short

not depend on destroyed Runtime state

not synchronously re-enter
the same ownership transition
```

---

### 6.15.1 Release Contract Violation

如果 external release：

```text
blocks beyond contract
throws
re-enters illegally
fails to release
accesses invalid Runtime state
```

则属于：

> Acquisition / external ownership Contract violation。

Runtime：

- 不得重复调用 release；
- 不得假设 release 已成功；
- 不得产生 double free；
- 不得提前销毁仍可能被访问的 backing storage；
- SHOULD 产生 operational diagnostic；
- 必要时进入 Failed cleanup / quarantine。

对于 C++ implementation：

> release function SHOULD be noexcept。

如果异常仍越过 ownership boundary：

> Runtime 必须将其转换为 terminal Contract violation handling，不能让 exception unwinding破坏 Runtime ownership bookkeeping。

---

# 7. Interface 与职责边界

## 7.1 Acquisition-facing Contract

Acquisition Plugin 至少需要表达：

```text
Copy
Adopt
Retain
```

三种 Payload input semantics。

---

### Copy

提供：

```text
const byte view
size
```

并保证：

> BorrowedInput 在 synchronous input lifetime 内有效。

Runtime 必须在该 lifetime 内完成所有 byte copy。

---

### Adopt

提供：

```text
const byte view
size
ownership anchor
```

Runtime：

1. 先验证 input/anchor；
2. validation 成功后取得 ownership；
3. ownership 不 rollback。

---

### Retain

提供：

```text
const byte view
size
independent lifetime anchor
```

Runtime：

1. 先验证 anchor；
2. validation 成功后取得 lifetime responsibility。

---

## 7.2 Acquisition Plugin Responsibilities

Acquisition Plugin 负责：

```text
native buffer interpretation

decide Copy / Adopt / Retain

provide structurally valid anchor

guarantee Payload immutability

flatten fragmented native payload
when V1 Runtime requires contiguous bytes

handle first sample before Source registration

handle Binding-switch pause
```

Runtime generic boundary不得暴露：

```text
rclcpp::SerializedMessage
DDS SerializedPayload
can_frame
vendor buffer
```

作为 Payload representation。

---

## 7.3 Runtime Internal Responsibilities

Runtime Payload subsystem 至少负责：

```text
validate Payload input

validate ownership anchor
before acquisition

reserve Payload resource

materialize Copy
within BorrowedInput lifetime

accept Adopt / Retain

create PayloadRef

maintain lifetime

provide read-only Storage view

process PayloadConsumed

release Payload exactly once

account bytes / objects

handle failure cleanup
```

具体 C++ API 不在本文冻结。

---

## 7.4 Storage-facing Contract

Storage Plugin 只需要看到：

```text
immutable PayloadView
```

Storage 不需要知道 Payload 原本来自：

```text
Copy
Adopt
Retain
```

中的哪一种。

Storage Contract 必须定义：

```text
synchronous / asynchronous access

whether handoff return itself
constitutes PayloadConsumed

explicit PayloadConsumed completion
when asynchronous

multi-worker access

failure quiescence

duplicate completion behavior
```

---

## 7.5 与 `admission.md` 的边界

Payload 已冻结：

```text
Payload validation
may happen before gate pass

Payload ownership
is independent of admission

Copy must read BorrowedInput
only during synchronous input lifetime

PayloadRef must be stable
before accepted enqueue

Payload resource is bounded
```

`admission.md` 必须定义：

```text
resource reservation point

sequence allocation order

ingress timestamp point

Copy allocation timing

accept/reject linearization

queue + Payload budget reservation

cleanup on rejected attempt
```

并必须满足：

> **Copy allocation / materialization timing 不得晚于 BorrowedInput synchronous input lifetime 结束。**

如果 Admission design 无法满足这一点：

> 对应 submission 必须使用 Adopt / Retain，而不能继续使用 naked Copy input。

---

## 7.6 与 `storage_plugin.md` 的边界

Payload 已冻结：

```text
Runtime retains Payload lifetime responsibility

Storage receives read-only access lease

PayloadConsumed ends Storage access

synchronous handoff return
may itself be PayloadConsumed
only when Contract explicitly says so

PayloadConsumed independent
from persistence

failure requires quiescence
before release
```

`storage_plugin.md` 进一步定义：

```text
PayloadView API

handoff completion

PayloadConsumed representation

synchronous-consumption declaration

multi-worker completion

failure / timeout semantics

quiescence contract
```

---

# 8. Invariants 与 Conformance

## 8.1 Payload Invariants

V1 Runtime 必须保持以下 invariant。

### Logical Content

```text
Payload logical bytes
do not change
during lifetime
```

### Physical Representation

```text
PayloadRef
resolves to one contiguous range
```

### Pointer Validity

```text
size > 0
⇒
data != nullptr
```

### Lifetime

```text
every live PayloadRef
has a valid lifetime anchor
```

### PayloadRef Move

```text
move transfers lifetime responsibility

moved-from PayloadRef
does not release original anchor
```

### Adopt

```text
anchor validated
before ownership acquisition

ownership transfer is one-way

no rollback
```

### Retain

```text
anchor validated
before lifetime acquisition

requires independent
Runtime lifetime guarantee
```

### Borrow / Copy

```text
raw borrow
cannot survive
synchronous input lifetime

Copy reads borrowed bytes
only during that lifetime
```

### Release

```text
release exactly once
```

### Storage Access

```text
Storage may access
before PayloadConsumed

Storage must not access
after PayloadConsumed
```

### Synchronous Consumption

```text
handoff return may equal
PayloadConsumed
only by explicit Storage Contract
```

### Completion

```text
PayloadConsumed
!= PersistenceSucceeded

PayloadConsumed
!= CrashDurability
```

### Resource

```text
Copy / Adopt / Retain
all participate
in bounded accounting
```

### Failure

```text
no reclamation
before consumer quiescence
```

---

## 8.2 Required Conformance Scenarios

V1 implementation 至少必须覆盖以下测试。

### Input Validation

验证：

```text
size == 0
data == nullptr
```

合法。

验证：

```text
size > 0
data == nullptr
```

在 admission 前拒绝。

---

### Copy Exactness

提交已知 byte pattern。

Submit 后立即释放或修改原 buffer。

验证：

> Runtime / Storage 仍看到 exact original bytes。

---

### Copy Materialization Lifetime

使用 callback-local BorrowedInput。

Runtime 在 submit 内完成 Copy。

submit 返回后立即使 source buffer invalid。

验证：

```text
Runtime never reads
BorrowedInput after submit return
```

并且 accepted RuntimeRecord 只引用 Runtime-owned copy。

---

### Copy Allocation Failure

在 BorrowedInput 仍有效期间强制 Runtime Copy allocation 失败。

验证：

- Runtime 不在 input lifetime 结束后继续读取 input；
- submission 按合法 pre-admission 或 admitted rejection path 收敛；
- 不产生 dangling PayloadRef；
- 不产生 memory leak。

---

### Naked Borrow

使用 callback-local raw buffer。

验证：

> accepted RuntimeRecord 不引用 callback-local raw storage。

---

### Adopt Valid Transfer

提交合法 AdoptedPayload。

验证：

```text
validation succeeds
        ↓
Runtime acquires ownership
        ↓
Plugin no longer owns release
```

最终 exactly once release。

---

### Invalid Adopt Anchor

提交：

```text
missing / structurally invalid
ownership anchor
```

验证：

```text
Runtime does not acquire ownership

submission pre-admission rejected

Plugin remains responsible
for original storage
```

并产生 Contract violation diagnostic。

---

### Adopt Pre-admission Rejection

使用合法 Adopt input，Runtime 已完成 ownership transfer 后使 Gate closed。

验证：

```text
no AcceptanceSequence
no RuntimeRecord

Runtime releases anchor
exactly once
```

---

### Adopt Admission Rejection

合法 Adopt 已进入 admission domain，最终 Rejected。

验证：

```text
AcceptanceSequence consumed
Runtime releases anchor exactly once
```

---

### Retain Valid Anchor

Plugin 与 Runtime 各持有独立 owner。

Plugin 先释放自己的 owner。

验证：

> Runtime / Storage Payload 仍然有效。

---

### Invalid Retain Anchor

提交：

```text
empty / invalid
lifetime anchor
```

验证：

```text
Runtime does not acquire
lifetime responsibility

submission rejected
before admission
```

并产生 Contract violation diagnostic。

---

### Retain Immutability Violation

构造错误 Plugin：

```text
Retain Payload
then mutate backing bytes
```

测试 instrumentation 应能识别：

> Plugin 违反 immutable Payload Contract。

production hot path 不要求每 Record checksum。

---

### PayloadRef Move

构造一个 PayloadRef，依次 move 到：

```text
temporary
RuntimeRecord
dispatch node
```

验证：

```text
only current owner
holds release responsibility

moved-from objects
cannot release anchor

final release exactly once
```

---

### Oversize Payload

```text
size > max_single_payload_size
```

必须在能够预先判断时：

```text
pre-admission rejection
```

且不分配 AcceptanceSequence。

---

### Payload Budget Exhaustion

耗尽：

```text
max_payload_bytes_in_flight
```

验证：

```text
no unbounded allocation
no unbounded external retention
```

最终行为符合 `admission.md`。

---

### External Payload Budget

大量 Retain external buffers。

验证：

> external zero-copy 同样进入 Runtime budget。

该测试允许 logical-byte accounting 高估 physical memory。

---

### Async Storage

Storage worker 在 handoff 返回后继续读取 Payload。

验证：

> PayloadConsumed 前 Runtime 不释放 Payload。

---

### Synchronous Storage Consumption

Storage Contract 声明：

```text
successful handoff return
==
PayloadConsumed
```

验证：

```text
handoff returns
        ↓
lease terminal
        ↓
Runtime may release Payload
```

并且 Storage：

> 不再发送第二个 PayloadConsumed callback。

---

### PayloadConsumed Before Persistence

Storage 先报告：

```text
PayloadConsumed
```

随后 persistence。

验证：

> Runtime 可以释放原 Payload，Persistence 继续独立完成。

---

### Persistence Before Consumption

先：

```text
PersistenceSucceeded
```

再：

```text
PayloadConsumed
```

验证：

> Runtime 不提前释放 Payload。

---

### Persistence Failure

```text
PersistenceFailed
```

但 Storage worker 仍访问 Payload。

验证 Runtime 保持 lifetime。

---

### Duplicate Completion

对同一 lease：

```text
synchronous handoff completion
```

之后又发送：

```text
PayloadConsumed callback
```

或者重复 callback。

验证：

```text
no double release
```

并报告 Storage Contract violation。

---

### Access After Consumption

Storage 在：

```text
PayloadConsumed
```

之后继续读取旧 PayloadView。

必须识别为：

> Storage Contract violation。

---

### Concurrent Readers

多个线程并发读取相同 Payload。

验证：

```text
same bytes
no mutation
no lifetime corruption
```

---

### RuntimeRecord Move

RuntimeRecord 在 queue / dispatch path 中多次 move。

验证：

> Payload address、ownership 和 lifetime 保持正确。

---

### Fatal Storage Failure

Record 已 handoff 但未 PayloadConsumed。

触发 fatal Storage failure。

验证：

> quiescence 之前 Payload 不释放。

---

### Failure Quarantine

构造无法停止的 consumer。

Runtime Failed 后：

> Payload 可以保留到 process teardown，但不得发生 use-after-free。

---

### Blocking Release Contract Violation

构造 external anchor，其 release 超过 contract-defined bounded execution requirement。

验证：

- Runtime 不重复执行 release；
- 产生 Contract violation diagnostic；
- 不发生 double free；
- 必要时进入 Failed cleanup / quarantine。

---

### Throwing Release Contract Violation

构造错误 external release implementation 抛出异常。

验证：

- exception 不破坏 Runtime ownership bookkeeping；
- physical release 不被重复执行；
- violation 被报告；
- Runtime 按 terminal failure policy 收敛。

---

### Resource Release

对：

```text
Copy
Adopt
Retain
```

分别验证正常、拒绝和 failure path 后：

```text
payload objects in flight
payload bytes in flight
```

正确归零或进入明确 quarantine accounting。

---

## 8.3 V1 Payload Design Baseline

RoDAQ V1 Payload path 为：

```text
Native Payload
        ↓
Acquisition Plugin
        ↓
Copy / Adopt / Retain
        ↓
PayloadRef
        ↓
RuntimeRecord
        ↓
Storage Payload Access Lease
        ↓
PayloadConsumed
        ↓
Release
```

V1 设计基线为：

```text
immutable logical byte string

contiguous Runtime representation

move-only PayloadRef semantic baseline

Copy
Adopt
Retain

Copy materialization
within BorrowedInput lifetime

no naked borrow
past submit lifetime

validate Adopt / Retain anchor
before ownership acquisition

one-way Adopt transfer

independent Retain anchor

stable Payload address

Runtime-owned lifetime responsibility

read-only Storage access lease

explicit PayloadConsumed boundary

synchronous handoff may be
PayloadConsumed only by contract

PayloadConsumed independent
from persistence / durability

bounded Payload byte accounting

external zero-copy included in budget

conservative logical-byte accounting

exactly-once release

no silent Payload transformation

release Contract violation
is explicit failure

no premature release on failure

quiescence before reclamation

memory safety before
failure cleanup completeness
```

该模型提供：

> **明确的 ownership transfer boundary、严格的 borrowed-input lifetime、稳定的异步访问语义、有界资源模型以及无需依赖具体 Middleware 或 Storage Backend 的 Payload Contract。**