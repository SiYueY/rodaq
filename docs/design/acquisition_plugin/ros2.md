# RoDAQ ROS2 Acquisition Plugin Mapping Specification

**Version:** v0.5
**Status:** Plugin ABI Mapping Review Candidate
**Scope:** RoDAQ V1 ROS2 Acquisition Plugin
**Implementation Scope:** Initial V1 functional implementation — required

**Normative Dependencies:**

- RoDAQ Canonical Data Model v0.9
- RoDAQ V1 Canonical Profile v0.6
- RoDAQ System Architecture Design Specification v0.6
- RoDAQ Runtime Design Specification v0.6
- RoDAQ Payload Design Specification v0.5
- RoDAQ Admission Design Specification v0.5
- RoDAQ Acquisition Plugin Design Specification v0.7
- RoDAQ V1 Plugin System Design — current Freeze Candidate

---

# 1. 目标与映射边界

本文定义 ROS 2 Acquisition 在 RoDAQ V1 中的正式 Mapping Contract。

本文不是具体 C++ 类设计，也不冻结：

```text
executor 结构
线程数量
文件拆分
allocator
具体 callback class
graph polling / graph event 实现
```

本文冻结的是：

> **ROS 2 native semantics 到 RoDAQ Runtime / Canonical boundary 的语义映射。**

ROS2 Acquisition 是首个功能版本必须实现的 Acquisition Plugin。

首条完整数据路径为：

```text
ROS 2 Graph / Subscription
        ↓
serialized ROS message + rmw_message_info_t
        ↓
ROS2 Acquisition Plugin
        ↓
source-native facts
        ↓
Runtime registration / Binding
        ↓
Runtime::submit
```

Plugin 不得：

```text
分配 Canonical ID
写入 rodaq/acquisition_ingress
直接修改 Canonical Registry
在 native identity 不可靠时伪造 SourceId
把 reception sequence 当作 SourceSequence
静默截断 native scalar
跨 Plugin 自动合并 Stream
```

V1 继续遵守：

```text
one Canonical Stream
    → one owning Acquisition Plugin
    → one active acquisition path
    → one current Runtime Binding
    → zero or more Source
```

因此一个 ROS Topic 可以存在多个 Publisher Source，但同一 Stream 不允许同时由 ROS2 与 DDS 两条 acquisition path 供数。

## 1.1 ROS2 Humble Native Evidence

目标实现基线为：

```text
Ubuntu 22.04
ROS 2 Humble
C++17
RMW implementation selectable
```

当前开发机默认具备 `rmw_fastrtps_cpp`，但 ROS2 Acquisition Plugin 的 generic boundary 不绑定 Fast DDS RMW；FastDDS Acquisition 由 `fastdds.md` 单独定义；它直接基于 Fast DDS / Fast CDR，不依赖 ROS2 runtime，但必须产生与本文等价的 ROS2 interoperability projection。

实现依赖边界：

```text
RoDAQ ROS2 Plugin
    depends on rclcpp / rcl / rmw / rosidl utilities as needed
    does NOT depend on rosbag2 runtime
```

rosbag2 在本文中是成熟参考实现，不是 RoDAQ ROS2 Plugin 的运行时依赖。这样可以避免把 rosbag2 Writer/Storage data model 重新引入 Acquisition boundary。

ROS2 Humble 的 `rmw_message_info_t` 提供：

```text
source_timestamp
received_timestamp
publication_sequence_number
reception_sequence_number
publisher_gid
from_intra_process
```

其中：

```text
publication_sequence_number
```

由 Publisher 设置，可与 Publisher GID 组合用于识别 source message。

但：

```text
RMW_MESSAGE_INFO_SEQUENCE_NUMBER_UNSUPPORTED
= UINT64_MAX
```

是 ROS2/RMW native sentinel。

因此该 sentinel：

> **必须在 ROS2 Mapping 内转换为 SourceSequence absent。**

不能因为 Profile v0.5 的 Canonical SourceSequence 允许 `UINT64_MAX`，就把 RMW sentinel 误解释成真实 sequence。

`publisher_gid` 的 contract 也需要保守处理。ROS2 Humble 只保证它在 local RMW context 中用于区分 Publisher；不同 context/process 对同一个 Publisher 的 GID 不一定相同，RMW 实现也可能存在 reuse/collision 限制。

所以 ROS2 SourceId 不能仅定义为裸 `publisher_gid`。

V1 把一个 ROS2 native Source identity domain 明确定义为：

```text
ROS2 Acquisition Plugin instance / RMW Context domain
+
RMW implementation identity
```

SourceId 必须把该 domain 与 Publisher GID 一起编码，使所有可能共存于同一 RoDAQ active Identity Scope 的 ROS2 identity domain 彼此可区分。

## 1.2 参考实现与工程取舍

本设计不是从 ROS2 API 表面字段出发重新发明 Recorder，而是重点参考以下成熟实现：

```text
ros2/rosbag2
    rosbag2_transport::Recorder
    rosbag2_cpp::Writer / SequentialWriter
    rosbag2_storage

ros2/rosbag2_storage_mcap
ros2/rosbag2_storage_sqlite3
```

参考仓库：

```text
https://github.com/ros2/rosbag2
```

### rosbag2 值得继承的设计经验

当前 rosbag2 Recorder 已经长期解决了以下 ROS2-specific 问题：

```text
dynamic topic discovery
static topic registration
include / exclude filtering
QoS adaptation / per-topic override
generic serialized subscription
message definition discovery
serialization format selection
bounded writer cache
record / stop / discovery lifecycle
```

这些能力证明：

> **ROS2 Acquisition Plugin 不应该自行解释 ROS message 字段，而应该尽量在 serialized message boundary 上工作。**

当前 rolling rosbag2 已能够在 generic serialized subscription callback 中同时使用 `rclcpp::MessageInfo`；但 RoDAQ 当前目标是 ROS2 Humble，本机 Humble `GenericSubscription` public callback 仍只暴露 serialized message。

因此本文把：

```text
rosbag2 rolling behavior
```

作为设计方向参考，而把：

```text
ROS2 Humble installed headers/API
```

作为首个实现的实际约束。不能把 rolling API 直接复制到 Humble 代码。

rosbag2 官方示例同样明确采用 serialized subscription，避免：

```text
receive serialized bytes
    ↓
deserialize
    ↓
serialize again
```

这种没有必要的 CPU 和内存开销。

RoDAQ V1 沿用这个方向：

```text
ROS2 native serialized bytes
    → Canonical Payload
```

而不是：

```text
ROS message object
    → generic object tree
    → Storage
```

### dynamic discovery

rosbag2 支持：

```text
record all currently known topics
+
discover newly appearing topics while recording
```

也支持 static topic list，使 Topic 尚未出现 Publisher 时就可以提前建立 subscription。

RoDAQ 不需要完整复制 rosbag2 的 CLI/filter feature set，但应继承两个工程结论：

1. discovery 是持续的 Control Plane 工作，不是一次 startup scan；
2. native discovery 与 Canonical registration 必须分层，发现 Topic 不代表已经可以提交 Record。

因此 RoDAQ ROS2 Plugin 的 discovery path 必须允许：

```text
Topic discovered
    ↓
collect native facts
    ↓
validate type / QoS / serialization
    ↓
Canonical registration
    ↓
Storage Descriptor readiness
    ↓
Binding Active
```

而不是 discovery callback 直接写数据。

### QoS adaptation

rosbag2 Recorder 会根据 Publisher offered QoS 自动适配 subscription request，并允许 per-topic override。

RoDAQ V1 应保留这个现实能力，否则：

```text
sensor_data BEST_EFFORT publisher
+
Recorder default RELIABLE subscription
```

可能直接造成 Topic 无法匹配。

但 QoS 是 acquisition policy，不进入 Canonical Record。

因此 V1 建议配置优先级：

```text
explicit per-topic QoS override
        ↓
compatible profile derived from discovered offers
        ↓
default ROS2 recording profile
```

如果当前 discovered Publisher 集合不存在兼容 QoS：

```text
Stream native path = Unavailable/Pending
```

而不是创建一个永远收不到数据的 Active Binding。

### message definition / schema

rosbag2 会保存 topic type、message definition encoding、完整 encoded message definition，并把这些信息交给 Storage Plugin。

RoDAQ 不直接复用 rosbag2 的 TopicMetadata 类型，但沿用这个工程边界：

```text
Acquisition Plugin
    → 负责获得 native type/schema facts

Runtime
    → 负责建立 Canonical SchemaDescriptor / StreamDescriptor

Storage
    → 负责持久化 Canonical Descriptor
```

对于 ROS2 Humble，Schema 获取应优先使用本地已安装 interface/package 的 exact definition dependency closure。

如果 exact dependency-closed schema 暂时不可得：

```text
reliable TypeIdentifier available
    → explicit rodaq/ros2/type-only-v1 SchemaDescriptor

reliable TypeIdentifier unavailable
    → native flow remains Pending / unavailable for normal Admission
```

比“拼一个看似完整但无法 round-trip 的 schema”或把 typed ROS2 Payload 作为无 type identity 的 schema-absent Stream 接受更正确。

### serialized buffer lifetime

rosbag2 Writer 的一个重要实现经验是：

> 可以通过持有原 `rclcpp::SerializedMessage` 的 shared ownership，让下游在不立即复制 Payload 的情况下安全使用 buffer。

这对应 RoDAQ 的 `Retain` 思路。

但首个 RoDAQ ROS2 实现仍建议：

```text
Copy first
```

原因不是 zero-copy 不可行，而是第一阶段更重要的是验证：

```text
Runtime
Admission
Payload ownership
Storage
Stop/failure
```

完整闭环。

等 ROS2 + MCAP correctness baseline 稳定后，再评估基于 serialized-message ownership 的 Retain 优化。

### bounded cache 的启示

rosbag2 `SequentialWriter` 可以使用 bounded message cache，把 callback 和 Storage writer 解耦。

RoDAQ 已经把这个问题提升为更明确的：

```text
Admission reservation
bounded dispatch
Storage handoff
```

因此不应再在 ROS2 Plugin 内复制一套无界 writer cache。

如果 ROS callback 需要从 executor 快速退出，可以增加 Plugin-local bounded staging queue，但必须满足：

```text
bounded capacity
explicit ownership
Stop/quiesce 可关闭
no accepted-record drop policy
```

并且不能绕开 Runtime Admission。

### rosbag2 与 RoDAQ 不同的地方

RoDAQ 不直接复制 rosbag2 的以下语义：

```text
one timestamp field as universal ordering time
rosbag2 TopicMetadata as Canonical descriptor
rosbag2 uint32 message sequence as Canonical sequence
snapshot / pause / split semantics inside Acquisition Plugin
Storage-specific cache behavior inside ROS2 Plugin
```

原因是 RoDAQ 需要同时支持：

```text
ROS2
Fast DDS
future native sensor SDKs
```

以及：

```text
MCAP
SQLite3
future Storage
```

所以参考 rosbag2 的 engineering pattern，但不把 rosbag2 data model 变成 RoDAQ Core data model。

---

## 1.3 Serialized Take Path

V1 需要同时取得：

```text
serialized message bytes
+
rmw_message_info_t
```

不能为了简单使用一个丢失 message info 的高层 callback。

ROS2 Humble `GenericSubscription` 的 virtual dispatch path 接收 `rclcpp::MessageInfo`，但其 public constructor 暴露给用户的 generic serialized callback 只接受：

```text
std::shared_ptr<rclcpp::SerializedMessage>
```

因此首批实现不得仅基于该 callback，然后丢弃：

```text
publisher_gid
publication_sequence_number
received_timestamp
source_timestamp
```

一个符合要求的实现方向是：

```text
SubscriptionBase::take_serialized(..., MessageInfo&)
```

或等价的 RCL/RMW serialized-take path。

具体调用方式属于实现设计，但最终 path 必须保留完整 message info。

---

# 2. ROS2 Native Flow 与 Canonical Registration

## 2.1 Native Logical Stream

V1 中一个 ROS2 native logical flow 至少由以下事实区分：

```text
resolved fully-qualified topic name
+
ROS interface type identity
```

推荐：

```text
StreamName
    = resolved ROS topic name
```

但 StreamName 只是 Canonical logical name：

> **不得作为 Runtime identity key。**

如果同一个 ROS Topic name 同时出现不兼容的 ROS interface type：

> 必须视为不同 native flow，不能静默合并。

同一个 owning ROS2 Plugin 对同一：

```text
resolved topic + compatible ROS type identity
```

的 rediscovery，可以根据 ROS2 Mapping 的 continuity 规则请求继续已有 Stream mapping。

`PayloadEncoding` 不属于长期 Stream identity；它属于 StreamDescriptor interpretation semantics，因此 encoding change 可以在同一 logical Stream continuity 下触发 Descriptor evolution。

Runtime 不根据：

```text
name 相同
schema 内容相同
opaque native bytes 相同
```

自行推断跨 Plugin Stream continuity。

## 2.2 Discovery 与 Activation

ROS Graph discovery 可以在 Runtime `Starting` 阶段、Canonical registration 之前发生。

逻辑流程：

```text
ROS context / node ready
        ↓
discover configured topics
        ↓
resolve topic type
        ↓
load type support / schema facts
        ↓
resolve RMW serialization format
        ↓
build ROS2 Stream facts
        ↓
Runtime Canonical registration
        ↓
Storage Descriptor readiness
        ↓
Binding ready
        ↓
Runtime Activation Commit
        ↓
Plugin obtains normal Submit permission
        ↓
normal serialized take
```

Plugin 可以在 Activation Commit 前创建 subscription/native resource，但不得让正常 callback 进入：

```text
Runtime::submit
```

Dynamic topic discovery 在 Running 阶段继续支持。

新发现的 Stream 必须重新经历：

```text
facts
→ registration
→ Descriptor readiness
→ Binding Active
```

而不能因为 ROS Graph 已发现就直接提交 Record。

## 2.3 Schema / Type / PayloadEncoding

ROS2 serialized Payload 的解释至少依赖：

```text
ROS interface type identity
+
serialization format
```

因此虽然 Canonical Profile 允许 `SchemaId absent`，ROS2 Mapping 进一步收紧：

> **任何可进入 normal Admission 的 typed ROS2 serialized StreamDescriptor 都必须保留 TypeIdentifier；不得因为 exact message definition 暂时不可得而丢失 ROS interface type identity。**

V1 Mapping：

```text
TypeIdentifier
    = exact ROS interface package resource name
      e.g. sensor_msgs/msg/JointState

PayloadEncoding
    = ros2/rmw/<rmw_get_serialization_format()>
```

当本地可获得 `.msg` dependency closure 时：

```text
SchemaFormat = ros2msg
```

`representation` 使用与 MCAP Registry / rosbag2 `encoded_message_definition` 兼容的 dependency bundle：

```text
<top-level .msg bytes>
================================================================================
MSG: <dependency package resource name>
<dependency .msg bytes>
...
```

规则：

- top-level definition 必须第一项且不带 delimiter；
- delimiter 第一行必须恰好 80 个 `=`；
- dependency header 必须是 `MSG: <package resource name>`；
- dependency package resource name 不带文件扩展名；
- 每个依赖必须且只能出现一次；
- 为保证同一 Plugin 对同一 schema 的 representation 稳定，V1 在 top-level 之后按 package resource name 的 bytewise ascending order 输出依赖。

当 `.idl` dependency closure 可获得而 `.msg` 不可获得或配置明确选择 IDL 时：

```text
SchemaFormat = ros2idl
```

`representation` 使用：

```text
================================================================================
IDL: <package resource name>
<IDL bytes>
...
```

每一项（包括 top-level）都有 delimiter；V1 同样按 package resource name bytewise ascending order 输出，并且必须包含解释 top-level type 所需的完整 dependency closure。

因此 `ros2msg` / `ros2idl` 在 RoDAQ V1 中是明确的 byte representation contract，而不是仅用于展示的格式标签。

如果 exact definition 暂时不可得，但 type identity 已可靠获得，V1 允许一个显式的 type-only descriptor：

```text
SchemaFormat
    = rodaq/ros2/type-only-v1

TypeIdentifier
    = exact ROS interface type name

representation
    = empty byte string
```

该格式只声明：

> **type identity 已被精确保留，但 Artifact 不是 self-contained ROS schema bundle。**

它不得伪装成 `ros2msg`，也不得让 native tooling 误认为存在完整 message definition。

对于 V1 required `MCAP Storage + ROS2 interoperability` path，type-only descriptor **不能进入 Ready Binding**：MCAP Descriptor readiness 必须要求完整 `rodaq.projection.ros2.v1` 与 `ros2msg/ros2idl` dependency-complete Schema。type-only 只保留为 generic Acquisition / diagnostic capability，不属于 V1 qualified recording output。

如果后续获得 exact schema：

```text
type-only SchemaDescriptor
        ↓
new exact ros2msg SchemaDescriptor
        ↓
new StreamDescriptor / Binding switch
```

如果连可靠 TypeIdentifier 都无法获得：

> 该 native flow 当前不能进入 normal ROS2 V1 Admission。

Plugin 必须读取 active RMW serialization format，不能仅因为常用 RMW 基于 DDS 就硬编码 `cdr`。

如果 ROS interface type identity 变为不兼容类型，V1 默认视为新的 native logical flow / 新 Stream，而不是把两个不兼容类型并入一个 current Binding。

在 logical Stream continuity 已经成立的前提下，以下 interpretation 变化需要新的 StreamDescriptor：

```text
schema representation / type-only → exact schema evolution
serialization format
accepted TimestampKeySet
Clock interpretation where relevant
```

Descriptor ID 仍由 Runtime 分配。

### 2.3.1 `rodaq.projection.ros2.v1`

ROS2 Acquisition 在发布 StreamDescriptor 后、Storage descriptor readiness 前，必须通过：

```text
rodaq.runtime.projection.v1
```

为该 descriptor 注册 `rodaq.projection.ros2.v1`。

该 projection **不是 Canonical State**；其目的只是把生成标准 ROS2 MCAP 所需要、但不属于 Canonical identity/equality 的信息传递给 Storage。Canonical Schema、StreamName 与 PayloadEncoding 仍然是 authority。

V1 logical fields：

```text
topic_name
    exact normalized ROS2 topic name
    MUST equal Canonical StreamName

type_name
    exact ROS interface package resource name
    MUST equal Canonical Schema.TypeIdentifier

serialization_format
    active RMW serialization format
    V1 qualified MCAP requires cdr-compatible ROS2 serialized payload

offered_qos_profiles
    ROS2 publisher offered QoS set in rosbag2-compatible YAML representation

type_hash
    optional; present only when the selected ROS2 baseline exposes a stable ROS2 type-hash identity
```

Schema body is **not duplicated** into the projection payload. MCAP Storage obtains `SchemaFormat` and exact representation from Canonical Schema and cross-checks：

```text
SchemaFormat ∈ {ros2msg, ros2idl}
TypeIdentifier == projection.type_name
StreamName == projection.topic_name
PayloadEncoding is compatible with projection.serialization_format
```

#### Projection payload encoding

`rodaq.projection.ros2.v1` uses a deterministic binary record so no JSON/YAML parser is required for the outer projection envelope：

```text
u32_le format_version = 1
u32_le topic_name_size
bytes  topic_name UTF-8
u32_le type_name_size
bytes  type_name UTF-8
u32_le serialization_format_size
bytes  serialization_format UTF-8
u32_le offered_qos_profiles_size
bytes  offered_qos_profiles UTF-8 YAML
u32_le type_hash_size
bytes  type_hash UTF-8; zero length = absent
```

Rules：

```text
no trailing bytes
all size arithmetic checked before allocation/copy
all required strings valid UTF-8 and non-empty except offered_qos_profiles may encode an empty set
projection payload subject to Runtime projection bounds
same logical input → same bytes
```

`offered_qos_profiles` YAML follows the rosbag2/ROS2 QoS representation expected by ROS2 MCAP readers. The ROS2 Plugin may implement this encoder itself or reuse a dependency already present in its ROS2 build, but MCAP Storage must not depend on ROS2 libraries to interpret native QoS objects; it only receives the normalized UTF-8 representation.

For multiple currently discovered publishers, the Plugin must build the offered-QoS set deterministically. A change that alters the externally visible offered-QoS projection creates a new immutable projection and therefore a new StreamDescriptor/Binding; it must not mutate projection bytes in place after the descriptor is Ready.

This is especially required for replay-sensitive QoS such as durability (`/tf_static` / transient-local cases).

---

## 2.4 ROS2 Native Metadata Classification

| ROS2 native fact | V1 归属 |
|---|---|
| resolved topic name | StreamName + native flow evidence |
| ROS interface type | TypeIdentifier / Schema input |
| serialized bytes | Canonical Payload |
| RMW serialization format | PayloadEncoding |
| publisher_gid | conditional SourceId input; requires domain-safe canonicalization |
| publication_sequence_number | conditional SourceSequence; requires SourceId and non-sentinel native value |
| reception_sequence_number | 非 SourceSequence；V1 不进入 Canonical State |
| source_timestamp | 条件式 Canonical timestamp，见 §3 |
| received_timestamp | 可映射 reader-side timestamp |
| Runtime ingress time | `rodaq/acquisition_ingress`，只能 Runtime 写入 |
| from_intra_process | operational diagnostic，不进入 V1 Canonical State |
| ROS QoS | acquisition configuration/capability，不是 Record metadata |

未进入 Canonical State 的 native metadata 必须明确分类。

不得为了“保存下来”而把：

```text
publisher_gid
QoS
from_intra_process
native sequence
```

额外塞进 ROS serialized Payload。

---

## 2.5 V1 Plugin 内部组织与配置边界

V1 不需要为每个职责创建 Manager class。

一个克制的实现可以围绕单个 ROS2 Acquisition Plugin 对象维护：

```text
rclcpp::Node / node interfaces
selected topic configuration
discovery state
subscription table
native-flow → Runtime Binding mapping
publisher GID → SourceHandle cache
schema/type cache
quiescence state
```

逻辑上需要区分：

```text
Control Plane
    discovery
    QoS/type/schema resolution
    registration
    Binding switch
    source lifecycle

Data Plane
    serialized take/callback
    MessageInfo extraction
    Source lookup
    timestamp slots
    Runtime::submit
```

但这不要求两个独立 manager hierarchy。

### Topic selection

V1 至少应支持：

```text
explicit topic list
all_topics
include regex
exclude regex
```

配置解析必须在 Starting 前完成。

Running 时允许 ROS Graph 变化，但不允许通过热修改配置任意改变 Plugin instance selection policy；这与 V1 fixed Plugin topology 一致。

### Discovery cadence

推荐同时使用：

```text
ROS graph event
+
bounded periodic rescan
```

原因是单纯依赖 polling 会增加发现延迟，而只依赖 graph event 又容易把所有 correctness 压在 middleware event delivery 上。

periodic rescan 周期属于 tuning parameter，不是 Canonical semantics。

### QoS state

每个 native flow 应缓存：

```text
selected subscription QoS
observed offered QoS set
last compatibility result
```

Publisher QoS 变化后：

- 如果现有 subscription 仍兼容，不需要 Descriptor evolution；
- 如果必须重建 subscription，先 quiesce 当前 native path；
- QoS 本身不自动产生新 StreamDescriptor，因为它不是 Payload interpretation contract；
- 只有 QoS change 同时导致 source/clock/encoding interpretation 改变时，才触发相关 descriptor evolution。

### schema/type cache

ROS interface type 的 dependency closure 属于低频 Control Plane 数据。

建议：

```text
one type name
    → one immutable resolved schema entry
```

在同一 activation 中复用。

如果 package/interface 不存在、definition 不可解析或 dependency closure 不完整：

> schema resolution failure 必须显式记录，不得返回半截 schema 当作 exact representation。

### first implementation data path

首个 ROS2 + MCAP milestone 推荐固定：

```text
serialized take
    ↓
extract MessageInfo
    ↓
lookup current Binding / optional SourceHandle
    ↓
construct timestamp submission slots
    ↓
Runtime::submit(Copy payload)
    ↓
return native serialized buffer
```

这条路径没有 Plugin-local long-lived Payload owner，便于验证 Stop 和 failure。

### 与 Runtime 的调用边界

Data Plane 不得调用：

```text
create Stream
register Schema
switch Binding
mutate Source registry
```

如果 callback 发现未知 GID 或 native semantics 变化：

```text
Data Plane
    → report / enqueue bounded control event
    → current Record 按已有 contract 处理

Control Plane
    → establish Source / Descriptor / Binding state
```

Source 处理必须区分两个完全不同的状态：

```text
A. native Source identity 真正 unavailable / invalid / untrustworthy
B. native Source identity 已可靠获得，但 SourceHandle registration 尚未完成
```

仅 A 可以映射为：

```text
SourceId absent
SourceSequence absent
```

对于 B：

> **不得为了绕过 Control Plane registration 把 known publisher 降级成 SourceId absent。**

在 SourceHandle 建立前，该 Record 必须：

```text
boundedly buffer
or
apply native backpressure
or
drop / pre-admission reject
```

并通过 bounded control event 触发 Source registration。

callback 首次看到可靠 GID 时仍不得直接修改 Runtime Source registry；但“尚未注册”不是“Source identity 不存在”。

---

## 2.6 Stable Plugin ABI Mapping

ROS2 Plugin 的 native mapping 不直接成为 ABI。跨 DSO contract 统一使用 `docs/design/rodaq_plugin_system.md` 定义的 Acquisition Protocol。

### Lifecycle

```text
create
    → 构造 ROS2 Plugin-local state
    → 不创建 subscription / worker

bind
    → query rodaq.runtime.common.v1
    → query rodaq.runtime.acquisition.v1
    → 按需 query rodaq.runtime.canonical.v1
    → require rodaq.acquisition.control.v1

configure
    → 通过 Runtime-owned ConfigView 读取 Topic selection / QoS / discovery 配置
    → 不保存 ConfigView 到调用返回之后

start
    → 创建/激活 ROS2 discovery 与 subscription path
    → 完成 required registration / Binding readiness
    → 只有 Runtime Activation Commit / Gate Open 后才 normal Submit

quiesce_plugin (required control interface)
    → instance-wide non-destructive barrier
    → blocks current/future ROS callback / take routing into Runtime submit
    → preserves ROS-native release capability needed by Runtime-owned Payload anchors

stop
    → destructive ROS-native shutdown only after Runtime drain / anchor safety
    → return 后 no future ROS callback / worker can access Runtime

destroy
    → 只在 callback、originated Payload anchor 与 ROS-native release capability 已安全收敛后调用
```

### Registration

ROS2 Control Plane 通过 `rodaq.runtime.acquisition.v1` 映射：

```text
register_source
register_clock
register_schema
register_stream
publish_stream_descriptor
report_source_availability
report_stream_availability
report_plugin_state
```

ROS native object pointer、`rclcpp` 类型、RMW handle 不得进入 Stable ABI。

### Data Submission

首个实现的 Copy path 映射为：

```text
rodaq_record_submission
    stream / descriptor
    optional SourceHandle
    optional SourceSequence
    explicit timestamp_values[]
    payload.mode = COPY
        ↓
rodaq.runtime.acquisition.v1::submit()
        ↓
rodaq_acquisition_submit_result
```

其中：

```text
publisher GID canonicalized + registered
    → source_present = true
    → source = cached SourceHandle

identity truly unavailable/untrustworthy
    → source_present = false
    → source_sequence_present = false

publication sequence only when proven source-originated + SourceId-scoped
    → source_sequence_present = true
```

ROS2 timestamp 必须先映射为对应 Canonical Clock 的 signed tick reading，再进入 `timestamp_values[]`；不存在的 timestamp 使用 explicit `present = false`，不能用数值 0 代替 Absent。Runtime 自己注入 `rodaq/acquisition_ingress`。

Plugin 必须显式处理：

```text
AdmissionOutcome
RejectionReason
PayloadDisposition
```

Copy path：

```text
PayloadDisposition = NOT_APPLICABLE
```

如果未来启用 Adopt / Retain：

> 必须严格遵守 Payload Transfer/Acquisition Point 和 CallerResponsible / RuntimeResponsible disposition；不能从 rejection reason 推断 ownership。

函数级 `rodaq_status_code != OK` 表示 Runtime boundary failure，不是普通 ROS2 backpressure。

---

# 3. Source、Sequence、Clock、Timestamp 与 Payload

## 3.1 SourceId

ROS Publisher 是 ROS2 Mapping 中的 concrete Source。

SourceId 必须满足 Canonical Profile v0.6 的 Scope-wide domain disjointness。

因此 ROS2 SourceId logical encoding 必须包含：

```text
ROS2 SourceId domain/version tag
+
ROS2 Plugin/RMW-context identity-domain discriminator
+
RMW implementation_identifier
+
RMW-specific canonical GID representation
```

其中 identity-domain discriminator 是 activation 内稳定、并由 Plugin configuration / Runtime configuration 保证在同一 active Scope 内唯一的非 Canonical routing value。

V1 推荐配置名：

```text
source_identity_domain
```

规则：

- 一个 ROS2 Plugin instance 的整个 activation 内保持不变；
- 多个 ROS2 Plugin instance / RMW Context 可能同时存在时必须彼此不同；
- active Scope 内只有一个 ROS2 Plugin instance 时可以使用固定默认 domain，例如 `default`；
- 它不作为独立 Canonical object，Runtime 不解释其业务语义；
- 缺失或重复 domain 导致 Scope-wide collision safety 无法证明时，Plugin 必须禁用 SourceId capability 或在 configuration validation 阶段拒绝。

其中最后一项必须与该 RMW 的 Publisher GID equality semantics 等价。ROS2 提供 `rmw_compare_gids_equal()` 作为正式 GID equality operation，因此 Mapping 不能仅凭 `rmw_gid_t.data` 是固定数组就假设所有 RMW 都把其 raw storage bytes 定义成 canonical byte representation。

对于具体 RMW，只有验证：

```text
rmw_compare_gids_equal(a, b) == true
    ⇔ canonical_gid_bytes(a) == canonical_gid_bytes(b)
```

并且不同 GID 不发生 canonical-byte collision 后，才能使用该 canonicalization。

首批实现可以在所选 RMW 已证明 raw `gid.data` 满足上述条件时直接采用其 fixed bytes；否则必须使用该 RMW 明确定义的 normalization/encoding，或者令 SourceId absent。

V1 ROS2 SourceId 建议冻结一个简单的 versioned binary layout：

```text
ASCII "rodaq.source.ros2.v1\0"

u16 source_identity_domain_length
D   source_identity_domain bytes

u16 implementation_identifier_length
N   implementation_identifier bytes

u16 canonical_gid_length
M   RMW-specific canonical GID bytes
```

其中长度字段使用固定 little-endian encoding。V1 应限制单个 identifier/GID representation 长度在 `uint16` 范围内；超出不是截断，而是 SourceId capability unsupported。

该格式天然与 Fast DDS SourceId domain 分离，同时允许不同 RMW 使用不同 canonical GID representation。

具体 SourceId binary encoding 必须：

```text
deterministic
length-delimited
byte-exact
```

不得直接做无边界字符串拼接。

必须满足：

```text
GIDs equal under the RMW equality contract
inside same RMW identity domain
    → same SourceId bytes

GIDs unequal under that contract
inside that domain
    → different SourceId bytes

different ROS2 source_identity_domain values
    → disjoint SourceId byte domains

ROS2 domain
    != DDS SourceId domain
```

如果：

```text
implementation_identifier unavailable
GID invalid
GID semantics cannot be trusted
```

则：

```text
SourceId = absent
```

不得使用：

```text
StreamId
arrival order
AcceptanceSequence
topic name hash
```

制造假的 SourceId。

Publisher restart 后如果获得新的 valid GID：

> 视为新的 Source。

如果底层 RMW 发生不可区分的 GID reuse/collision：

> 这是 native identity limitation，Plugin 应诊断，而不是由 Runtime 猜测拆分。

## 3.2 SourceSequence

ROS2 `publication_sequence_number` 是 Publisher-originated，因此在 native API 支持时可以映射为 SourceSequence。

```text
SourceId present
+ publication_sequence_number
  != RMW_MESSAGE_INFO_SEQUENCE_NUMBER_UNSUPPORTED
    → SourceSequence present(value)

SourceId absent
    → SourceSequence absent

publication_sequence_number
== RMW_MESSAGE_INFO_SEQUENCE_NUMBER_UNSUPPORTED
    → SourceSequence absent
```

特别注意：

```text
UINT64_MAX
```

在 Canonical Profile 中是合法 SourceSequence value，但在 ROS2 Humble 这个 native field 中被占用为 unsupported sentinel。

所以该解释只能存在于：

> ROS2 Mapping。

Runtime generic layer 不得看到 `UINT64_MAX` 后自行判断 absent。

`reception_sequence_number` 是 subscription-originated：

> 不得映射为 SourceSequence。

它也不得替代 AcceptanceSequence。

ROS2 文档允许 publication sequence 在极长运行后 wrap；RoDAQ 只保存 native source-originated value，不额外宣称 SourceSequence 永不 wrap。

## 3.3 Clock 与 Timestamp

每个 ROS2 Stream 必须具有：

```text
rodaq/acquisition_ingress
```

由 Runtime 写入。

ROS2 Plugin 可以另外提供：

```text
ros2/received_timestamp
```

前提是 `received_timestamp`：

- native API 支持；
- 在该 Binding 生命周期内属于可描述的稳定 reader-side clock domain；
- 能无损进入 Profile v0.5 `int64 ClockReading`。

ROS2 Humble 的 RMW time-point value 本身是 int64 nanosecond value，因此数值域与 V1 `ClockReading` 相容。

对应 ClockOrigin 必须明确指出：

```text
ROS2/RMW receive-time domain
+
RMW implementation identity where necessary
```

不得默认声称它与其他进程、其他 RMW 或其他机器上的 clock 直接可比。

### Source Timestamp 限制

`source_timestamp` 来自 Publisher-side path。

一个 Stream 可以有多个 Publisher Source；这些 Publisher 的 source timestamp 可能来自不同物理 clock instance。

当前 Canonical V1：

```text
TimestampKey
    = RoleIdentifier + one fixed ClockId
```

并要求：

```text
same Scope + same ClockId
    → readings directly comparable
```

因此不得简单执行：

```text
all ROS publishers
source_timestamp
    → one ros2/source_timestamp ClockId
```

V1 默认规则：

```text
multi-source or unproven source clock domain
    → source_timestamp 不注册为 Canonical TimestampKey
```

只有在配置/Mapping 能证明：

> 当前 StreamDescriptor 下所有相关 Record 共享同一个有效 source clock domain

时，才可以增加：

```text
ros2/source_timestamp
```

这通常意味着受约束的 single-source/common-clock 场景。

不支持的 source_timestamp：

- 不得错误绑定 reader ClockId；
- 不得偷偷塞入 Payload；
- 不得因为“字段存在”就破坏 ClockId comparability。

## 3.4 TimestampKeySet

安全的默认 ROS2 V1 TKS：

```text
1. rodaq/acquisition_ingress
2. ros2/received_timestamp      optional
```

证明 common source clock 后可以增加：

```text
3. ros2/source_timestamp        optional
```

因此当前 ROS2 Mapping：

> 不要求提高 `MaxTimestampKeysPerSet = 4`。

Timestamp presence：

```text
Absent
!= Present(0)
```

除非 RMW 明确定义某个值为 unavailable sentinel，否则 `0` 不能自动被解释为 absent。

## 3.5 Payload 与 Ownership

Canonical Payload 是 serialized-take path 得到的 exact ROS message byte string。

Plugin 不应该为了方便：

```text
deserialize
    ↓
reserialize
```

然后继续声称与原 serialized encoding byte-identical。

首个实现推荐先采用：

```text
RMW serialized buffer
        ↓
Runtime Copy submit
        ↓
synchronous exact-byte copy
        ↓
ROS2 buffer reusable
```

这是最简单、最容易证明 ownership 正确的路径。

Adopt / Retain 可以后续优化，但前提是：

- native buffer lifetime 满足 `payload.md`；
- release anchor 不依赖已经被 Plugin Stop 销毁的 ROS resource；
- resource reservation 先于 ownership transfer；
- Stop/failure cleanup 可证明 exactly-once release。

所有 ROS2 Payload 必须先通过：

```text
effective_payload_limit
```

才能 Gate-pass。

---

# 4. Lifecycle、Backpressure 与 Failure

## 4.1 Normal Submission Eligibility

ROS2 Record 只有在以下条件同时满足时才能正常提交：

```text
Plugin Enabled
Runtime Running
Global Admission Gate Open
Binding Active + current
Stream Available
Source eligible when Source is present
```

ROS Graph discovery 或 subscription object 已经存在：

> 不代表 Stream 已经可以 Submit。

如果 schema/encoding/TKS 等语义变化，执行 generic Binding switch：

```text
new facts
    ↓
new Descriptor / Binding Prepared
    ↓
Storage descriptor readiness
    ↓
ROS2 stream-local quiescence
    ↓
ReadyToCommit
    ↓
Runtime switches current Binding
    ↓
CommitComplete
    ↓
resume new semantics
```

V1 这是同一 ROS2 active path 内的语义替换，不是 multi-path failover。

## 4.2 Backpressure

`Runtime::submit` 返回同步 terminal admission result：

```text
Accepted
PreAdmissionRejected
AdmissionRejected
```

普通资源不足必须发生在 Gate-pass 前：

```text
Reject
or
bounded / interruptible Wait
```

ROS callback/take execution 如果允许 Wait：

> 必须能被 Stop/failure/cutoff 打断，不能导致 Plugin quiescence 死锁。

首批 ROS2 实现应该优先：

```text
bounded Reject-oriented policy
```

尤其是高频传感器 Topic。

ROS2/RMW 自己仍有 native queue/QoS loss semantics。

RoDAQ 不得声称：

```text
Runtime Accepted/Rejected
==
ROS transport delivered/lost
```

## 4.3 Source / Stream / Plugin Failure

Publisher disappearance：

```text
Source → Unavailable
```

如果同一 ROS2 active path 仍存在其他 eligible Publisher Source：

```text
Stream may remain Available
```

如果没有任何 eligible Source：

```text
Stream → Unavailable
```

以下 failure 可能升级为 Plugin-wide：

```text
ROS context failure
fatal subscription failure
fatal RMW failure
native discovery subsystem failure
```

因为 V1 一个 Stream 只有一条 active acquisition path，owning ROS2 Plugin fatal failure 后：

> 相关 Stream 不会自动 failover 到 DDS 或第二个 ROS2 Plugin。

Same-semantics recovery 只有在以下均保持有效时才能复用当前 Binding：

```text
native flow identity
schema
PayloadEncoding
ClockDescriptor
TimestampKeySet
Source identity rules
```

否则必须 descriptor/binding evolution。

## 4.4 Quiescence 与 Stop

ROS2 Plugin-wide quiescence 对应 `rodaq.acquisition.control.v1::quiesce_plugin()`，必须证明：

```text
no new ROS callback / take execution
can begin Runtime::submit
```

但这不等于已经进入 Runtime 的 Submit call 全部返回。

Runtime 还必须等待：

```text
active_submission_calls == 0
```

之后才能安全 teardown Submit-path state。

Plugin Stop 在 Runtime 满足 Payload/native-resource safety 后执行，并必须 bounded 地关闭：

```text
subscription
graph watcher
executor-facing acquisition resource
ROS-native worker
```

如果 future Adopt/Retain anchor 的 release 依赖某个 ROS object：

> Plugin Stop 不得先销毁该 release capability。

Starting activation abort 中，如果 Plugin 从未取得 normal Submit permission：

- 不要求先做正常 data-path quiescence；
- 但仍必须 Stop 已经 Prepare 的 native resource。

---

# 5. Conformance 与设计审查结论

## 5.1 明确不属于当前 V1 的能力

```text
ROS2 ↔ DDS cross-Plugin Stream auto-merge

global publisher identity across independent RMW contexts

Canonical persistence of reception_sequence_number

Canonical persistence of from_intra_process

multi-publisher source_timestamp without proven common clock

exact ROS graph / QoS replay as Canonical Record

unproven loaned-buffer zero-copy
```

这些是显式 Mapping boundary，不是隐藏的数据丢失。

## 5.2 必须验证的 Conformance Cases

### Discovery / Registration

```text
deterministic ros2msg dependency bundle round-trip
deterministic ros2idl dependency bundle round-trip
same dependency set discovered in different enumeration order → same representation bytes
startup topic exists
runtime dynamic topic appears
same native flow rediscovery
same topic name + incompatible type
type identity always preserved for admitted typed payload
exact schema unavailable → explicit type-only descriptor or no activation
type-only → exact schema causes Descriptor evolution
Storage readiness before Binding Active
rodaq.projection.ros2.v1 registered before readiness
projection topic/type/serialization cross-checks Canonical Descriptor
projection change → new descriptor, never in-place mutation
```

### Source

```text
two ROS2 Plugin/RMW contexts with identical raw GID bytes but different source_identity_domain → different SourceId
missing / duplicate source_identity_domain when multiple contexts coexist → reject or SourceId disabled
multiple publishers on one Stream
RMW-equal publisher GID → stable SourceId in activation
RMW-unequal publisher GID → different Source
publisher restart / new unequal GID → new Source
ROS2 SourceId domain != DDS SourceId domain
untrustworthy GID → SourceId absent
```

### Sequence

```text
publication sequence supported + SourceId present → present
SourceId absent → SourceSequence absent
RMW UINT64_MAX unsupported sentinel → absent
reception sequence not used as SourceSequence
```

### Timestamp

```text
rodaq/acquisition_ingress always Present
received_timestamp Present(0) remains Present(0)
source timestamp omitted for unproven multi-source clock
optional common-source-clock configuration
```

### Payload / Admission

```text
serialized bytes exact
qualified path serialization format projects to MCAP cdr without decode/re-encode
offered_qos_profiles representation is deterministic
TRANSIENT_LOCAL publisher projection is preserved for replay
oversize → pre-admission rejection
Copy lifetime safe
Stop while pre-gate submit is active
```

首个 Copy path 还必须直接验证 Stable ABI result：

```text
Accepted
    → rodaq_status_code == OK
    → outcome == RODAQ_ADMISSION_ACCEPTED
    → rejection_reason == NONE
    → payload_disposition == NOT_APPLICABLE

ordinary GateClosed / ResourceUnavailable / WaitTimeout
    → rodaq_status_code == OK
    → outcome == RODAQ_ADMISSION_PRE_REJECTED
    → matching rejection_reason
    → payload_disposition == NOT_APPLICABLE
```

如果后续增加 Adopt / Retain path，则必须加入 transfer/acquisition point 前后的 CallerResponsible / RuntimeResponsible conformance cases。

### Lifecycle / Failure

```text
Activation Commit race
publisher disappear/reappear
fatal subscription/RMW failure
active_submission_calls barrier
Plugin Stop timeout/failure
Starting activation abort
```

## 5.3 设计审查结论

ROS2 Humble Mapping 当前没有暴露必须重开 Canonical Data Model 的结构性问题。

它验证了：

```text
SourceSequence = uint64
    可用，但必须做 ROS2-specific sentinel → absent

ClockReading = int64
    与 RMW time point 数值域相容

one Stream + multiple Source
    足以表达多 Publisher Topic

single active acquisition path
    可实现

MaxTimestampKeysPerSet = 4
    对当前安全 ROS2 Mapping 足够
```

当前显式语义边界是：

1. multi-source Stream 的 publisher-side source timestamp 不能安全共享一个 ClockId；
2. exact ROS message definition 不可得时，只能使用明确标记的 type-only SchemaDescriptor，不能丢失 type identity；
3. RMW Publisher GID 只在其 native identity domain 内有意义，因此 SourceId 必须携带 Plugin/RMW-context domain discriminator。

V1 选择：

```text
没有 common clock evidence
→ 不映射该 timestamp
```

而不是削弱 Canonical Clock semantics。

## 5.4 Implementation Readiness

本文已经足以指导 V1 ROS2 Acquisition 实现。实现还必须同时落地 `rodaq.projection.ros2.v1`，因为 V1 MCAP interoperability 已是 required contract，而不是 optional export feature。目标 Humble headers 已确认：

```text
GenericSubscription public callback
    = SerializedMessage only

SubscriptionBase::take_serialized
    = SerializedMessage + MessageInfo
```

因此首个实现不能因为高层 generic callback 更方便就牺牲 `rmw_message_info_t`。

本文仍不冻结：

```text
class name
file layout
executor implementation
discovery polling/event mechanism
allocator
queue container
```

这些实现决策必须服从本文与通用 Acquisition Contract。

## 5.5 Native Evidence

本 Mapping 已对照目标环境中的 ROS2 Humble headers：

```text
rmw/rmw/types.h
rmw/rmw/rmw.h
rmw_compare_gids_equal() equality contract
rclcpp/subscription_base.hpp
rclcpp/generic_subscription.hpp
```

首批实现完成后还需要使用真实 Topic、多个 Publisher、动态发现、Stop race 和 RMW sentinel 做运行验证。
