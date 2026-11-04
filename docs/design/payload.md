# RoDAQ Payload Design Specification

**Version:** v0.5
**Status:** Design Specification  
**Scope:** RoDAQ V1 Payload Representation, Ownership and Lifetime

**Normative Dependencies:**

- RoDAQ Canonical Data Model Design Specification v0.9
- RoDAQ V1 Canonical Profile v0.6
- RoDAQ System Architecture Design Specification v0.6
- RoDAQ Runtime Design Specification v0.6

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

> **从 Runtime 的 submit operation 被调用开始，到该 submit operation 返回为止的同步调用期间。**

对于没有独立 lifetime anchor 的 `BorrowedInput`：

> Runtime 不得在 submit operation 返回后继续读取 input bytes。

---

### Structurally Valid Anchor

`structurally valid anchor` 指 external ownership/lifetime anchor 至少满足：

```text
anchor object exists

release capability exists
when release is required

referenced byte range is valid

size/data relationship is valid

anchor can keep backing storage alive

release does not depend on
already-destroyed Runtime state
```

同时还必须满足本文第 3.5 节规定的 lifetime 和 release contract。

---

### Payload Materialization

Payload materialization 指：

> **将 Acquisition-facing Payload input 建立为 Runtime 可以安全持有的 PayloadRef 的过程。**

V1 三种 ingress mode 分别为：

```text
Copy
    → allocate + copy

Adopt
    → validate + acquire ownership

Retain
    → validate + acquire
      independent lifetime anchor
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

对于 Adopt / Retain：

> 即使 `size == 0`，external anchor 仍然必须按对应 ingress mode 的规则验证。

如果 anchor validation 成功并且 Runtime 取得 ownership / lifetime responsibility：

> anchor 仍必须在后续正常、拒绝或 failure path 中 exactly-once release。

Empty Payload 不意味着：

```text
no ownership resource exists
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

PayloadRef move 本身是否通过内部 atomic 操作实现：

> 属于 implementation detail。

但如果 PayloadRef 可能跨线程 move：

> 实现必须保证 move 不产生 double release、lost release 或同时出现两个 logical owner。

同一个 PayloadRef object 本身：

> 不因为 Payload bytes immutable 就自动获得无同步 concurrent move safety。

调用方必须遵守实现规定的 object-level synchronization contract。

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

三种模式具有独立而明确的语义。

V1：

> **不支持“Copy + external lifetime anchor”这种第四种混合 ingress mode。**

Storage Plugin 不需要知道原始 Payload 属于哪种 mode。

---

## 3.2 Copy

`Copy` 表示：

> Plugin 提供临时只读 byte view，Runtime 在 synchronous input lifetime 内创建自己的 backing storage，并复制 exact logical bytes。

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
submit() called
        ↓
Runtime validates input
        ↓
Runtime allocates copy storage
        ↓
Runtime copies logical bytes
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

如果 Runtime / Admission implementation 希望把 Payload 的稳定持有延迟到：

```text
gate pass
```

之后，并且该时刻可能已经超出 ordinary Copy 的 synchronous input lifetime：

> 普通 `Copy/BorrowedInput` 不适用。

Plugin 必须改用以下二者之一：

```text
Adopt
```

或：

```text
Retain
```

其语义分别为：

### Adopt

Runtime 取得 backing storage ownership，因此可以在后续 Runtime lifetime 内继续使用该 storage。

### Retain

Runtime 取得独立 lifetime anchor，因此可以在后续 Runtime lifetime 内直接引用该 original backing storage。

V1 明确不定义：

```text
Copy intent
+
temporary external anchor
+
later delayed copy
```

这种混合 ingress mode。

如果 Plugin 希望保留 backing storage ownership，同时允许 Runtime 在 submit 返回之后继续使用原 backing storage：

> 应使用 Retain。

Retain 的 Runtime Payload 就是该 retained backing storage 本身，不再保持“稍后必须 Copy”的额外语义。

如果 Plugin 希望完全交出 backing storage ownership：

> 应使用 Adopt。

---

### 3.2.3 Copy Ownership

Copy 完成后：

```text
Plugin retains native ownership

Runtime owns independent copied Payload
```

Plugin 在 submit 返回后可以：

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
external lifetime contract
```

对于：

```text
size == 0
```

允许：

```text
data == nullptr
```

但如果 AdoptedPayload 提供 ownership anchor：

> anchor 仍必须被验证。

只有 validation 成功后：

> Runtime 才取得 ownership responsibility。

---

### 3.3.2 Ownership Transfer Point

Adopt transfer 的精确语义为：

```text
Plugin calls Runtime
with Adopt input
        ↓
Runtime validates
input and anchor
        ↓
validation succeeds
        ↓
Runtime reserves required
Payload byte / object capacity
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
- 必须发生在 Runtime 能安全管理该 anchor 之后；
- **必须发生在对应 logical Payload budget / object capacity 已经成功 reservation 之后。**

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

---

### 3.3.5 Plugin Access

真正的 unique adoption 完成后：

> Plugin 不得继续访问该 backing storage。

如果 Plugin 需要继续访问：

> 应使用 Retain/shared ownership，而不是 Adopt。

---

## 3.4 Retain

`Retain` 表示：

> Payload backing storage 由 external owner 管理，但 Runtime 获得独立 lifetime anchor，并直接把该 retained byte range 作为 Runtime Payload。

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

anchor exists where required

retain ownership relation valid

release capability available

range representable
```

对于：

```text
size == 0
```

允许：

```text
data == nullptr
```

但 Retain lifetime anchor 仍必须按 Contract 验证。

只有 validation 成功，并且 Runtime 已成功 reservation 对应：

```text
Payload logical byte budget
Payload object / control capacity
```

之后，Runtime 才可以取得 independent lifetime anchor，并把它纳入：

```text
PayloadRef
resource accounting
release responsibility
```

---

### 3.4.2 Invalid Retain Input

validation 失败时：

```text
Runtime does not acquire
lifetime responsibility
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
pointer valid only while
caller remains active
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
- 不得假设 release 已成功；
- 不得牺牲 memory safety；
- 必须产生 operational diagnostic；
- 如果无法确认资源已经安全释放，可以进入 quarantine / terminal failure cleanup。

C++ V1 raw ABI release callback 是 `RODAQ_NOEXCEPT`。

```text
SDK authoring callback throws
    → SDK wrapper catches before entering raw ABI
    → records terminal diagnostic/failure

raw ABI release callback throws
    → ABI violation
    → std::terminate / abnormal process termination is expected
    → no same-process Runtime recovery guarantee
```

Runtime 只对没有逃出 raw noexcept boundary 的正常/contained failure 路径维护 exactly-once ownership bookkeeping。

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
Runtime passes
Adopt Transfer Point
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

size <= activation effective_payload_limit

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

对于 valid Adopt / Retain input：

> Runtime 仍必须先取得 required Payload resource reservation，再越过 ownership/lifetime acquisition point。

`effective_payload_limit` 由 Runtime / Storage capability 在 Starting 阶段共同建立；Storage 比 Runtime 更严格的 Payload limit 必须在这里成为 pre-gate constraint，而不能延迟到 Storage Handoff。

---

## 4.3 Payload Materialization

Payload materialization 表示：

> 建立一个能够安全进入 RuntimeRecord 的 PayloadRef。

不同 mode：

```text
Copy
    → allocate
    → exact copy

Adopt
    → validate
    → reserve Payload resource
    → acquire ownership

Retain
    → validate
    → reserve Payload resource
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

Admission v0.5 冻结：

> **Payload materialization 必须在 Gate pass 前完成。**

因此正常 Payload failure：

```text
invalid input
oversize against effective_payload_limit
payload capacity unavailable
Copy allocation failure
Copy byte-copy failure
invalid Adopt / Retain anchor
Retain control-object failure
```

都必须在 Gate pass 前收敛为：

```text
PreAdmissionRejected
```

并正确 rollback reservation / release 已经取得的 Payload responsibility。

如果实现直到 Gate pass 后才首次发生所谓普通 Payload materialization failure：

> 该实现违反 Admission / Payload ordering contract，属于 Runtime internal correctness failure，而不是合法的普通 `AdmissionRejected` Payload path。

---

### 4.5.1 Copy Failure Lifetime Rule

对于 Copy：

> allocation 和 byte copy 必须全部在 BorrowedInput synchronous input lifetime 内，并且必须在 Gate pass 前完成。

正确顺序为：

```text
BorrowedInput valid
        ↓
reserve required resource
        ↓
allocate / copy exact bytes
        ↓
stable PayloadRef
        ↓
final pre-gate revalidation
        ↓
Gate pass
```

不得设计为：

```text
Gate pass
    ↓
later Copy
```

如果普通 Copy 无法在该 synchronous lifetime 内完成，Plugin 必须改用 Adopt / Retain，而不是延迟裸 BorrowedInput。

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

## 4.8 Submit Activity Lifetime

Payload materialization 发生在 Gate pass 前，因此 Stop / failure cutoff 可能发生在：

```text
Runtime already owns / retains PayloadRef
+
submission has not Gate-passed
```

的窗口。

Runtime v0.5 的：

```text
active_submission_calls
```

必须覆盖该窗口。

因此 activation teardown 不得仅依据：

```text
global admission in-flight == 0
```

释放 Submit-path state。还必须保证：

```text
no new Submit can begin
+
active_submission_calls == 0
```

这样 pre-gate rejection cleanup 才能安全完成：

```text
PayloadRef destruction / release
reservation rollback
external anchor exactly-once release
```

该 barrier 不改变 Payload ownership 语义；它只是保证 ownership cleanup 所依赖的 Runtime state 在 synchronous Submit 返回前仍然有效。

---

## 4.9 Exactly-once Release

所有 ingress mode 都必须满足：

```text
Runtime acquires
lifetime responsibility
        ↓
exactly one final
physical release
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

Runtime 发起 Record Handoff 前，必须先为该 Storage operation 建立 Runtime-owned operation state，并保持当前调用所需 Payload backing 有效。

Storage 获得一个 read-only `PayloadView`。V1 把异步发布线性化点定义为 Plugin-internal **HandoffCommit**：

```text
Plugin irrevocably commits to
submit() == OK
and payload axis == PAYLOAD_PENDING
```

HandoffCommit 不对 ABI caller 建立 HandoffAccepted；HandoffAccepted 仍由后续 `submit() return == OK` 建立。

在这个 HandoffCommit 之前，Plugin 不得把 PayloadView 发布给跨调用 worker/deferred consumer。

一旦该决策成立，Plugin 可以发布 worker；worker completion 可以与 `submit()` return 竞态。此时 Runtime 已经预建 operation tracking，并把对应 Payload Access Lease 视为 provisional-active，直到 return/reconcile 确认结果。

`submit()` 的成功 return 是对该 irreversible handoff decision 的 ABI confirmation，不是允许异步发布的唯一物理时点。

lease 期间：

```text
Runtime keeps Payload alive
Storage may read Payload asynchronously
```

lease 结束于：

```text
PayloadConsumed
```

---

## 5.4 Asynchronous Access

Storage Plugin 只有在已经到达 HandoffCommit、不可逆决定：

```text
submit() will return OK
payload_state == PAYLOAD_PENDING
```

后才可以把 PayloadView 发布给异步 worker。

如果 worker 在 caller 观察到 `submit()` return 前就完成，Runtime 只记录 early completion event；**不得在 submit call/reconcile 结束前因为该 early event 提前释放当前调用仍可能读取的 Payload backing。**

return/reconcile 后：

```text
OK + PAYLOAD_PENDING + early payload_consumed
    → Payload axis terminalize exactly once
    → then Payload backing may be released
```

并且任何异步读取都必须发生在 PayloadConsumed terminalization 之前。

如果 Handoff 在 HandoffCommit 建立前明确返回 `FatalFailure` / function-level non-OK：

> return 后不得仍有 Storage execution context 保留 / 访问该 PayloadView。

如果 Runtime-side progress timeout 发生时 Handoff execution 是否已经停止不可证明：

> timeout 不自动等价于 PayloadConsumed；Runtime 必须先建立 Storage failure quiescence，再释放 Payload。

Runtime 在此期间必须保持 PayloadRef 有效。

---

## 5.5 PayloadConsumed

`PayloadConsumed` 表示：

> **Storage Plugin 已永久结束对 Runtime-provided Payload bytes 的所有访问。**

报告后必须满足：

```text
no Storage thread
no Storage worker
no deferred task
```

再访问该 PayloadView。

因此：

> **PayloadConsumed 之后不得再次读取、写入、保存或解引用旧 PayloadView。**

任何：

```text
PayloadConsumed
        ↓
later access old PayloadView
```

都属于：

> **Storage Contract violation**

并可能访问已经被 Runtime 释放或复用的 backing storage。

Runtime：

- 不得因为这种非法访问重新建立 Payload lifetime；
- 不得重复 release；
- 应通过 conformance instrumentation / diagnostics 识别该 violation；
- production Runtime 不承担让已经 consumed 的 stale view 再次有效的责任。

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
successful HandoffAccepted return
    ⇒
PayloadConsumed
```

此时：

> **successful HandoffAccepted return 本身同时建立并终止该 lease，构成一次 terminal PayloadConsumed completion。**

Runtime 据此：

```text
end Payload Access Lease
release Payload lifetime
when no other consumer exists
```

Storage：

> 不再额外发送 PayloadConsumed callback。

如果之后再次发送：

```text
PayloadConsumed callback
```

则属于：

> duplicate completion Contract violation。

如果 Storage Contract 没有明确声明：

```text
successful HandoffAccepted return
==
PayloadConsumed
```

则：

> Runtime 必须等待显式 PayloadConsumed completion，不得自行推断。

---

## 5.8 Completion Exactly Once

每个 Payload Access Lease：

> 恰好以一种 Contract-defined 方式结束。

可以是：

```text
explicit PayloadConsumed callback
```

或：

```text
successful synchronous
handoff return
declared as PayloadConsumed
```

但不能二者同时重复完成同一个 lease。

duplicate completion：

> 属于 Storage Contract violation。

Runtime 不得执行第二次 release。

---

## 5.9 Storage Failure

fatal Storage failure：

> 不自动终止 Payload lifetime。

Runtime 必须首先按照：

```text
§6.13 Failure Quiescence
```

尝试建立：

```text
no Storage execution context
can access Payload
```

的 guarantee。

只有 quiescence 成立后：

> 才可以释放未正常 PayloadConsumed 的 Payload。

如果无法建立 quiescence：

> 按 §6.14 进入 Quarantine。

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

V1 不要求：

```text
physical backing allocation
dedup accounting
```

因此 logical-byte accounting：

> **不等于真实 physical memory accounting。**

它与真实物理内存占用可能出现双向偏差。

---

### 6.4.1 可能高估

例如两个 PayloadRef 都是：

```text
100 bytes
```

并且二者引用同一个：

```text
100-byte backing allocation
```

中的重叠或相同区域。

V1 logical charge 可以是：

```text
100 + 100 = 200 bytes
```

而真实 backing storage 可能只有：

```text
100 bytes
```

此时：

> logical-byte accounting 高估 physical memory。

---

### 6.4.2 可能低估

例如：

```text
PayloadRef.size = 100 bytes
```

但 external owner 为维持这 100-byte view 的 lifetime，必须保留整个：

```text
4 MiB native frame buffer
```

则 V1 charge 仍然可以只是：

```text
100 bytes
```

此时：

> logical-byte accounting 低估 physical memory retention。

---

### 6.4.3 V1 Accounting 的目的

因此 V1 logical-byte accounting 的目标不是：

> 精确计算 process physical memory usage。

它的目标是：

- 为 Payload logical workload 建立统一可预测的有界策略；
- 让 Copy / Adopt / Retain 进入同一 resource admission model；
- 防止 external zero-copy 完全绕过 Payload resource accounting；
- 避免引入复杂 backing-allocation ownership graph。

如果未来真实部署表明：

```text
large external allocation
+
small Payload subview
```

使 logical accounting 无法充分控制实际 memory pressure，则可以增加：

```text
external retained physical-byte estimate
```

等独立 resource dimension。

这属于 future profile / implementation extension，不改变 Payload semantics。

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

## 6.6 Runtime-owned Copy Allocation

V1 Runtime Copy path 必须使用 activation-owned、显式配置的
`PayloadPool` size-class 架构。单一 block size 不满足 V1 standard mixed
workload，因为 large camera Payload 和 high-rate small joint Payload 必须使用
隔离的 object capacity。

每个 activation 的配置至少包括：

```text
ordered set of Payload size classes

for each class:
    maximum payload bytes
    block count
    physical byte capacity
```

Reference profile 的初始 class 建议：

```text
1 KiB
4 KiB
16 KiB
64 KiB
256 KiB
1 MiB
4 MiB
large configured block
```

这些尺寸可以在 benchmark 后调整，不属于 Stable ABI。以下语义属于 V1 契约：

```text
1. 选择能够容纳 Payload 的最小 class。
2. class selection 发生在 Gate Pass 前。
3. 对应 class 的 object capacity 必须已经可用。
4. class 之间默认不互相挪用预留给其它 class 的 capacity。
5. class 耗尽必须返回 ResourceExhausted/Admission rejection。
6. Gate Pass 后不得 fallback 到普通 heap 或其它无限 allocator。
7. Stop / failure 后所有 class accounting 必须归零。
```

Payload logical-byte budget 与 class object capacity 是两个独立维度。实现必须同时
维护：

```text
logical payload bytes in flight
occupied blocks per class
physical bytes reserved per class
high-water per class
```

small Payload 不得因为 large class 有剩余空间而占用数 MiB block；large Payload
也不得因为 small class 的 object 配额而无法进入自己的 bounded class。

如果后续配置允许 class overflow/fallback，它必须是显式、有界且由统一
ResourceBudget 预留的策略，不能成为隐式 malloc fallback。

本文不冻结 allocator backend。mimalloc 等 backend 只能为这些 bounded class
提供 backing allocation，不能替代 size-class selection、capacity isolation 或
Reservation/rollback 语义。

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

PayloadRef object 本身的 move/copy synchronization：

> 仍由其具体 implementation contract 定义。

immutable Payload bytes：

> 不意味着同一个 ownership handle 可以被多个线程无同步地同时 move。

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

Record 的 Canonical history 仍然保持：

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

Runtime 必须首先执行：

```text
§6.13 Failure Quiescence
```

如果不能建立 quiescence：

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

contract-defined terminal
failure barrier
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

not depend on
destroyed Runtime state

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
- 必要时进入 Failed cleanup / Quarantine。

对于 C++ implementation：

> release function SHOULD be `noexcept`。

SDK authoring layer 必须在进入 raw ABI `release(...) RODAQ_NOEXCEPT` 前捕获可捕获的 C++ author callback exception，并转换为 Plugin-local terminal diagnostic/failure state，不允许异常穿越 raw ABI。

如果异常已经逃出 raw `RODAQ_NOEXCEPT` release callback：

> **这是进程级 ABI violation；C++ `std::terminate` 行为是允许且预期的，Runtime 不承诺同进程恢复。**

测试必须用 subprocess/death-test 验证 raw noexcept violation，而不能要求 Runtime 外层 catch 后继续收敛。Runtime 仍必须保证在正常/SDK-contained failure path 下不会主动重复调用 release。

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

Runtime 必须在该 lifetime 内完成所有 byte read 和 Copy materialization。

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
2. validation 成功后取得 lifetime responsibility；
3. 直接把 retained backing storage 作为 Runtime Payload 使用。

---

## 7.2 Acquisition Plugin Responsibilities

Acquisition Plugin 负责：

```text
native buffer interpretation

decide Copy / Adopt / Retain

provide structurally valid anchor

guarantee Payload immutability

flatten fragmented native payload
when V1 Runtime requires
contiguous bytes

handle first sample
before Source registration

handle Binding-switch pause
```

Runtime generic boundary 不得暴露：

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
before Adopt / Retain acquisition

materialize Copy
within BorrowedInput lifetime
and before Gate pass

accept Adopt / Retain
after required reservation

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

whether successful HandoffAccepted return
itself constitutes PayloadConsumed

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
is independent of admission outcome

Adopt / Retain transfer requires
Payload resource reservation first

Copy must read BorrowedInput
only during synchronous
input lifetime

V1 does not support
delayed Copy + anchor
hybrid ingress mode

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

active_submission_calls integration
for pre-gate ownership cleanup
```

并必须满足：

> **Copy allocation / materialization timing 不得晚于 BorrowedInput synchronous input lifetime 结束。**

如果 Admission design 无法满足这一点：

> 对应 submission 必须使用 Adopt / Retain，而不能继续使用 Copy。

---

## 7.6 与 `storage_plugin.md` 的边界

Payload 已冻结：

```text
Runtime retains Payload
lifetime responsibility

Storage Handoff entry receives
call-scoped read-only view

HandoffAccepted establishes
Storage responsibility + async access lease

PayloadConsumed ends
Storage access permanently

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

### Empty Payload

```text
size == 0
may use data == nullptr

Adopt / Retain anchor
still follows normal
validation and release rules
```

### Lifetime

```text
every live PayloadRef
has a valid lifetime anchor
```

### PayloadRef Move

```text
move transfers
lifetime responsibility

moved-from PayloadRef
does not release original anchor
```

### Adopt

```text
anchor validated
before ownership acquisition

required Payload resource reserved
before ownership acquisition

ownership transfer is one-way

no rollback
```

### Retain

```text
anchor validated
before lifetime acquisition

required Payload resource reserved
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

### Delayed Input

```text
V1 has no
Copy + temporary anchor
hybrid ingress mode
```

### Release

```text
release exactly once
```

### Storage Access

```text
Storage may access
before PayloadConsumed

Storage must never access
after PayloadConsumed
```

### Synchronous Consumption

```text
successful HandoffAccepted return
may equal PayloadConsumed
only by explicit
Storage Contract

if so:
Runtime does not wait
for explicit completion

if not:
Runtime must wait
for explicit completion
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

### Accounting

```text
logical-byte accounting
is not exact physical-memory
accounting
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

### Empty Copy

提交：

```text
Copy
size = 0
data = nullptr
```

验证：

> 合法建立 zero-length Payload，并正常完成生命周期。

---

### Empty Adopt

提交：

```text
Adopt
size = 0
data = nullptr
valid ownership anchor
```

验证：

```text
anchor validated
        ↓
Runtime acquires ownership
        ↓
anchor exactly-once release
```

分别覆盖：

```text
accepted
pre-admission rejected
admission rejected
failure
```

路径。

---

### Empty Retain

提交：

```text
Retain
size = 0
data = nullptr
valid lifetime anchor
```

验证：

> Runtime 仍取得 lifetime responsibility，并最终 exactly-once release。

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
BorrowedInput
after submit return
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

该测试：

> SHOULD 使用 allocator replacement、failure-injection allocator 或等价 deterministic fault-injection mechanism。

如果目标 implementation 无法提供可控 allocation fault injection：

> 可以记录为 conformance test infrastructure limitation，但不能因此删除 allocation-failure handling contract。

---

### No Delayed Copy Hybrid

构造一个测试 Plugin，试图提交：

```text
BorrowedInput
+
temporary owner anchor
+
request Runtime copy later
```

验证：

> generic V1 Payload API 不提供这种 ingress mode。

Plugin 必须选择：

```text
Copy
Adopt
or
Retain
```

之一。

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

最终 exactly-once release。

---

### Invalid Adopt Anchor

提交：

```text
missing / structurally invalid
ownership anchor
```

验证：

```text
Runtime does not
acquire ownership

submission
pre-admission rejected

Plugin remains responsible
for original storage
```

并产生 Contract violation diagnostic。

---

### Adopt Pre-admission Rejection

使用合法 Adopt input。

Runtime 已完成 ownership transfer 后使 submission 无法通过 Gate。

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

Runtime releases
anchor exactly once
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

如果 implementation 支持跨线程 move：

> 还必须验证其 object-level synchronization contract 不会产生 double release 或 lost release。

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

测试不要求：

> logical accounting 与实际 physical RSS 精确相等。

---

### Logical Accounting Overestimate

构造多个 PayloadRef 引用相同或重叠 backing region。

验证：

> budget 可以按各自 logical size 累加，高于 unique backing memory。

---

### Logical Accounting Underestimate

构造一个小 PayloadView，但其 lifetime anchor 必须保留一个大型 native allocation。

验证：

> V1 budget 仍可按 Payload logical size 计费。

并确认这是：

> 当前 V1 accounting approximation，而不是 physical memory estimator。

---

### Async Storage

Storage 在已经不可逆决定 `submit() == OK + PAYLOAD_PENDING` 后发布 worker；worker 可以与 `submit()` return 竞态并继续读取 Payload。

验证：

- HandoffCommit 前不得把 PayloadView 发布给跨 return worker；
- Runtime 在调用 Storage 前已建立 operation tracking / provisional lease；
- early PayloadConsumed 只暂存，不在 submit return/reconcile 前提前释放 Payload；
- reconcile 后、PayloadConsumed terminalization 前 Runtime 保持 Payload backing。

---

### Synchronous Storage Consumption

Storage Contract 声明：

```text
successful HandoffAccepted return
==
PayloadConsumed
```

验证：

```text
handoff returns
        ↓
lease terminal
        ↓
Runtime does not wait
for explicit PayloadConsumed
        ↓
Runtime may release Payload
```

---

### Synchronous Completion Followed by Callback

Storage Contract 已声明：

```text
successful HandoffAccepted return
==
PayloadConsumed
```

handoff 成功返回后，Storage 又发送显式：

```text
PayloadConsumed
```

callback。

验证：

```text
second completion detected
as duplicate
```

Runtime：

- 不执行第二次 release；
- 报告 Storage Contract violation。

---

### Explicit Async Completion

Storage Contract 未声明：

```text
successful HandoffAccepted return
==
PayloadConsumed
```

验证：

> handoff 返回后 Runtime 仍保持 Payload，直到收到 explicit PayloadConsumed。

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

异步 Storage 对同一 lease 重复发送：

```text
PayloadConsumed
```

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

测试不得通过延长 Runtime lifetime 来掩盖这一错误。

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

Record 已 `HandoffAccepted` 但未 PayloadConsumed。

触发 fatal Storage failure。

验证：

> Runtime 首先尝试建立 Storage quiescence。

quiescence 前：

> Payload 不释放。

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
- 必要时进入 Failed cleanup / Quarantine。

---

### Throwing Release Contract Violation

分两层验证：

```text
SDK authoring callback throws
    → SDK wrapper catches before raw ABI
    → records terminal diagnostic/failure
    → no exception crosses RODAQ_NOEXCEPT

raw ABI release callback throws anyway
    → subprocess death test
    → std::terminate / abnormal process termination expected
    → no same-process recovery guarantee
```

不得再把 raw noexcept violation 写成 Runtime 可以 catch 并继续同进程收敛的场景。

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
Storage Handoff call-scoped view
        ↓
HandoffAccepted
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

move-only PayloadRef
semantic baseline

Copy
Adopt
Retain

Copy materialization
within BorrowedInput lifetime

no delayed Copy + anchor
hybrid ingress mode

no naked borrow
past submit lifetime

validate Adopt / Retain anchor
before ownership acquisition

reserve Payload byte/object resource
before Adopt / Retain acquisition

Payload materialization
completes before Gate pass

active_submission_calls keeps
pre-gate cleanup state alive

empty Payload still follows
anchor ownership rules

one-way Adopt transfer

independent Retain anchor

stable Payload address

Runtime-owned
lifetime responsibility

call-scoped Storage read before HandoffAccepted

async read-only Storage access lease
only after HandoffAccepted

PayloadConsumed permanently
ends Storage access

explicit PayloadConsumed boundary

synchronous handoff return
may equal PayloadConsumed
only by explicit Contract

if synchronous completion
is declared:
Runtime does not wait
for explicit callback

if not declared:
Runtime waits for
explicit completion

PayloadConsumed independent
from persistence / durability

bounded Payload byte accounting

external zero-copy
included in budget

logical-byte accounting
is deliberately not exact
physical-memory accounting

exactly-once release

no silent Payload transformation

release Contract violation
is explicit failure

no premature release
on failure

quiescence before reclamation

memory safety before
failure cleanup completeness
```

该模型提供：

> **明确的 ownership transfer boundary、严格的 borrowed-input lifetime、永久有效的 PayloadConsumed 终止语义、有界但不冒充物理内存估算器的资源模型，以及无需依赖具体 Middleware 或 Storage Backend 的 Payload Contract。**

---

## M2 Convergence Payload Amendment

V1 Copy uses activated owned size classes. Configuration is a strictly
increasing ordered set of `{max_payload_bytes, block_count}` entries. The
smallest accommodating class is selected before Gate Pass; class budgets do
not borrow from each other and no unbounded heap fallback exists.

Payload accounting exposes logical bytes and per-class physical blocks,
physical bytes, exhaustion and high-water values. Current occupancy returns to
zero; high-water values remain available for diagnosis.

Adopt/Retain Runtime anchors carry the Acquisition Plugin instance lifetime
lease. An Accepted external anchor owns exactly-once release work reserved
before Gate Pass. Plugin release callbacks execute on bounded Release workers,
not Storage completion workers. A blocked release worker and its anchor/leases
transfer to process-wide quarantine instead of being detached or unloaded.
