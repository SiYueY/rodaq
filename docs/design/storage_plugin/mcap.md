# RoDAQ MCAP Storage Plugin Mapping Specification

**Version:** v0.5
**Status:** Plugin ABI Mapping Review Candidate
**Scope:** RoDAQ V1 MCAP Storage Plugin
**Implementation Scope:** Initial V1 functional implementation — required

**Normative Dependencies:**

- RoDAQ Canonical Data Model v0.9
- RoDAQ V1 Canonical Profile v0.5
- RoDAQ System Architecture Design Specification v0.5
- RoDAQ Runtime Design Specification v0.5
- RoDAQ Payload Design Specification v0.5
- RoDAQ Admission Design Specification v0.5
- RoDAQ Storage Plugin Design Specification v0.4
- RoDAQ V1 Plugin System Design — current Freeze Candidate

---

# 1. 目标、边界与参考实现

## 1.1 设计目标

本文定义 RoDAQ V1 Canonical State 到 MCAP Artifact 的正式 Storage Plugin 设计。

MCAP 是 首个 V1 functional milestone 必须实现的 Storage Backend，与 ROS2 Acquisition 一起形成第一条可运行链路：

```text
ROS2 Acquisition
        ↓
RoDAQ Runtime
        ↓
MCAP Storage Plugin
        ↓
*.mcap
```

本文同时要求 MCAP Plugin 的设计不能只适配 ROS2。后续接入 DDS Acquisition 后，同一个 Storage Contract 必须继续成立。

MCAP Plugin 直接依赖：

```text
mcap-cpp
+
RoDAQ checked file sink
```

首个 ROS2 + MCAP implementation 的目标环境当前为：

```text
ROS 2 Humble mcap_vendor
MCAP_LIBRARY_VERSION = 0.8.0
```

Mapping Contract 不绑定永久版本号，但实现必须以实际 vendored API 为准；不能把较新 mcap-cpp 的接口能力倒灌到 Humble baseline。

不把 `rosbag2_storage_mcap` 作为运行时中间层：

```text
RoDAQ Runtime
    ↓
MCAP Storage Plugin
    ↓
mcap-cpp
    ↓
checked file sink
```

这样以下语义仍由 RoDAQ 控制：

```text
Descriptor readiness
HandoffAccepted
PayloadConsumed
PersistenceSucceeded / Failed / Unknown
Flush
NormalComplete
failure quiescence
```

而不是被 rosbag2 Storage API 反向决定。

本文不冻结：

```text
具体 C++ class name
源文件拆分
线程数量
queue container
默认 chunk size
默认 compression level
默认 metadata batch size
```

这些属于实现和 benchmark 参数。

本文冻结的是：

> **RoDAQ Canonical Descriptor / Record / Payload 如何在不丢失语义的前提下投影为一个尽可能保持 MCAP 原生生态可用性的 Artifact。**

---

## 1.2 为什么不能只使用 MCAP Message Header

MCAP Message 原生字段只有：

```text
channel_id     uint16
sequence       uint32
log_time       uint64
publish_time   uint64
data            bytes
```

而 RoDAQ Canonical Record 至少还需要完整保存：

```text
StreamDescriptor identity
AcceptanceSequence uint64
optional SourceId opaque bytes
optional SourceSequence uint64
1..4 TimestampState
exact Payload bytes
```

因此不能执行：

```text
AcceptanceSequence uint64
    → MCAP sequence uint32
```

然后截断。

也不能把：

```text
多个 Role + ClockId timestamp
```

压缩成 `log_time / publish_time` 两个字段后声称 full-fidelity。

但另一个极端同样不合适：

```text
RoDAQ metadata + native Payload
    → one custom RecordEnvelope
    → MCAP Message.data
```

虽然这种方式最容易 round-trip，却会让原本标准的 ROS2 CDR / DDS CDR Payload 不再位于 MCAP Message.data 的原生位置，从而破坏 MCAP 的主要生态价值。

V1 最终采用：

> **native Payload 保留在正常 MCAP Message.data；RoDAQ-specific Canonical metadata 通过 bounded Attachment sidecar 保存。**

这是 full-fidelity 与现实工具兼容性之间更合适的边界。

---

## 1.3 参考实现

本设计重点参考：

```text
ros2/rosbag2_storage_mcap
https://github.com/ros2/rosbag2/tree/rolling/rosbag2_storage_mcap

eProsima DDS Record & Replay
https://github.com/eProsima/DDS-Record-Replay

Foxglove MCAP / mcap-cpp
https://github.com/foxglove/mcap
```

### rosbag2_storage_mcap

rosbag2 的成熟结构是：

```text
ROS Topic
    → MCAP Channel

ROS message definition
    → MCAP Schema

serialized ROS message
    → MCAP Message.data

receive timestamp
    → logTime

publish timestamp
    → publishTime
```

并已经工程化支持：

```text
Chunk
Message Index
Summary
CRC
LZ4
Zstd
fastwrite / zstd_fast / zstd_small preset
```

对 RoDAQ 最重要的结论不是复制 rosbag2 data model，而是：

> **当 source-native Payload 本来就是 MCAP registry 已知的 serialization 时，应尽量保持它原样进入 Message.data。**

这样 ROS2/Foxglove/MCAP 工具才有机会继续直接消费数据。

### DDS Record & Replay

eProsima DDS Record & Replay 同样把 DDS serialized Payload 作为正常 MCAP Message 写入。

它还使用：

```text
Attachment
Metadata
Channel metadata
```

保存 DynamicTypes、Writer GUID/partition 等旁路信息。

这提供了一个非常重要的现实证据：

> **native Message 与 recorder-specific metadata 不必强行合并为一种 Payload。**

RoDAQ Record metadata 比 DDS Record & Replay 更多，因此不能直接复用其字符串字典格式，但采用 Attachment 作为 sidecar 的方向是成熟可行的。

### MCAP Format / mcap-cpp

MCAP 本身支持：

```text
heterogeneous Channel
arbitrary message_encoding
Schema
Chunk
Attachment
Metadata
Summary / Index
```

并且 Attachment：

- 不属于 Topic Message；
- 可以保存任意 bytes；
- 有独立 index；
- data size 是 uint64；
- 适合低频 descriptor 和批量 side metadata。

因此 V1 不需要使用 private opcode，也不需要修改 mcap-cpp。

---

## 1.4 V1 Artifact 基本原则

V1 使用：

```text
MCAP Header.profile = "rodaq/v1"
```

`rodaq/v1` 是 RoDAQ profile，而不是 MCAP Registry 的 `ros2` profile。

原因是一个 V1 Artifact 将来可以同时包含：

```text
ROS2 CDR
Fast DDS CDR
future heterogeneous streams
```

不能把整个文件永久声明为纯 ROS2 profile。

但 data Channel 应尽可能使用 MCAP Registry 的 well-known：

```text
message_encoding
schema encoding
```

例如：

```text
ROS2 CDR       → message_encoding = cdr
ROS2 .msg      → schema encoding = ros2msg
ROS2 .idl      → schema encoding = ros2idl
OMG IDL        → schema encoding = omgidl
```

因此：

```text
RoDAQ profile
```

不等于：

```text
所有 Channel 都是 RoDAQ-private encoding
```

---

## 1.5 MCAP Native Facts 与 V1 约束

V1 依赖以下 MCAP 事实：

```text
Schema.id       uint16, non-zero
Channel.id      uint16
Message.sequence uint32
Message timestamp uint64 nanoseconds
Schema.data length uint32
Attachment.data length uint64
```

因此 MCAP Plugin 必须显式声明 backend capability：

```text
max projected Schema count
max projected Channel count
max native MCAP Schema representation size
```

不能让 `uint16` Channel/Schema ID overflow 在第一条 Record 写入时才被发现。

MCAP 的原生字段宽度：

> 只限制 MCAP projection，不得反向缩窄 Canonical Profile 的 uint64 domain。

---

## 1.6 Stable Plugin Lifecycle / Service Mapping

MCAP Plugin 通过 V1 Storage Family Protocol 接入 Runtime。

```text
create
    → 构造 Plugin-local state
    → 不创建文件、不启动 writer

bind
    → require rodaq.runtime.common.v1
    → require rodaq.runtime.canonical.v1
    → require rodaq.runtime.storage.v1
    → require rodaq.storage.control.v1

configure
    → 通过 Runtime-owned ConfigView 读取 chunking、compression、batch、target policy 等配置
    → 不保存 ConfigView 到调用返回之后

open
    → 创建 checked sink
    → 创建/初始化 McapWriter
    → 建立 Incomplete Artifact / metadata state
    → 建立 bounded batch state

submit
    → 对应 §3 Record Handoff

flush
    → 完成当前 flush-set persistence batch/barrier

close(close_request)
    → NORMAL: 完成 final batch、允许在全部 finalization checks 成功后发布 NormalComplete
    → ACTIVATION_ABORT: 只做安全 cleanup / incomplete finalization，禁止 NormalComplete
    → FAILURE: failure cleanup，禁止把失败 Session 伪装为 NormalComplete
    → return 后 no future payload/persistence callback

destroy
    → 只在未 Open 或 Closed/quiesced state 调用
```

MCAP / mcap-cpp C++ 类型：

> 只存在于 Plugin implementation 内，不进入 `include/rodaq/abi/*` 或 Runtime Public API。

---

# 2. Artifact、Descriptor 与 Native Channel Projection

## 2.1 Artifact 结构

一个 NormalComplete RoDAQ V1 MCAP Artifact 逻辑结构为：

```text
MCAP Magic
MCAP Header(profile="rodaq/v1")

RoDAQ file metadata

zero or more DescriptorBatch Attachments

zero or more native MCAP Schemas
zero or more native MCAP Channels

native Message records
    Message.data = exact Canonical Payload

zero or more RecordMetadataBatch Attachments

RoDAQ Finalization Manifest Metadata

Data End
Summary / indexes as configured
Footer
trailing MCAP Magic
```

其中：

```text
native Message
    → 保存业务 Payload

DescriptorBatch Attachment
    → 保存 Canonical Descriptor Graph

RecordMetadataBatch Attachment
    → 保存每条 Record 的 Canonical metadata
```

三者职责分离。

---

## 2.2 File Metadata

Prepare 阶段写一个低频 `mcap::Metadata`：

```text
name = "rodaq"
```

至少包含：

```text
format_version       = "1"
canonical_model      = "v0.9"
canonical_profile    = "v0.5"
storage_plugin       = "mcap"
storage_contract     = "v0.3"
mapping_version      = "v0.4"
```

这些 string metadata：

> 用于 inspection / version discovery，不是 Canonical State authority。

Canonical Descriptor 和 Record 仍由 binary sidecar 恢复。

---

## 2.3 DescriptorBatch Attachment

### 2.3.1 目的

Canonical Descriptor Graph 包含：

```text
SchemaDescriptor
ClockDescriptor
TimestampKeySet
StreamDescriptor
```

MCAP 原生 Schema / Channel 只能表达其中一部分。

因此 RoDAQ 使用 DescriptorBatch Attachment 保存完整 Canonical graph。

Attachment：

```text
name
    = "rodaq/descriptors/<revision>"

media_type
    = "application/octet-stream"

log_time    = 0
create_time = 0
```

`revision` 是 Storage-local monotonic revision，不是 Canonical ID。

Attachment 自带的 `log_time/create_time` 不是 RoDAQ Canonical timestamp authority。V1 固定为 0，避免制造一个不存在的 descriptor event wall-clock semantics。

### 2.3.2 Binary Header

V1 DescriptorBatch 使用 little-endian：

```text
8 bytes magic = "RDAQDS01"
u16 version = 1
u16 flags = 0
u32 reserved = 0
u64 revision
u64 event_count
```

所有 variable-length bytes：

```text
u64 length
N bytes
```

这样 sidecar 不额外引入 `uint32` semantic length narrowing。

### 2.3.3 Descriptor Event

每个 event：

```text
u8  kind
u8  reserved[7]
u64 body_size
body
```

kind：

```text
1 = SchemaDescriptor
2 = ClockDescriptor
3 = TimestampKeySet
4 = StreamDescriptor
```

#### SchemaDescriptor

```text
u64 SchemaId
bytes SchemaFormat
u8 TypeIdentifierPresent
if present:
    bytes TypeIdentifier
bytes exact representation
```

#### ClockDescriptor

```text
u64 ClockId
u64 resolution_numerator
u64 resolution_denominator
bytes ClockOrigin
```

#### TimestampKeySet

```text
u64 TimestampKeySetId
u8  key_count
for each Runtime compiled slot:
    bytes RoleIdentifier
    u64 ClockId
```

Canonical TKS 是 unordered set。

Attachment 内 slot order 只是 Storage projection；decode 后必须恢复同一个 Canonical set。

#### StreamDescriptor

```text
u64 StreamDescriptorId
u64 StreamId
bytes StreamName
u8 SchemaIdPresent
if present:
    u64 SchemaId
bytes PayloadEncoding
u64 TimestampKeySetId
```

### 2.3.4 Descriptor-before-Record

一个新 StreamDescriptor 变为 Storage Ready 前必须完成：

```text
all newly required Canonical descriptors
        ↓
DescriptorBatch serialized into bounded buffer
        ↓
DescriptorBatch Attachment write accepted
        ↓
checked sink has no known error
        ↓
native MCAP Schema projection established when possible
        ↓
native MCAP Channel established
        ↓
Descriptor Ready
```

之后 Runtime 才允许相关 Binding Active。

因此第一条 Record 永远不会成为发现 descriptor capacity 不足的正常路径。

---

## 2.4 Native MCAP Schema Projection

Canonical SchemaDescriptor 是 authority。

MCAP Schema 是：

> **为了 native tooling interoperability 建立的 projection。**

如果 Canonical Stream 没有 Schema：

```text
MCAP Channel.schema_id = 0
```

如果存在 SchemaDescriptor，并且 representation size 在 MCAP `uint32` schema-data limit 与 Storage capability 内，则建立 MCAP Schema。

推荐映射：

| Canonical SchemaFormat | MCAP schema.encoding |
|---|---|
| `ros2msg` | `ros2msg` |
| `ros2idl` | `ros2idl` |
| `omg/idl` | `omgidl` |
| 已知 MCAP Registry encoding | 对应 well-known encoding |
| 其它 exact format | 使用明确 custom encoding 或仅 sidecar，取决于 backend config |

Schema name：

```text
TypeIdentifier present
    → TypeIdentifier

TypeIdentifier absent
    → "rodaq/schema/<SchemaId>"
```

`Schema.data` 保存 exact representation bytes。

如果某个 Canonical Schema 无法合法投影成 MCAP Schema：

> 不影响 Canonical full-fidelity sidecar；Channel 可以 `schema_id = 0`，但 third-party native decode capability 降级。

如果配置要求：

```text
require_native_schema_projection = true
```

则这种情况在 Descriptor readiness 阶段直接 Unsupported。

Initial ROS2 implementation 推荐启用该要求，因为 ROS2 message definition 正常应可获得。

---

## 2.5 Native MCAP Channel Projection

每个 StreamDescriptor 建立一个独立 MCAP Channel。

原因：

```text
StreamDescriptor immutable
```

所以 Descriptor evolution：

```text
same StreamId
StreamDescriptor A → Channel A
StreamDescriptor B → Channel B
```

不能复用已写入的 Channel 后修改其 interpretation。

Channel：

```text
topic
    = StreamName

schema_id
    = projected SchemaId or 0

message_encoding
    = projected PayloadEncoding
```

### PayloadEncoding projection

推荐：

```text
Canonical ros2/rmw/cdr
    → MCAP cdr

Canonical DDS encoding
that is exact RTPS SerializedPayload framing
    → MCAP cdr

known registry encoding
    → corresponding registry string

unknown/custom exact encoding
    → explicit custom message_encoding string
```

不得仅因为 bytes “看起来像 CDR” 就声明：

```text
message_encoding = cdr
```

只有 Payload 实际符合 MCAP registry 对 `cdr` 的 framing contract 才可以这样映射。

Channel metadata 可以增加：

```text
rodaq.stream_id
rodaq.stream_descriptor_id
rodaq.payload_encoding
rodaq.schema_id
```

建议以固定格式 hex / string 保存，便于 inspection。

但：

> **Channel metadata 不是 Canonical round-trip authority。**

真正 full-fidelity 仍依赖 DescriptorBatch。

---

## 2.6 Native ID Capacity

MCAP Channel ID 与 Schema ID 都是 `uint16`，并且 `schema_id = 0` 具有“无 schema”的特殊含义。

因此 Storage capability 必须在 Prepare 时分别声明：

```text
max_projected_stream_descriptors
max_projected_schemas
```

并保证：

```text
new Descriptor / Schema projection
```

只有在对应 native ID capacity 可用时才能 Ready。

不允许：

```text
Channel / Schema ID exhausted
    ↓
reuse old native ID with new semantics
```

也不允许 silently wrap。

---

## 2.7 V1 Plugin 内部状态

一个克制的 MCAP Plugin 对象即可维护：

```text
mcap::McapWriter
checked sink
Canonical Descriptor projection table
StreamDescriptorId → ChannelId
SchemaId → native MCAP SchemaId
next StorageRecordOrdinal
current RecordMetadataBatch
pending persistence batch state
artifact counters / finalization state
```

不要求：

```text
SchemaManager
ChannelManager
MetadataManager
ChunkManager
PersistenceManager
```

等独立层级。

逻辑状态清晰比 class 数量对称更重要。

---

# 3. Record、Metadata Sidecar 与 Persistence

## 3.1 Native Message.data

V1 的关键决定：

> **MCAP Message.data 必须保存 exact Canonical Payload bytes。**

即：

```text
RuntimeRecord.Payload
        ↓
MCAP Message.data
```

不加 RecordEnvelope header，不反序列化，不重编码。

这样对首个 ROS2 functional implementation：

```text
ROS2 CDR Payload
    → MCAP CDR Message.data
```

可以尽量保持 rosbag2/Foxglove/普通 MCAP 工具可用。

DDS deferred implementation 也沿用同一原则。

---

## 3.2 StorageRecordOrdinal

MCAP Storage 为每个正常 data Message 分配一个：

```text
StorageRecordOrdinal : uint64
```

规则：

```text
first data Message = 0
then +1
never reused inside one Artifact
```

它只是：

> **Storage-local correlation key。**

不是 Canonical ID，不进入 Runtime，不代替 AcceptanceSequence。

MCAP Message.sequence 在 V1 固定为：

```text
0
```

原因：

- Canonical `AcceptanceSequence` 是完整 `uint64`，已经由 RecordMetadataBatch 保存；
- `StorageRecordOrdinal` 是 Artifact-global correlation key，不是 MCAP Channel-local source sequence；
- 把它截成 `uint32` 会在长期运行后 wrap，并可能违反非零 MCAP sequence “per-channel increasing” 的正常预期；
- 使用 0 明确表示该 native field 不承担 RoDAQ Canonical ordering / identity 语义。

RoDAQ decoder 完全不依赖 `Message.sequence`。

---

## 3.3 MCAP log_time / publish_time

`log_time / publish_time` 只作为 MCAP index / interoperability projection。

Canonical timestamp authority 在 RecordMetadataBatch 内。

### V1 default

默认：

```text
log_time
    = Storage Handoff observed system wall time in uint64 ns

publish_time
    = log_time
```

这样普通 MCAP 工具得到一个实用的 wall-time timeline，同时不错误宣称 Runtime monotonic ingress Clock 与 Unix epoch 等价。

这两个值：

> 不是 Canonical timestamp，也不用于 full-fidelity round-trip。

### Optional timestamp projection

未来/配置可以选择一个明确可转换为：

```text
Unix epoch nanoseconds
```

的 Canonical TimestampKey 投影到 log/publish time。

但必须同时证明：

```text
ClockOrigin semantics known
resolution exact-convertible
reading in uint64 ns range
```

否则回退 default wall-time projection。

不能为了 MCAP timestamp 字段而改变 Canonical Clock semantics。

---

## 3.4 RecordMetadataBatch Attachment

### 3.4.1 目的

以下 per-Record Canonical data 不能完全放入 MCAP Message header：

```text
StreamDescriptorId
AcceptanceSequence uint64
SourceId
SourceSequence uint64
TimestampStateMap
```

因此 MCAP Plugin 将这些字段以 compact binary batch 写入 Attachment。

Attachment：

```text
name
    = "rodaq/records/<batch_id>"

media_type
    = "application/octet-stream"

log_time    = 0
create_time = 0
```

这里同样不使用 Attachment timestamp 承载 Record 时间语义；所有 Canonical timestamp state 只来自 RecordMetadataEntry + DescriptorBatch 中的 TimestampKeySet / ClockDescriptor。

### 3.4.2 Batch Header

little-endian：

```text
8 bytes magic = "RDAQRM01"
u16 version = 1
u16 flags = 0
u32 reserved = 0
u64 batch_id
u64 first_storage_record_ordinal
u64 record_count
```

一个 batch 对应：

```text
[first_ordinal,
 first_ordinal + record_count)
```

连续的 data Message ordinal。

### 3.4.3 RecordMetadataEntry

每个 entry 顺序对应一个 StorageRecordOrdinal，不重复保存 ordinal。

```text
u16 projected_channel_id
u8  flags
u8  timestamp_present_mask
u32 reserved = 0

u64 StreamDescriptorId
u64 AcceptanceSequence

if flags.bit0:
    u64 SourceIdLength
    N   SourceId exact bytes

if flags.bit1:
    u64 SourceSequence

for each set timestamp bit in referenced TKS slot order:
    i64 ClockReading

u64 PayloadSize
```

flags：

```text
bit 0 = SourceId present
bit 1 = SourceSequence present
bits 2..7 = 0
```

`PayloadSize` 用于 decoder/validator 检测 sidecar 与 data Message 错配。

V1 默认不计算 per-Payload hash，避免在高带宽相机/点云场景增加重复 CPU 成本。

如果后续 fault model 证明需要，可以增加 versioned optional checksum flag，而不是 V1 强制。

---

## 3.5 Metadata Batch Correlation

RoDAQ decoder 按 MCAP：

```text
FileOrder
```

遍历所有正常 data Message，并从 0 开始重建 StorageRecordOrdinal。

同时读取：

```text
rodaq/records/* Attachments
```

覆盖同一 ordinal 空间。

必须满足：

```text
one data Message
↔ exactly one RecordMetadataEntry
```

并检查：

```text
message.sequence == 0
entry.projected_channel_id == message.channel_id
entry.PayloadSize == message.dataSize
StreamDescriptorId ↔ Channel projection consistent
```

如果出现：

```text
missing metadata entry
extra metadata entry
overlap batch
gap batch
channel mismatch
payload-size mismatch
```

则 Artifact 不是 conforming NormalComplete RoDAQ Artifact。

这种 correlation 不依赖 `Message.sequence`，所以不会被 uint32 wrap 破坏。

---

## 3.6 Handoff 流程

Handoff 前 Runtime 已经保证：

```text
RuntimeRecord immutable
Payload lifetime valid
same-Stream Storage handoff order == AcceptanceSequence order
```

MCAP Plugin 的一个 Record Handoff：

```text
1. reserve bounded RecordMetadataEntry capacity
2. build metadata entry in Storage-owned memory
3. read current next StorageRecordOrdinal as candidate_ordinal
   but do not advance it yet
4. resolve already-ready ChannelId
5. build mcap::Message header with sequence = 0
6. Message.data points to call-scoped Runtime PayloadView
7. McapWriter::write(Message)
8. inspect write Status + checked-sink error latch
9. on success only:
       commit metadata entry using candidate_ordinal
       advance next StorageRecordOrdinal exactly once
10. HandoffAccepted
11. PayloadConsumed
12. successful Handoff return
```

关键点：

> **mcap-cpp `write(Message)` 必须在 call 内完成对 Message.data 的同步消费。**

启用 Chunk 时，mcap-cpp 会把 Message record bytes 写入自身 Chunk buffer；关闭 Chunk 时已经不再需要原 Payload pointer。

no-chunk 模式则在该 call 内写入 sink。

因此首版 MCAP Plugin 可以声明：

```text
successful HandoffAccepted return
    ⇒ PayloadConsumed
```

不需要让 Runtime Payload 跨 Handoff return 存活。

但该结论依赖所选 `mcap-cpp` 实现确实在 `write(Message)` return 前完成 caller `Message.data` 的同步消费。

因此：

> **mcap-cpp version upgrade 必须重新运行 §5.6 Payload Lifetime Tests；测试失败时不得继续使用 synchronous PayloadConsumed capability。**

如果 `write()` 返回失败，或 checked sink 已 latch I/O error：

```text
HandoffAccepted 不成立
metadata entry 不提交
next StorageRecordOrdinal 不前进
return FatalFailure
```

因此 StorageRecordOrdinal 对实际写入成功的 data Message 始终连续；失败的 pre-Handoff attempt 不制造 sidecar ordinal gap。

return 后不得保留 Runtime Payload access。

---

### 3.6.1 Stable Storage ABI Mapping

MCAP Handoff 通过 `rodaq_storage_plugin::submit()` 表达。

`rodaq_storage_submission` 必须完整消费当前 Stable ABI 的 Canonical Record 投影：

```text
AcceptanceSequence
optional SourceHandle / SourceId
optional SourceSequence
complete TimestampStateMap
Payload
```

低频 Canonical Descriptor Graph 通过 `rodaq.runtime.canonical.v1` 查询，包括：

```text
StreamId / StreamDescriptorId
optional SchemaId
SchemaFormat / optional TypeIdentifier / representation
TimestampKeySetId / TimestampKey content
ClockId / exact reduced rational resolution / ClockOrigin
```

MCAP sidecar/metadata projection必须保存这些 Canonical value，而不是 Runtime Handle 或 transport slot index。

首个 V1 实现推荐固定为：

```text
submit() function return == OK
    ↔ HandoffAccepted

out_result.payload_state
    = RODAQ_STORAGE_PAYLOAD_CONSUMED

out_result.persistence_state
    = RODAQ_STORAGE_PERSISTENCE_PENDING
```

原因是当前设计已经要求并通过 §5.6 Payload Lifetime Tests 验证：

> `mcap-cpp::McapWriter::write(Message)` 在 return 前完成对 caller `Message.data` 的读取/复制，不在 return 后保留 Runtime Payload pointer。

因此首个 MCAP Plugin：

```text
不发送 payload_consumed() callback
```

而是在 `submit()` result 中同步返回 `PAYLOAD_CONSUMED`。

Persistence 不能同步报告成功，因为：

```text
Message data
+
RecordMetadataBatch Attachment
+
checked-sink batch barrier
```

尚未共同完成。

所以 Record 保持：

```text
PayloadConsumed
+
PersistencePending
```

直到 §3.8 batch commit。

Batch commit 成功后，对每个仍 Pending 的 operation exactly-once 调用：

```text
rodaq.runtime.storage.v1::persistence_completed(
    operation,
    RODAQ_STORAGE_PERSISTENCE_SUCCEEDED,
    RODAQ_STATUS_OK,
    empty_message)
```

失败时根据可证明结果报告：

```text
PERSISTENCE_FAILED
or
PERSISTENCE_UNKNOWN
```

并进入 generic Storage fatal-failure path。

如果 `McapWriter::write()`、checked sink 或 metadata preparation 在 HandoffAccepted 前失败：

```text
submit() returns non-OK
no payload callback
no persistence callback
no retained Runtime Payload access after return
```

如果未来 mcap-cpp 版本不再满足 synchronous Payload consumption：

> 不得继续返回 `PAYLOAD_CONSUMED`；必须改成 `PAYLOAD_PENDING` 并通过 `payload_consumed()` exactly-once 终止 Payload Access Lease。

这也是 mcap-cpp 版本升级必须重跑 Payload Lifetime Test 的原因。

---

## 3.7 Metadata Batch Resource Bound

current RecordMetadataBatch 必须同时受：

```text
max_records_per_metadata_batch
max_bytes_per_metadata_batch
max_metadata_batch_age
```

约束。

达到任一条件就安排 batch commit。

这样限制：

```text
PersistencePending latency
sidecar memory
failure blast radius
```

不能只依赖 MCAP chunkSize 作为 metadata budget。

还必须考虑 MCAP format 的一个现实约束：

> **Attachment 不能位于 Chunk 内。**

因此每次 RecordMetadataBatch commit 在 chunking 模式下都会形成一个 chunk boundary：

```text
Message batch in Chunk
        ↓
close chunk
        ↓
RecordMetadataBatch Attachment
        ↓
next Chunk
```

所以 metadata batch 不能设置得过小，否则会导致：

```text
excessive small Chunks
more Chunk/Message indexes
compression-ratio degradation
additional sink calls
```

V1 仍以 correctness / bounded latency 为首要约束，但 implementation benchmark 必须联合选择：

```text
metadata batch thresholds
chunk target size
max persistence latency
```

而不是分别独立调参。

---

## 3.8 Persistence Batch Commit

一个 metadata batch 中的 Record 只有在：

```text
对应 native Message bytes
+
对应 RecordMetadataBatch Attachment
```

都成功进入 checked sink 后，才能 `PersistenceSucceeded`。

推荐 commit：

```text
current data Messages
        ↓
if chunking:
    McapWriter::closeLastChunk()
        ↓
check checked-sink error latch
        ↓
McapWriter::write(RecordMetadataBatch Attachment)
        ↓
check Attachment Status
check checked-sink error latch
        ↓
checked-sink batch barrier
        ↓
check barrier result / error latch
        ↓
PersistenceSucceeded for batch records
```

MCAP Attachment 不能存在 Chunk 内。

mcap-cpp 写 Attachment 时会先关闭当前 Chunk，因此显式 `closeLastChunk()` 的作用是：

> 让 RoDAQ 的 persistence barrier 更清楚，也便于在 Attachment write 前先检查 sink failure。

如果配置：

```text
noChunking = true
```

Message 已经直接写入 Data section，仍要等 metadata Attachment 和 checked-sink batch barrier 都成功后再报告 batch PersistenceSucceeded。

---

## 3.9 为什么不在 Handoff 时立刻 PersistenceSucceeded

即使：

```text
McapWriter::write(Message) == Success
```

也不能自动认为 Canonical Record 已持久化。

原因：

```text
chunking:
    Message may only be in memory Chunk

full fidelity:
    RecordMetadataEntry may still only be in memory batch
```

因此：

```text
HandoffAccepted
PayloadConsumed
```

可以很早发生，而：

```text
PersistenceSucceeded
```

必须等 batch commit。

这正好验证 generic Storage Contract 将这些 completion 分离是必要的。

---

## 3.10 Ordering

Runtime 已建立：

```text
same Stream handoff order
== AcceptanceSequence order
```

MCAP Storage 使用一个 logical writer lane：

```text
handoff order
    → Message FileOrder
    → StorageRecordOrdinal order
    → metadata entry order
```

因此同一 Stream 的 file order 继续保持 AcceptanceSequence order。

不同 Stream 不定义 Canonical total order。

StorageRecordOrdinal 只记录实际 Storage writer order，不能反向变成 GlobalSequence。

---

## 3.11 Descriptor Evolution

新 StreamDescriptor：

```text
new DescriptorBatch revision
        ↓
new native Schema if needed
        ↓
new Channel
        ↓
Descriptor Ready
        ↓
new Record uses new ChannelId
```

旧 RecordMetadataEntry 继续引用旧 StreamDescriptorId。

所以同一个 Stream rename/schema/encoding/TKS evolution 可以在 Artifact 内准确重建。

---

# 4. Writer 配置、Failure、Flush 与 Close

## 4.1 mcap-cpp API Reality

首个实现必须以目标 Humble `mcap_vendor` 中实际 vendored 的 mcap-cpp 0.8.0 为 correctness baseline。

该版本关键 API：

```text
McapWriter::write(Message)    → Status
McapWriter::write(Attachment) → Status
McapWriter::write(Metadata)   → Status
McapWriter::closeLastChunk()  → void
McapWriter::close()           → void
McapWriter::terminate()       → void

IWritable::handleWrite(...)   → void
IWritable::end()              → void
IWritable::size()             → uint64
```

目标 0.8.0 `IWritable` 没有可以由 `McapWriter` 调用并返回状态的 generic flush/barrier operation。后续 mcap-cpp 版本可能增加 `flush()` 等能力，但 V1 Mapping：

> **不得依赖某个较新版本才存在的 flush/error propagation API 来证明 Humble baseline 的 PersistenceSucceeded。**

同样不得写出并不存在的：

```text
McapWriter::close() Status
McapWriter::closeLastChunk() Status
```

作为 correctness proof。

因此所有 sink-level write / buffered flush / underlying close 结果必须由 RoDAQ 自己持有的 concrete checked sink 额外暴露和检查。

---

## 4.2 Checked Sink

首个实现必须提供 Plugin-owned checked sink，而不是直接使用 `McapWriter::open(filename, ...)` 后只相信 writer `Status`。

目标 0.8.0 的 `IWritable` write/end callback 都返回 `void`，所以 checked sink 必须在自己的 concrete API 上维护并暴露：

```text
first_error
bytes_written / current_offset
barrier() result
end/close result
```

建议关系：

```text
McapWriter
    ↓ IWritable calls
CheckedSink
    ├── handleWrite()
    ├── end()
    ├── size()
    ├── barrier()       // RoDAQ-owned concrete API
    └── close_result()  // RoDAQ-owned concrete API/state
```

`barrier()` 不属于 mcap-cpp 0.8.0 `IWritable`；Runtime/Storage Plugin 持有 concrete CheckedSink reference，在一个 persistence batch 的 Message/Attachment bytes 已经交给 writer 后显式调用。

一个 successful batch barrier 必须证明：

> 此前交给 sink 的 bytes 已越过所有可能隐藏 application/user-space I/O error 的 buffering 层，并被底层 OS/backend 接受。

这可以通过：

```text
checked fd/write path with no opaque stdio buffering

or
Plugin-owned FILE*/stream + explicit checked fflush/equivalent barrier
```

实现。

它不要求 `fsync`，所以仍不等于 power-loss durability。

如果 `handleWrite()` 因 API 形态无法直接返回 error，checked sink 可以：

```text
latch first_error
+
optionally throw a backend exception caught at Plugin boundary
```

但无论采用哪种实现：

> `McapWriter::write(...) == Success` 本身都不足以覆盖 sink-level failure；每个 responsibility / persistence boundary 必须同时检查 checked-sink state。

一旦 `first_error` latch：

```text
future normal write
    → fatal Storage failure
```

不得继续把后续 Record 报成 PersistenceSucceeded。

### Exception Boundary

不同 mcap-cpp 版本 / concrete sink 可能通过 C++ exception 报告底层 I/O failure；RoDAQ 自己的 checked sink 也可以选择在 latch error 的同时抛出受控 backend exception。

Humble baseline 不依赖默认 `FileWriter` 是否、以及以何种方式传播 short-write error。

RoDAQ Storage Plugin boundary 必须：

```text
catch all backend exceptions
        ↓
convert to checked Storage failure state
        ↓
latch first_error / failure disposition
        ↓
return / report Storage Contract failure
```

不得让 exception 穿过 Runtime generic Storage boundary。

如果 exception 发生在某个 Record 尚未 `HandoffAccepted` 的 synchronous Handoff 内：

- 该 Record 不取得 Storage responsibility；
- return/unwind 后 Storage 不得保留 Runtime PayloadView；
- Artifact 整体进入 fatal failure path。

如果 exception 发生在已接受 batch 的 chunk/Attachment/Flush/Close path：

> 依据 checked sink progress 与 batch boundary，把相关 Record 收敛为 `PersistenceFailed` 或 `PersistenceUnknown`，不能猜测成功。

### per-message write

检查：

```text
McapWriter::write(Message) Status
+
checked sink error latch
```

### Chunk boundary

```text
McapWriter::closeLastChunk()
+
checked sink error latch
+
CheckedSink::barrier() when this boundary is used as persistence commit
```

### Attachment

```text
McapWriter::write(Attachment) Status
+
checked sink error latch
```

### Close

```text
McapWriter::close()
+
checked sink end/close result
+
any required final CheckedSink::barrier() result
```

writer close 返回 `void`，所以真实 I/O failure proof 来自 checked sink 的 latched error、barrier 与 end/close result。

---

## 4.3 Writer Options

V1 不重复发明 MCAP writer vocabulary。

配置尽量对应 mcap-cpp / rosbag2_storage_mcap：

```text
noChunking
chunkSize
compression
compressionLevel
forceCompression
noMessageIndex
CRC options
summary / index options
```

另外 RoDAQ 自己增加：

```text
metadata_batch_max_records
metadata_batch_max_bytes
metadata_batch_max_age

max_payload_size
max_queued_records
max_queued_bytes
max_projected_channels
max_projected_schemas

handoff_timeout
flush_timeout
close_timeout
```

MCAP format tuning 与 generic Storage resource/progress limit 分开。

---

## 4.4 Initial Implementation 默认策略

首个 correctness baseline 必须先在 Humble vendored mcap-cpp 0.8.0 上通过 §5.6/§5.7 executable tests，并额外完成 **long-run writer-memory qualification**。

V1 production baseline 不允许默认开启任何会让 writer-resident memory 随 Record/Chunk 数无界增长的 index/summary feature。

初始推荐：

```text
chunking           = enabled
compression        = None
message index      = disabled by default
summary/index      = only enable components proven bounded for the configured artifact lifetime
chunk/data CRC     = enabled when measured cost acceptable
metadata sidecar   = bounded uncompressed binary Attachment batches
```

原因：

- 先减少 compression 变量；
- boundedness 优先于随机访问 convenience；
- 避免 mcap-cpp 内部 message/chunk/index bookkeeping 随长 Session 线性增长；
- 先测真实 Payload/metadata/index overhead；
- 方便 failure injection。

如果业务确实需要某个会 O(N) 累积的 index，而 mcap-cpp 当前实现无法 boundedly flush/reclaim：

```text
either
    disable that index
or
    introduce an explicitly bounded artifact-rolling policy
```

不得把“close 时最终会释放”视为 Runtime bounded-memory proof。

随后性能测试再决定：

```text
fastwrite-like
zstd_fast-like
zstd_small-like
```

preset。

不能在设计阶段拍一个“机器人一定最优”的压缩参数。

---

## 4.5 Memory Bound

MCAP Storage memory 至少包括：

```text
Runtime-dispatch accepted work
current mcap Chunk buffer
current RecordMetadataBatch
compression workspace
DescriptorBatch temporary buffer
pending completion state
```

因此：

```text
chunkSize
```

不是 Storage 总内存上限。

实现必须把这些预算合并进 Storage capability / admission planning。

特别是：

> metadata sidecar 不能因为“每条很小”就无限累计到 Close 才写。

---

## 4.6 Fatal Failure Classification

fatal writer/sink failure 后：

```text
Runtime closes Admission Gate
new normal Handoff stops
```

Record outcome：

```text
never HandoffAccepted
    → Runtime classifies PersistenceFailed

HandoffAccepted
+ metadata batch definitely not committed
    → PersistenceFailed
    if non-persistence can be proven

HandoffAccepted
+ sink/backend outcome uncertain
    → PersistenceUnknown

already completed metadata batch
    → PersistenceSucceeded remains Succeeded
```

即使 raw native Message bytes 已经部分出现在损坏文件里：

> 只要 Canonical metadata sidecar 未完成，不能把该 Record 当作 full-fidelity persistence success。

Admission Accepted history 永远不重写。

---

## 4.7 Flush

Final Flush 必须：

```text
stop accepting new Handoff at Runtime barrier
        ↓
commit current RecordMetadataBatch
        ↓
if open Chunk remains:
    closeLastChunk()
        ↓
check checked sink
        ↓
flush checked sink
        ↓
resolve all required persistence completions
```

如果 current metadata batch 是空的，也仍要关闭可能存在的最后 Chunk。

Flush failure / timeout：

```text
terminal disposition = Failure
```

Flush success：

> 不自动等于 NormalComplete。

---

## 4.8 Finalization Manifest

所有 Record persistence completion 已收敛后，写：

```text
mcap::Metadata
name = "rodaq.finalization"
```

至少包含：

```text
record_count
last_storage_record_ordinal
last_descriptor_revision
metadata_batch_count
canonical_model
canonical_profile
storage_contract
mapping_version
```

其中本文件当前值分别为：

```text
storage_contract = v0.3
mapping_version  = v0.4
```

该 Manifest 是：

> **NormalComplete validation aid。**

它不是单独的 completion marker。

因为 Manifest 写完之后仍然可能：

```text
Footer write failure
sink flush failure
file close failure
```

所以 offline validator 必须同时检查 MCAP 结构完整性。

---

## 4.9 Normal Close

Normal Close：

```text
all persistence batches terminal
        ↓
Finalization Manifest write success
        ↓
McapWriter::close()
        ↓
Data End / Summary / Footer / trailing magic written
        ↓
checked sink no error
        ↓
checked sink flush/end/underlying close success
        ↓
CloseSuccess
        ↓
Artifact NormalComplete
```

只有全部成立才返回 normal success。

`McapWriter::close()` 是 `void`；不能把调用完成本身当作成功证据。

---

## 4.10 Starting Activation Abort

Storage Prepare 可能已经创建：

```text
file
Header
initial Metadata
possibly descriptor projection
```

但 Runtime Activation Commit 尚未发生。

此时 abort：

```text
cancel/settle pending Descriptor work
        ↓
McapWriter::terminate()
        ↓
no Footer / normal finalization
        ↓
checked sink closes resource
        ↓
Artifact remains Incomplete / Aborted
```

不得调用 normal Close 后再把它标成一个合法的空 recording。

如果 `terminate()` 本身已经不安全可调用，则按 generic failure cleanup/quarantine 规则处理 native resource。

---

## 4.11 Crash / Power-loss Boundary

V1 `PersistenceSucceeded`：

> 表示 Storage backend write batch 已成功完成。

不自动等于：

```text
fsync completed
storage device non-volatile persistence guaranteed
```

MCAP Plugin 可以未来提供：

```text
durability_policy
```

例如 batch fsync / close fsync。

但当前没有足够 cross-backend evidence 把它提升成 generic Canonical semantics。

因此仍不需要单独新增 `persistence.md`。

---

# 5. Interoperability、Conformance 与设计审查结论

## 5.1 Full-fidelity Decode

RoDAQ MCAP decoder/validator 的逻辑流程：

```text
open MCAP
        ↓
validate Header.profile = rodaq/v1
        ↓
read RoDAQ file metadata
        ↓
read DescriptorBatch attachments by revision
        ↓
rebuild Canonical Descriptor Graph
        ↓
read RecordMetadataBatch attachments
        ↓
iterate normal MCAP Messages in FileOrder
        ↓
assign StorageRecordOrdinal 0..N-1
        ↓
pair each Message with exactly one metadata entry
        ↓
Payload = Message.data exact bytes
        ↓
reconstruct Canonical Record
        ↓
validate Finalization Manifest / Footer / counts
```

必须 field-wise 恢复 same Canonical State。

---

## 5.2 ROS2 Initial Implementation Interoperability

对于 ROS2 Stream，如果：

```text
SchemaFormat = ros2msg / ros2idl
PayloadEncoding can project to MCAP cdr
Schema representation available
```

则 native data Channel 可以投影为：

```text
topic            = ROS2 StreamName
schema encoding  = ros2msg / ros2idl
message_encoding = cdr
Message.data     = original ROS2 serialized bytes
```

RoDAQ-specific metadata只存在于 Attachment / Metadata，不额外占用用户 Topic Channel。

这可以显著提升：

```text
mcap CLI
Foxglove / generic MCAP readers
other readers that understand ros2msg/ros2idl + cdr
```

对 normal data Channel 的可读性。

但是必须严格区分：

```text
RoDAQ MCAP profile
!=
MCAP Registry ROS2 profile
```

官方 MCAP ROS2 profile 要求：

```text
Header.profile = ros2
Channel.message_encoding = cdr
Schema.encoding = ros2msg or ros2idl
Channel.metadata contains offered_qos_profiles
```

RoDAQ V1 Artifact 则固定：

```text
Header.profile = rodaq/v1
```

并且 generic Storage Contract 当前没有把 ROS2 offered QoS profile 作为 Canonical / Storage input。

因此 V1 正式保证是：

> **ROS2-compatible native data projection where possible，而不是严格 ROS2-profile MCAP conformance。**

不能把：

```text
ros2 bag info -s mcap
ros2 bag play -s mcap
```

作为 RoDAQ V1 normative correctness requirement。某个 rosbag2 版本可能接受 `rodaq/v1` profile 并忽略 RoDAQ Attachment，这属于 compatibility evidence，不属于格式保证。

如果未来需要正式 ROS2-profile export：

> 应设计显式 exporter / conversion path，补齐 ROS2 profile 要求的 QoS metadata，并写出 `Header.profile = ros2`。

这不应通过给 generic Storage Contract 增加 ROS2-specific metadata 来实现。

如果某 Stream 只有 `rodaq/ros2/type-only-v1` SchemaDescriptor：

> RoDAQ full-fidelity 仍可成立，但 native ROS2 schema decode capability 不成立；Channel 应使用 `schema_id = 0` 或明确 custom projection，不能伪装成 `ros2msg`。

---

## 5.3 DDS Interoperability（Fast DDS Baseline）

对于 Fast DDS：

```text
exact RTPS SerializedPayload framing
    → MCAP cdr

exact OMG IDL representation
    → MCAP omgidl
```

时，可以保持标准 MCAP Channel。

如果 Fast DDS Plugin 最终选择的 Canonical Payload 不符合 MCAP registry `cdr` framing：

> 必须使用明确 custom message_encoding，不能为了第三方工具兼容性错误标成 `cdr`。

Full-fidelity 的优先级高于伪兼容。

---

## 5.4 Common Canonical Corpus

MCAP 与 SQLite3 必须使用同一套 Canonical Corpus。

至少包括：

### Scalar

```text
Canonical ID = 0
Canonical ID near UINT64_MAX
AcceptanceSequence = 0
AcceptanceSequence near UINT64_MAX
intentional AcceptanceSequence gaps
```

### Source

```text
SourceId absent
binary SourceId containing NUL
SourceSequence absent
SourceSequence present(0)
SourceSequence near UINT64_MAX
```

### Timestamp

```text
1..4 TimestampKeys
Absent
Present(0)
negative ClockReading
INT64_MIN
INT64_MAX
```

### Schema

```text
schema absent
ROS2 schema
OMG IDL schema
binary/unknown exact schema format
```

### Payload

```text
empty Payload
binary Payload containing NUL / 0xff
large Payload near effective_payload_limit
```

### Evolution

```text
multiple Streams
same Stream + multiple StreamDescriptors
Clock restart → new Clock / TKS / StreamDescriptor
```

round-trip 后必须恢复 same Canonical graph/records。

---

## 5.5 Ordering / Sidecar Validation

必须验证：

```text
DescriptorBatch before first Record using descriptor

same-Stream FileOrder
== AcceptanceSequence order

StorageRecordOrdinal continuous over data Messages

RecordMetadataBatch ordinal ranges:
    no overlap
    no gap
    cover exactly all data Messages

ChannelId / StreamDescriptorId consistent
PayloadSize matches Message.dataSize
```

第三方工具重新排序、过滤或重写 MCAP 后：

> 可能不再是一个 conforming RoDAQ full-fidelity Artifact。

这是可接受的；普通 MCAP compatibility 不等于任意外部 rewrite 后仍保持 RoDAQ sidecar correlation。

RoDAQ validator 必须至少通过 ordinal range、ChannelId、PayloadSize 和 Descriptor projection 检测常见 reorder/filter/rewrite。`Message.sequence` 固定为 0，不参与 sidecar correlation。

V1 不宣称提供 cryptographic tamper detection：如果外部工具同时重写 Message 顺序、sequence、sidecar 关联字段，并恰好保持所有结构校验成立，则超出当前 Artifact integrity threat model。需要抗恶意篡改时应另行增加 checksum/hash/signature contract，而不是把它隐含在普通 MCAP replay compatibility 中。

还必须 benchmark / validate metadata batch 导致的 chunk-boundary 成本，至少记录：

```text
average messages per Chunk
Chunk count
Attachment count
index overhead
compression ratio when enabled
persistence completion latency
```

确保 bounded sidecar policy 不会因为过小 batch 退化成近似 one-message-per-chunk。

---

## 5.6 Payload Lifetime Tests

验证：

```text
Runtime PayloadView
    ↓
McapWriter::write(Message)
    ↓
successful return
    ↓
PayloadConsumed
    ↓
original Runtime buffer immediately reused/overwritten
    ↓
MCAP Message bytes remain exact
```

分别覆盖：

```text
chunking enabled
noChunking
small payload
large payload
empty payload
```

这直接验证目标 mcap-cpp writer 不在 `write(Message)` return 后继续读取 caller Payload pointer。

同时必须验证 Stable ABI result：

```text
submit() == OK
submit_result.payload_state == PAYLOAD_CONSUMED
submit_result.persistence_state == PERSISTENCE_PENDING
```

并验证：

```text
no payload_consumed() callback
later batch commit
    → exactly one persistence_completed(SUCCEEDED)
```

首个实现至少对 Humble vendored mcap-cpp 0.8.0 固化该测试；任何 mcap-cpp upgrade 都必须重新运行，不能把某版本实现行为当作永久 API 保证。

---

## 5.7 Persistence / Failure Injection

至少注入：

```text
Message write Status failure
checked sink partial/short write
checked sink ENOSPC
checked-sink batch-barrier / buffered-flush failure
closeLastChunk path I/O failure
RecordMetadataBatch Attachment write failure
DescriptorBatch Attachment write failure
flush failure
Finalization Manifest write failure
Footer/close sink failure
metadata batch timeout
Channel/Schema capacity exhausted
Starting activation abort
```

验证每种情况下：

```text
HandoffAccepted
PayloadConsumed
PersistenceSucceeded / Failed / Unknown
NormalComplete
```

不会互相偷换。

---

## 5.8 Executable Smoke Validation

当前设计应在实现前至少建立两个独立 smoke test：

### Container test

```text
Header.profile = rodaq/v1
native Schema
native Channel
raw Message.data
DescriptorBatch Attachment
RecordMetadataBatch Attachment
Finalization Metadata
close
reopen
FileOrder decode
Attachment decode
```

### ROS2 projection compatibility experiment

使用真实 ROS2 type，例如：

```text
std_msgs/msg/String
sensor_msgs/msg/JointState
```

验证：

```text
ros2msg / ros2idl Schema projection
cdr Message.data
RoDAQ Attachments do not alter native data bytes
```

并可额外尝试：

```text
ros2 bag info -s mcap
ros2 bag play -s mcap
```

但结果只能记录为目标 rosbag2 版本的 interoperability evidence。

> **由于 Header.profile 仍是 `rodaq/v1`，这些命令的成功不能被提升为 MCAP ROS2-profile conformance。**

---

## 5.9 设计审查结论

本轮对照 rosbag2_storage_mcap、DDS Record & Replay 与 MCAP Format 后，对早期方案做了一个重要修正：

```text
旧方案：
Canonical metadata + Payload
    → custom RecordEnvelope in Message.data

最终 V1：
native Payload
    → normal MCAP Message.data

Canonical record metadata
    → bounded RecordMetadataBatch Attachment

Canonical Descriptor Graph
    → DescriptorBatch Attachment
```

最终方案更符合 MCAP 的实际定位：

- 不丢失 RoDAQ Canonical semantics；
- 不把 uint64 sequence 压进 uint32；
- 不把多个 timestamp 压进两个 MCAP 时间字段；
- 保持 source-native Payload 原样；
- 允许 ROS2/DDS data Channel 尽量使用标准 schema/encoding projection；
- 不复制 Payload 做 native mirror；
- 不需要 private MCAP opcode；
- 不需要修改 mcap-cpp；
- metadata batch 有界并能形成明确 Persistence barrier。

当前没有发现必须重开 Canonical Data Model、Canonical Profile 或 generic Storage Plugin Contract 的结构性问题。

MCAP Mapping 已达到：

> **可以进入 ROS2 + MCAP initial functional implementation，用 executable tests 验证 writer/sink implementation-specific assumptions。**

实现前仍保持 OPEN 的主要内容只有：

```text
metadata batch 默认阈值
chunk 默认大小
CRC 默认策略
compression preset
checked sink 具体类设计
ROS2 ecosystem best-effort interoperability 的实机验证结果
```

这些应由实现与 benchmark 冻结，而不是继续扩张架构。
