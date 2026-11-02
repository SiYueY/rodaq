# RoDAQ DDS Acquisition Plugin Mapping Specification

**Version:** v0.4
**Status:** Plugin ABI Mapping Review Candidate
**Scope:** RoDAQ V1 DDS Acquisition Plugin
**Implementation Scope:** V1 design target; implementation deferred until after ROS2 + MCAP functional validation

**Normative Dependencies:**

- RoDAQ Canonical Data Model v0.9
- RoDAQ V1 Canonical Profile v0.5
- RoDAQ System Architecture Design Specification v0.5
- RoDAQ Runtime Design Specification v0.5
- RoDAQ Payload Design Specification v0.5
- RoDAQ Admission Design Specification v0.5
- RoDAQ Acquisition Plugin Design Specification v0.6
- RoDAQ V1 Plugin System Design — current Freeze Candidate

---

# 1. 目标与映射边界

本文定义 DDS Acquisition Plugin 在 RoDAQ V1 中的正式 native-to-RoDAQ Mapping Contract。

本文的规范层级以：

```text
DDS / RTPS concepts
    Topic
    DataWriter
    DataReader
    SampleInfo
    XTypes / TypeIdentifier / TypeObject
    source / reception timestamp
    instance lifecycle
```

为语义基础，不把某个 DDS vendor 的 C++ 类型提升为 RoDAQ generic contract。

V1 concrete implementation baseline 选用：

```text
eProsima Fast DDS 2.6.x
Fast CDR
C++17
```

原因是当前 Ubuntu 22.04 + ROS2 Humble 开发环境已经提供该版本，并且它能够为本文的 DDS/RTPS mapping 提供直接可执行验证。

因此本文同时区分：

```text
portable DDS mapping rule
    → normative V1 DDS semantics

Fast DDS adapter evidence
    → concrete implementation / feasibility evidence
```

如果某项能力只能通过 Fast DDS-specific API 获得，必须明确标记为 adapter capability；它不能反向成为 Runtime 或 generic Acquisition Contract 的必需类型。

DDS Acquisition 属于 V1 design / mapping validation scope，但不阻塞首个：

```text
ROS2 Acquisition + MCAP Storage
```

functional milestone。实现可在该 milestone 验证后进入。

DDS Record & Replay 作为工程参考，不作为 RoDAQ runtime dependency。

数据路径：

```text
DDS DomainParticipant / Topic / DataReader
        ↓
data-bearing sample + SampleInfo
        ↓
DDS Acquisition Plugin
        ↓
DDS native facts
        ↓
Runtime registration / Binding
        ↓
Runtime::Submit
```

V1 保持：

```text
one Canonical Stream
    → one owning DDS Plugin
    → one active acquisition path
    → one current Binding
    → zero or more DataWriter Source
```

ROS2 与 DDS 即使最终连接的是同一 DDS network：

> Runtime 也不进行自动跨 Plugin Stream merge。

## 1.1 Native Evidence

DDS `SampleInfo` 语义至少需要考虑以下信息；Fast DDS 2.6.x concrete baseline 可直接提供：

```text
sample_state
view_state
instance_state
source_timestamp
reception_timestamp
instance_handle
publication_handle
valid_data
sample_identity
related_sample_identity
```

其中：

```text
valid_data == false
```

可能表示 instance/liveliness state change，而不是 application data。

Fast DDS 的 `SampleIdentity` 可以包含 writer identity + sequence number，但官方文档明确把 `SampleInfo.sample_identity` 定义为：

> **requester-replier configuration 的扩展。**

因此普通 Topic data acquisition：

> **不得把 `sample_identity` 当作通用 Writer identity / SourceSequence source。**

普通 sample 的 Writer 关联使用 `publication_handle`；Fast DDS-specific Mapping 再利用其 `InstanceHandle_t ↔ GUID_t` 语义恢复 RTPS Writer GUID。

因此 DDS Mapping 明确区分：

```text
portable DDS semantics
+
implementation capability
```

---

## 1.2 参考实现、标准边界与工程取舍

DDS Mapping 的主要工程参考为：

```text
eProsima DDS Record & Replay
https://github.com/eProsima/DDS-Record-Replay

Fast DDS
https://github.com/eProsima/Fast-DDS
```

其中 DDS Record & Replay 是最直接的工程参考，因为它已经真实处理：

```text
DDS automatic discovery
DynamicTypes / TypeObject
data samples with writer identity
writer sequence
publish / reception timestamps
unknown type arrival
MCAP output
SQLite output
resource limits
compression
filtering / partitions
```

### DynamicTypes 与自动发现

DDS Record & Replay 的核心经验是利用 Fast DDS DynamicTypes，使 Recorder 不需要在编译期知道所有 Topic type。

这正是 RoDAQ DDS Plugin concrete Fast DDS adapter 应采用的方向：

```text
Participant discovery
    ↓
Topic / TypeIdentifier / TypeObject
    ↓
DynamicType resolution
    ↓
Runtime Schema / Stream registration
    ↓
DataReader activation
```

不建议 V1 为每个 DDS type 生成/编译 C++ message class。

### unknown type 与 late type information

DDS Record & Replay 明确支持：

```text
data arrived
but type information not yet available
```

并提供：

```text
max-pending-samples
only-with-type
```

等策略。

这个实践直接验证了 RoDAQ 之前设计的：

```text
late schema
bounded pending state
type-only vs wait-for-exact-schema policy
```

是现实需求。

RoDAQ V1 DDS Mapping 推荐默认：

```text
require_type_for_binding = true
```

即没有足够 type/schema/encoding facts 时：

```text
native flow = Pending
no Active Binding
```

为了减少启动阶段 sample 丢失，可以提供 bounded：

```text
pending_type_samples
```

但绝不能像“直到 TypeObject 到来”为止无限缓存。

即使以后证明 raw serialized payload 可以在没有完整 TypeObject 的情况下获得，normal Admission 仍必须保留可靠 TypeIdentifier；最多退化为显式 `rodaq/dds/type-only-v1`，不能退化为丢失 type identity 的 schema-absent typed Stream。

### type information 持久化

DDS Record & Replay 默认可以记录 type information，并且会保存依赖 type，使 replay 端能够独立恢复 DynamicTypes。

RoDAQ 应吸收的不是其具体 MCAP attachment 格式，而是：

> **Schema representation 应当是可独立解释的 dependency-complete representation。**

所以 DDS Plugin 注册 SchemaDescriptor 时应优先获得：

```text
TypeIdentifier
TypeObject / dependency graph
or exact OMG IDL representation
```

并清楚声明 SchemaFormat。

### Writer GUID 与 Writer Sequence

DDS Record & Replay 的 SQL schema 使用：

```text
writer_guid + sequence_number
```

说明 RTPS 层确实存在这两个有价值的 native facts。

但 RoDAQ DDS V1 必须区分：

```text
Fast DDS public DDS DataReader path
    → publication_handle 可关联 Writer
    → ordinary SampleInfo 不保证暴露 RTPS writer sequence

lower RTPS / recorder-specific path
    → 可能同时取得 Writer GUID + writer sequence
```

因此当前设计冻结：

```text
Writer GUID
    → Fast DDS adapter SourceId capability

Writer sequence
    → optional future capability
       only after the selected data path proves it
```

Canonical Record ordering 始终由 AcceptanceSequence 建立；SourceSequence 缺失不会削弱 Record ordering。

### publish time / log time

DDS Record & Replay 同时记录 publication timestamp 与 reception/log timestamp。

这证明同时保存多个时间 observation 是合理需求。

但 RoDAQ 比它更严格：

> 不允许把多个不同 DataWriter 的 publisher clock 值错误标成同一个 ClockId。

所以 source timestamp 的存在本身，不代表它可以无条件成为一个 Stream-level Canonical TimestampKey。

### resource limits

DDS Record & Replay 提供：

```text
buffer-size
max-pending-samples
max-file-size
max-size
safety-margin
log rotation
```

这些配置说明 DDS Recorder 的工程问题并不只是“能收到数据”，而是：

```text
未知 type 等待必须有界
内部 buffering 必须有界
Storage 输出必须有资源策略
```

RoDAQ 已把这三类资源分别放在：

```text
Acquisition Plugin local bounded state
Runtime Admission
Storage Plugin resource contract
```

因此不复制 DDS Record & Replay 的 storage coupling，而是保留职责分层。

### DataReader loan

Fast DDS `read/take` 可以把 receive queue 内部 buffer loan 给应用，并要求 `return_loan()`。

这是未来 zero-copy 优化的重要基础，但也意味着：

```text
loan retained too long
    → DataReader internal resource exhaustion
```

所以 V1 首个 DDS concrete implementation 仍建议：

```text
Fast DDS sample
    ↓
serialize/copy into Runtime Payload
    ↓
return loan promptly
```

而不是一开始就让 Runtime/Storage 长时间持有 DataReader loan。

等 Copy path correctness 与性能数据明确后，再评估 Adopt/Retain + return_loan anchor。

### 不直接复用 DDS Record & Replay 的原因

DDS Record & Replay 是完整 Recorder 产品，它把：

```text
DDS acquisition
MCAP writer
SQL writer
controller
rotation
```

组合在同一个系统中。

RoDAQ 的目标不同：

```text
Acquisition Plugin
    ↓
plugin-independent Runtime
    ↓
independent Storage Plugin
```

因此我们参考其 native DDS 处理方式，但不复制其 acquisition-storage coupling。

---

# 2. DDS Native Flow 与 Canonical Registration

## 2.1 Native Logical Stream

V1 中一个 DDS native logical flow 至少由：

```text
DDS domain / participant mapping domain
+
Topic name
+
registered type identity
```

区分。

推荐：

```text
StreamName = DDS Topic name
```

但 Topic name 仍然不是 Runtime Canonical identity key。

同名 Topic 如果存在不兼容 Type：

> 不得静默合并。

DDS Plugin 负责判断其 own native domain 内的 flow continuity。

Runtime 只负责：

```text
validate registration
allocate StreamId
own Descriptor Registry
own Binding
```

Runtime 不比较 DDS facts 与 ROS2 facts 来判断：

```text
same logical Stream ?
```

## 2.2 Schema / Type

DDS serialized Payload 的解释至少依赖：

```text
type identity
+
serialized encoding
```

因此 V1 DDS Mapping 与 ROS2 Mapping 使用相同的最低原则：

> **任何进入 normal Admission 的 typed DDS StreamDescriptor 都必须保留 TypeIdentifier；不能因为 TypeObject / IDL 暂时不可得而把 type identity 一并丢失。**

DDS 类型信息可能来自：

```text
static TypeSupport
DDS-XTypes TypeIdentifier / TypeObject
IDL
DynamicType
implementation-specific type support
```

V1 logical mapping：

```text
TypeIdentifier
    = exact UTF-8 DDS type name used to select
      the top-level type inside the Schema representation

PayloadEncoding
    = dds/xcdr1
      dds/xcdr2
      or exact implementation-declared serialized encoding
```

`TypeIdentifier` 是 Canonical UTF-8 Identifier，因此：

> **不得把 binary XTypes `TypeIdentifier` / `TypeObject` raw bytes 直接拼入该字段。**

如果 DDS XTypes equivalence identity 用于区分同名但不兼容的 type revision，它属于 native flow/type-continuity evidence；结构差异最终通过新的 `SchemaDescriptorId` 和 exact Schema representation 保留，而不是通过把 opaque binary identity 塞进 Canonical `TypeIdentifier`。

如果存在 exact IDL：

```text
SchemaFormat = omg/idl
representation = exact IDL bytes selected by the Mapping
```

如果 source-native exact representation 是 XTypes TypeObject / dependency graph，而不是 IDL：

> 使用单独、明确且 versioned 的 SchemaFormat 保存 exact TypeObject representation；不得先生成“看起来等价”的 IDL 再声称 byte-exact native schema fidelity。

V1 DDS Mapping 允许：

```text
SchemaFormat = dds/xtypes/type-object-v1
```

但只有 concrete DDS adapter 已冻结并验证 deterministic、dependency-complete binary bundle 后才能使用。该 bundle 至少包含：

```text
top-level XTypes identity evidence
exact TypeObject bytes
all required dependent TypeObject bytes
explicit entry lengths and deterministic ordering
```

Fast DDS implementation 尚未完成该 bundle 的 executable validation 时，应优先使用 exact self-contained `omg/idl`；否则退化到明确的 `rodaq/dds/type-only-v1`，不能生成半完整 TypeObject bundle。

如果 exact structural representation 暂时不可得，但 type identity 已可靠获得，可以显式使用：

```text
SchemaFormat
    = rodaq/dds/type-only-v1

TypeIdentifier
    = exact DDS type identity

representation
    = empty byte string
```

该模式只保存 type identity，不声称 Artifact self-contained。

如果连可靠 TypeIdentifier 都不可获得：

> 该 flow 不能进入 normal V1 Admission。

如果 DDS API 只暴露 deserialized C++ object，Plugin 有两种合法选择：

```text
A. reserialize to a clearly declared logical DDS encoding

B. use lower serialized / RTPS access path
   to preserve source bytes
```

但不允许：

```text
reserialized bytes
    ↓
claim they are exact original wire bytes
```

`PayloadEncoding` 必须描述实际产生的 byte representation。

## 2.3 Data-bearing Sample 与 DDS Lifecycle Sample

V1 定义：

```text
valid_data == true
    → candidate Canonical Record

valid_data == false
    → no ordinary Canonical data Record
```

因此以下 native semantics 当前不变成普通 Record：

```text
DISPOSED instance
NO_WRITERS instance
sample_state
view_state
sample_rank
generation_rank
```

它们可以：

- 影响 Source/Stream operational state；
- 进入 diagnostic/observability；

但不得伪装成 Payload Record。

所以当前 DDS V1 的目标是：

> **完整采集 data-bearing DDS samples。**

不是：

> exact replay complete DDS instance lifecycle。

如果未来明确要求 dispose/unregister/no-writers exact replay：

> 应重开 Canonical capability 评审，而不是引入 Generic Metadata hack。

## 2.4 Descriptor Evolution

如果 DDS Topic 的 type identity 变为不兼容类型，V1 默认形成新的 native logical flow / Stream。

在 Topic/type continuity 已经成立的前提下，以下 interpretation 变化需要新的 StreamDescriptor：

```text
compatible schema evolution / schema representation
PayloadEncoding
TimestampKeySet
Clock semantics
```

执行 generic Binding switch，不修改旧 Descriptor。

---

## 2.5 V1 DDS Plugin 内部组织

DDS Plugin 可以保持为一个紧凑对象；Fast DDS adapter 不要求拆出多个 Manager。

内部需要维护的核心状态：

```text
DomainParticipant
Subscriber
native topic/type discovery state
DataReader table
TypeIdentifier / TypeObject cache
native flow → Runtime Binding mapping
Writer GUID → SourceHandle cache
pending type-resolution state
quiescence state
```

### Participant / Domain

V1 一个 DDS Plugin instance 对应一个明确配置的：

```text
DDS DomainId
+
Participant configuration
```

不同 DomainId 不应在一个 Plugin instance 中隐式合并。

如果需要同时采集多个 DDS domain：

> 使用多个 DDS Acquisition Plugin instance。

这与 Runtime 支持多个 Acquisition Plugin instance 的拓扑一致。

### Discovery / Type Resolution

Control Plane 监听 Participant/Endpoint discovery，至少获取：

```text
Topic name
Type name
TypeIdentifier / TypeObject when available
Writer GUID
Writer QoS / partition facts needed for subscription
```

discovery callback 不直接创建 Canonical Record。

推荐流程：

```text
new topic/type observed
    ↓
resolve DynamicType
    ↓
construct native flow facts
    ↓
Runtime registration
    ↓
create/activate DataReader only when Binding can become usable
```

如果实际 Fast DDS API 要求先有 DataReader 才能获得某些 type/data facts，也可以提前创建 Reader，但必须保持：

```text
normal Runtime::Submit disabled before Binding Active
```

### DataReader processing

`on_data_available()` 不应该只 `take_next_sample()` 一次。

Fast DDS 文档明确指出：

> 多个 sample 同时到达时 callback 可能只触发一次。

因此 callback/worker 必须循环 take，直到：

```text
RETCODE_NO_DATA
```

对于每个 sample：

```text
check SampleInfo.valid_data
    ↓
resolve Writer Source
    ↓
extract sequence / timestamps
    ↓
serialize exact declared PayloadEncoding
    ↓
Runtime::Submit
```

### DynamicData 与 serialization

第一版 Fast DDS 实现推荐以 DynamicTypes 路径获得 type knowledge，但 Canonical Payload 必须保持：

```text
serialized logical DDS bytes
```

而不是 DynamicData object tree。

如果 high-level API 返回 DynamicData：

> 必须使用明确的 Fast CDR/XCDR serialization path，并把实际 encoding 注册为 PayloadEncoding。

如果未来选择 lower RTPS serialized payload path，则可以避免一次 reserialization，但需要单独审查：

```text
payload ownership
inline QoS / encapsulation
fragmentation
loan lifetime
```

不能为了 zero-copy 直接把 RTPS internal buffer 暴露给 Runtime。

### pending type samples

如果启用 bounded pending-type queue，必须按 native flow/type 分组，并至少限制：

```text
max pending samples
max pending bytes
max wait duration
```

达到上限后不能无限增长。

策略必须显式配置为：

```text
reject/drop pre-Runtime native sample
or
type-only fallback if reliable TypeIdentifier is available and configuration permits
```

这类 sample 尚未进入 Runtime Admission，因此不消耗 AcceptanceSequence。

---

## 2.6 Stable Plugin ABI Mapping

DDS Plugin 的 Fast DDS / DDS native object 不直接进入 Plugin ABI。跨 DSO contract 统一采用 Acquisition Family Protocol。

### Lifecycle

```text
create
    → 构造 Plugin-local state

bind
    → query rodaq.runtime.common.v1
    → query rodaq.runtime.acquisition.v1
    → 按需 query rodaq.runtime.canonical.v1
    → require rodaq.acquisition.control.v1

configure
    → 通过 Runtime-owned ConfigView 读取 Domain / Participant / discovery / type-resolution 配置

start
    → 创建或激活 Participant / discovery / DataReader path
    → 完成 required canonical registration / Binding readiness
    → Runtime Activation Commit 后才允许 normal Submit

quiesce_plugin (required control extension)
    → instance-wide non-destructive barrier
    → blocks current/future DDS receive/discovery routing into Runtime submit
    → preserves DDS-native release capability needed by Runtime-owned Payload anchors

stop
    → destructive DDS-native shutdown only after Runtime drain / anchor safety
    → return 后 no future DDS receive/native worker can access Runtime

destroy
    → 只在 callback / loan / retained anchor / native release capability 安全收敛后调用
```

### Registration

DDS native facts与 operational state 通过 `rodaq.runtime.acquisition.v1` 映射为：

```text
Source
Clock
Schema
Stream
StreamDescriptor
```

Fast DDS GUID、TypeObject、DataReader、loan handle 等 native type 不得泄露进 Stable ABI。

### Data Submission

DDS serialized/re-serialized payload 使用：

```text
rodaq_record_submission
    stream / descriptor
    optional SourceHandle
    optional SourceSequence
    explicit timestamp_values[]
    payload
+
rodaq.runtime.acquisition.v1::submit()
+
rodaq_acquisition_submit_result
```

其中 Writer identity 已 canonicalize 并完成 registration 时按 Record 填入 `SourceHandle`；identity 真正 unavailable 才允许 `source_present = false`。已知 Writer 但 SourceHandle 尚未注册时不得伪装成 absent。

Fast DDS RTPS timestamp 保持 exact `1 / 2^32 s` Clock resolution；`timestamp_values[]` 保存 signed tick reading。`INFINITE` / invalid / unavailable 映射为 explicit Absent，合法零值必须保持 `Present(0)`。Runtime 自己注入 `rodaq/acquisition_ingress`。

Plugin 必须处理三个正交维度：

```text
AdmissionOutcome
RejectionReason
PayloadDisposition
```

Copy path：

```text
PayloadDisposition = NOT_APPLICABLE
```

如果未来把 DataReader loan 或其他 native storage 通过 Retain 暴露给 Runtime，则 disposition 只决定本次 retained anchor 谁负责 release；它不转移 Plugin 对原 DDS object 的所有权。

函数级 `rodaq_status != OK` 是 Runtime boundary failure，不是普通 DDS backpressure。

---

# 3. Source、Sequence、Clock、Timestamp 与 Payload

## 3.1 SourceId

DDS DataWriter 是 natural Source。

优先使用：

> stable DDS/RTPS writer GUID 或实现提供的等价 writer identity。

以下 native handle：

```text
publication_handle
instance_handle
```

如果仅具有 process-local / reader-local 语义：

> 不得直接自动成为 Canonical SourceId。

DDS SourceId 的 canonical byte domain 必须与 ROS2 domain disjoint。

同一组 RTPS GUID bytes 也不能脱离 DDS Domain identity 单独比较：不同 DDS DomainId 可以存在相同 GUID bytes，但它们不是同一个 native Source。

因此 V1 DDS SourceId identity domain 至少包含：

```text
DDS DomainId
+
writer identity scheme
```

V1 DDS SourceId 使用 versioned、scheme-tagged binary domain：

```text
ASCII "rodaq.source.dds.v1\0"

u32 dds_domain_id

u16 identity_scheme_length
N   identity_scheme bytes

u16 identity_length
M   canonical writer identity bytes
```

`identity_scheme` 必须明确声明 concrete adapter 使用的 writer identity contract。

Fast DDS 2.6.x concrete adapter 采用：

```text
dds_domain_id   = configured Fast DDS DomainId
identity_scheme = "rtps-guid-v1"
identity        = 16-byte RTPS Writer GUID
```

其中 Fast DDS `GUID_t`：

```text
GuidPrefix_t   12 bytes
EntityId_t      4 bytes
```

按以下固定顺序编码：

```text
guidPrefix.value[0..11]
entityId.value[0..3]
```

这个 `rodaq.source.dds.v1` prefix 保证 DDS identity domain 与 ROS2 SourceId domain disjoint；`dds_domain_id` 防止跨 DDS Domain 的 GUID byte collision；scheme tag 又允许未来其它 DDS adapter 使用不同、但仍 deterministic 的 writer identity representation。

Fast DDS ordinary Topic sample 的具体 Writer identity 路径为：

```text
SampleInfo.publication_handle
        ↓
Fast DDS adapter resolves InstanceHandle_t → GUID_t
        ↓
rtps-guid-v1 canonical identity
```

目标 Fast DDS 2.6.x 提供 `InstanceHandle_t` / `GUID_t` 互转能力；Plugin 可以结合 matched-publication discovery / `get_matched_publication_data()` 对 handle 与已发现 Writer 的关联做 Control Plane consistency validation。

其它 DDS implementation 若不能证明其 writer identity representation 在当前 active Scope 内 stable、producer-scoped 且 collision-safe：

```text
SourceId = absent
```

`SampleInfo.sample_identity.writer_guid` 只可在明确的 requester-replier 场景中作为额外 evidence，**不得作为普通 Topic SourceId 的必要来源。**

不得把 `publication_handle` 格式化成实现相关字符串作为 Canonical identity。

如果无法取得 stable writer identity：

```text
SourceId = absent
```

不得使用：

```text
Topic name
instance handle
arrival index
AcceptanceSequence
```

制造假的 SourceId。

## 3.2 SourceSequence

Canonical `SourceSequence` 是 optional capability。

portable DDS `SampleInfo` 对普通 Topic sample 不保证暴露一个可直接作为 Writer-originated transport sequence 的字段；Fast DDS public DDS `SampleInfo` 同样不能把 requester-replier `sample_identity.sequence_number` 当作普通 Topic sequence。特别是：

```text
sample_identity.sequence_number
```

属于 requester-replier extension，不能被普通 recorder 无条件使用。

因此 V1 portable DDS baseline 冻结为：

```text
ordinary DDS Topic sample
    → SourceSequence absent
```

这不会影响：

```text
AcceptanceSequence
same-Stream ordering
SourceId
```

如果 concrete DDS adapter 选择并验证了：

```text
lower RTPS serialized/cache-change path
or another Fast DDS API
```

能够对每条 ordinary data sample 稳定取得真正的 Writer-originated `SequenceNumber_t`，则可以启用 SourceSequence capability。该 conversion 必须遵守：

```text
SequenceNumber_t::unknown() = {-1, 0}
    → absent

high < 0
    → invalid / absent + diagnostic

otherwise
    → (uint64(high) << 32) | low
```

并且必须证明该 sequence 与 `SourceId` 表示的同一个 DataWriter identity domain 对齐。

无论 concrete adapter 是否取得 Writer-originated sequence：

```text
SourceId absent
    → SourceSequence absent
```

不得只保存 sequence 而丢失其 producer identity。

以下永远不能替代 SourceSequence：

```text
sample_rank
generation_rank
absolute_generation_rank
reader local order
queue index
AcceptanceSequence
```

---

## 3.3 Clock / Timestamp

所有 DDS Stream 仍有：

```text
rodaq/acquisition_ingress
```

由 Runtime 写入。

DataReader side 可选 timestamp 推荐：

```text
dds/reception_timestamp
```

如果 reception timestamp 属于一个稳定 reader-side clock domain，并且能建立合法 ClockDescriptor，则可以成为 Canonical timestamp。

### DataWriter Source Timestamp

DDS `source_timestamp` 来自 DataWriter side。

多个 Writer：

> 可能对应不同物理时钟。

因此和 ROS2 一样，不能把多个 writer timestamp 都映射成：

```text
same Role + same ClockId
```

除非 Mapping/configuration 能证明：

> 所有相关 Writer 共享同一个 source clock domain。

默认：

```text
multi-writer / unproven source clock
    → source_timestamp not registered as Canonical key
```

证明 common clock 后可以增加：

```text
dds/source_timestamp
```

这个限制是为了保持：

```text
same ClockId
→ directly comparable readings
```

而不是为了减少 timestamp 数量。

## 3.4 DDS Time Representation 与 Fast DDS Adapter

DDS/RTPS timestamp 语义允许 second + fractional-second 表示。这里必须区分 Fast DDS 的 DDS-layer `fastrtps::Time_t` 与 RTPS-layer `fastrtps::rtps::Time_t`。

`SampleInfo.source_timestamp` / `reception_timestamp` 使用的是 RTPS-layer `fastrtps::rtps::Time_t`；其 concrete representation 可提供：

```text
int32 seconds
uint32 fraction
1 fraction tick = 1 / 2^32 second
```

同时还提供 `to_ns()` convenience conversion。

RoDAQ 不应默认使用 `to_ns()` 作为 full-fidelity representation，因为 `1 / 2^32 s` 不能被整数 nanosecond 精确表示。

在进行数值转换前，Fast DDS adapter 必须先处理 native special values。目标 2.6.x `Time_t` 暴露：

```text
INFINITE
INVALID / implementation-defined invalid state
```

这些值不是普通时间 observation，不能机械转换成合法 Canonical `ClockReading`。

规则：

```text
finite valid Time_t
    → exact conversion allowed

INFINITE / invalid / unavailable
    → TimestampState = Absent
       + optional diagnostic
```

如果某个 DDS implementation 对“零值”定义为 unavailable sentinel，也必须由该 concrete adapter 显式判断；generic Runtime 不得把 `0` 自动解释为 absent。

V1 可以直接构造 exact ClockDescriptor：

```text
resolution_numerator   = 1
resolution_denominator = 4294967296   // 2^32
```

并把 native time 映射为：

```text
ClockReading
= int64(seconds) * 2^32 + fraction
```

这个映射刚好覆盖完整：

```text
int32 seconds + uint32 fraction
```

到 Profile v0.5 `int64 ClockReading`，范围从 `INT64_MIN` 到 `INT64_MAX`，不会丢失 fraction precision。

因此对于 Fast DDS concrete RTPS time：

> **优先保存 exact 2^-32-second ticks，而不是纳秒化后再声称 full fidelity。**

只有当某个 concrete API 明确提供的是 DDS-layer nanosecond-domain `fastrtps::Time_t`，并且该值本身就是 Plugin 所要保存的 source-native observation，才可以使用 `1 / 10^9` ClockDescriptor。

不能把 RTPS `fraction` 先调用 convenience `to_ns()` 后再声称 exact 2^-32-second fidelity；两种 representation 不能在同一个 ClockId 下混用。

## 3.5 TimestampKeySet

典型安全 DDS TKS（Fast DDS baseline）：

```text
1. rodaq/acquisition_ingress
2. dds/reception_timestamp       optional
3. dds/source_timestamp          optional only with proven common clock
```

因此当前 DDS Mapping 同样：

> 不要求提高 `MaxTimestampKeysPerSet = 4`。

## 3.6 Payload

Canonical Payload 是：

> serialized logical DDS sample bytes。

不是 C++ object graph。

Plugin 必须明确：

```text
XCDR1
XCDR2
or another exact serialization
```

并保证 accepted 后 immutable。

优先 source-native serialized path。

如果未来实现使用 typed sample reserialization：

> PayloadEncoding 必须描述 reserialization 后的 encoding，而不能冒充原始 wire bytes。

本文不提前冻结 Fast DDS-specific lower-level API；这是后续 Fast DDS implementation review 的内容。

---

# 4. Lifecycle、Discovery、Backpressure 与 Failure

## 4.1 Activation

DDS Plugin activation：

```text
prepare Participant / Reader resource
        ↓
discover Topic / Type / Writer
        ↓
build DDS Stream facts
        ↓
Runtime registration
        ↓
Storage descriptor readiness
        ↓
Binding ready
        ↓
Runtime Activation Commit
        ↓
release normal DataReader Submit permission
```

DDS middleware 可能在 Activation Commit 前就收到 sample。

Plugin 必须采用 bounded pre-enable policy：

```text
drop before Plugin Enable
or
bounded native queue without Runtime::Submit
```

不得让早到 callback 在 Runtime 仍 `Starting` 时进入 normal Submit。

## 4.2 Dynamic Writer / Source

同一 Topic/type 上新出现的 DataWriter：

> 是新的 Source，不是新的 Stream。

Writer disappearance：

```text
Source → Unavailable
```

只要其他 eligible Writer 仍在同一 active path：

```text
Stream may remain Available
```

新 Topic/type native flow：

> 走完整 Stream registration。

DDS discovery callback 不得直接修改 Runtime Registry。

对于新 Writer 必须区分：

```text
Writer identity 真正 unavailable / invalid / untrustworthy
    → SourceId absent 合法

Writer GUID / equivalent identity 已可靠获得
但 SourceHandle registration 尚未完成
    → registration-pending known Source
```

后一种情况不得伪装成 `SourceId absent`。在 SourceHandle 建立前，该 Writer 的 sample 必须：

```text
boundedly buffer
or
apply DDS/native backpressure
or
drop / pre-admission reject
```

并由 Control Plane 完成 Source registration 后才能携带该 Source 进入 Admission。

## 4.3 Backpressure

DDS 自身已经具有：

```text
History
QoS
reliability
middleware queue
```

RoDAQ Admission 是额外的 application-side bounded boundary。

因此不能宣称：

```text
RoDAQ Accepted / Rejected
==
DDS delivered / lost
```

普通 Runtime resource pressure：

```text
Reject
or
bounded / interruptible Wait
```

但长时间阻塞 DDS listener thread 可能影响 middleware progress。

因此concrete DDS implementation 应优先考虑：

```text
short native callback
    ↓
bounded Plugin-owned queue
    ↓
Plugin worker
    ↓
Runtime::Submit
```

前提是 queue：

- 有明确 capacity；
- Stop/quiesce 可关闭；
- 不形成无界复制；
- 不改变 Canonical ordering contract。

这只是实现机制，不引入新的 Canonical subsystem。

## 4.4 Failure / Recovery

Failure 可以包括：

```text
DataWriter Source loss
Topic loss
DataReader failure
Participant failure
transport failure
Plugin-wide DDS failure
```

Source loss 可以局部化。

Plugin-wide failure 后，因为 V1 每 Stream 只有一条 active acquisition path：

> 相关 Stream 全部 Unavailable，不自动 failover 到第二个 Plugin。

Recovery 只有在以下语义完全连续时才能复用现有 Binding：

```text
Topic/type identity
Schema
PayloadEncoding
Clock continuity
TimestampKeySet
```

否则必须 descriptor/binding evolution。

不得为了 recovery continuity 伪造：

```text
SourceId
SourceSequence
ClockId
```

## 4.5 Quiescence / Stop

DDS Plugin-wide quiescence 对应 `rodaq.acquisition.control.v1::quiesce_plugin()`，必须证明：

```text
no new DataReader/listener/worker
can begin Runtime::Submit
```

Runtime 再等待：

```text
active_submission_calls == 0
```

Plugin Stop 才可以关闭：

```text
Reader
Subscriber
Participant
transport/native worker
```

如果未来 Retain/Adopt 使用 DDS loaned memory：

> release capability 必须一直有效到 Runtime release。

做不到时必须 Copy。

---

# 5. Conformance 与设计审查结论

## 5.1 明确不属于当前 V1 的能力

```text
exact DDS dispose/no-writers replay

sample/view/generation rank persistence

portable SourceSequence for every DDS implementation

portable raw-wire byte access for every DDS implementation

unconditional multi-writer source_timestamp

cross-Plugin merge with ROS2 Stream

DDS path failover inside one activation
```

## 5.2 必须验证的 Cases

### Discovery

```text
Topic/type discovery
multiple writers
writer add/remove
incompatible type not merged
type identity preserved for every admitted typed payload
type-only → exact TypeObject/IDL causes Descriptor evolution
```

### Data Semantics

```text
valid_data true → Record candidate
valid_data false → no ordinary Record
instance lifecycle classification
```

### Source

```text
same Writer GUID bytes in different DDS DomainId → different SourceId
DDS DomainId + Writer GUID stable → stable SourceId in activation
publication_handle → Fast DDS GUID conversion → stable SourceId
matched-publication discovery consistency validation
publication_handle → GUID resolution is Fast DDS adapter-specific, not portable DDS semantics
DDS SourceId domain != ROS2 domain
identity unavailable → absent
```

### Sequence

```text
public DDS ordinary Topic path → SourceSequence absent
SourceId absent → SourceSequence absent
sample_identity not used as generic ordinary Topic sequence
optional lower RTPS path: valid writer sequence → present
optional lower RTPS path: unknown sentinel → absent
reader rank not SourceSequence
```

### Timestamp

```text
RTPS Time_t exact fraction ticks round-trip without to_ns precision loss
DDS-layer nanosecond Time_t never mixed into same ClockId as RTPS fraction ticks
reader reception clock exact mapping
multi-writer source clock omitted by default
common source clock optional capability
rational native resolution preserved
Fast DDS INFINITE / invalid time → Absent, never ordinary ClockReading
```

### Payload

```text
encoding explicit
source-native serialized bytes
reserialization explicitly identified
oversize pre-gate rejection
```

Copy baseline 必须验证 Stable ABI result：

```text
Accepted
    → rodaq_status == OK
    → outcome == RODAQ_ADMISSION_ACCEPTED
    → rejection_reason == NONE
    → payload_disposition == NOT_APPLICABLE

ordinary pre-gate rejection
    → rodaq_status == OK
    → outcome == RODAQ_ADMISSION_PRE_REJECTED
    → explicit rejection_reason
```

如果后续 Fast DDS loan path 使用 Retain，则必须验证本次 retained anchor 在 acquisition point 前后的 CallerResponsible / RuntimeResponsible disposition，并证明原 DDS native owner 不被该 disposition 隐式转移。

### Lifecycle

```text
early DDS callback before Activation Commit
listener/worker quiescence
Plugin Stop
loaned-buffer lifetime safety
same-semantics recovery
descriptor evolution
```

## 5.3 设计审查结论

DDS Mapping 设计提供了独立于 ROS2 的 Acquisition evidence；Fast DDS 只是当前 concrete feasibility baseline。

当前通用架构能够表达：

```text
multiple Writer Source per Stream
optional SourceSequence, including an explicit absent baseline
reader-side clock/timestamp
schema/type evolution
data-bearing sample
bounded Plugin queue / backpressure
quiescence / recovery
```

没有发现为了 DDS data acquisition 必须新增：

```text
Generic Metadata
Generic Event
SourceDescriptor
multi-path Stream
```

的证据。

当前显式边界包括：

- ordinary Topic portable API 不保证 Writer-originated SourceSequence；
- multi-writer source timestamp 没有 common-clock evidence 时不映射；
- complete DDS instance lifecycle replay 当前被裁剪；
- Writer GUID 必须与 DDS DomainId 一起形成 Scope-safe SourceId；
- XTypes binary type identity 属于 native type evidence / exact schema representation，不能直接污染 UTF-8 Canonical TypeIdentifier。

如果以后它成为真实需求，再重新评估 Canonical Model。

`MaxTimestampKeysPerSet = 4` 对当前 DDS Mapping（Fast DDS baseline）足够。

## 5.4 Implementation Status

DDS Acquisition 是 V1 design target，但 concrete implementation：

> 不阻塞首个 ROS2 + MCAP functional milestone。

正式实现前必须针对最终选定的 DDS implementation/version 再审核：

```text
serialized sample access
publication_handle → Writer GUID mapping
optional lower-path SourceSequence capability
Time_t exact representation
loaned buffer lifetime
```

## 5.5 Native Evidence 与可执行验证

当前 concrete evidence 已直接对照目标环境 Fast DDS 2.6.x headers / library，包括：

```text
fastdds/dds/subscriber/SampleInfo.hpp
fastdds/rtps/common/Guid.h
fastdds/rtps/common/SequenceNumber.h
fastdds/rtps/common/Time_t.h
```

并完成一个 C++17 小型可执行验证：

```text
GUID_t
    GuidPrefix_t 12 bytes + EntityId_t 4 bytes
    → exact 16-byte canonical writer GUID

InstanceHandle_t / GUID_t
    → Fast DDS-specific 16-byte Writer GUID mapping is available

SequenceNumber_t::unknown() = {-1, 0}
SequenceNumber_t{1, 7} = exact uint64 4294967303
    → conversion utility verified for a future lower RTPS SourceSequence path
    → NOT evidence that ordinary public DDS SampleInfo exposes writer sequence

RTPS Time_t
    seconds = 1
    fraction = 0x80000000
    → exact ClockReading = 6442450944 ticks
    → Clock resolution = 1 / 4294967296 second
```

结果：

```text
FASTDDS_MAPPING_CHECK = PASS
```

这验证了 GUID / SequenceNumber / Time_t 的底层 representation 与转换可在当前 Fast DDS 2.6.10 环境实现。

同时官方 SampleInfo contract 的自审查明确修正了一个早期假设：

> `sample_identity` 只属于 requester-replier extension，不能证明 ordinary Topic SourceSequence availability。

因此普通 DDS baseline 保持 SourceSequence absent，直到未来 lower RTPS data path 的逐 sample sequence capability 被单独验证。

Fast DDS 在这里是 V1 concrete implementation baseline；`GUID_t / SequenceNumber_t / Time_t` 等 adapter 类型不得泄漏到 generic Runtime API，也不得被写成 portable DDS requirement。
