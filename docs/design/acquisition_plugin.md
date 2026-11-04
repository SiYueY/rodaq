# RoDAQ Acquisition Plugin Design Specification

**Version:** v0.7
**Status:** Plugin ABI Reconciliation Candidate
**Scope:** RoDAQ V1 Acquisition Plugin Architecture and Runtime Contract

**Normative Dependencies:**

- RoDAQ Canonical Data Model Design Specification v0.9
- RoDAQ V1 Canonical Profile v0.6
- RoDAQ System Architecture Design Specification v0.6
- RoDAQ Runtime Design Specification v0.6
- RoDAQ Payload Design Specification v0.5
- RoDAQ Admission Design Specification v0.5
- RoDAQ V1 Plugin System Design — current Freeze Candidate

---

# 1. 目标与设计边界

## 1.1 设计目标

Acquisition Plugin 是 RoDAQ 与外部数据源之间的适配边界。

其职责是：

> **把 source-native discovery、identity、schema、clock、timestamp、sequence 和 payload 等事实转换为 Runtime 可以注册和消费的通用 acquisition facts，并通过 Runtime 已建立的 Binding 提交数据。**

主路径为：

```text
Native Source
    ↓
Acquisition Plugin
    │
    ├── Native Discovery
    ├── Native Identity
    ├── Schema / Type
    ├── Clock / Timestamp
    ├── Source Sequence
    └── Payload
    ↓
Capability Negotiation
    ↓
Canonical Registration
    ↓
Runtime Binding
    ↓
Submit
    ↓
Admission
    ↓
RuntimeRecord
```

Acquisition Plugin 必须：

- 保留 source-native 语义；
- 不伪造不存在的 native identity、clock continuity 或 sequence；
- 不分配 Canonical ID；
- 不直接修改 Canonical Registry；
- 不把 middleware-specific object 泄漏到 Runtime generic boundary；
- 不把关键 native metadata 随意塞入 Payload；
- 在数据进入 Runtime 前完成 native-specific canonicalization；
- 只使用 Runtime 已注册的 Binding / SourceHandle；
- 遵守 Payload ownership contract；
- 遵守 Admission backpressure、rejection 和 cutoff contract；
- 支持 V1 所需的 dynamic discovery、source loss、schema/clock evolution 和 recovery；
- 在 Binding switch、Stop 和 failure cleanup 时提供可证明的 quiescence；
- 区分 Plugin-wide failure 与 Stream-local operational failure；
- 不把 Plugin lifecycle event 或 failure event 伪装成 Canonical data Record。

---

## 1.2 V1 目标 Plugin 类型

V1 Acquisition Contract 的 required concrete implementation 为：

```text
ROS2 Acquisition
FastDDS Acquisition
```

两者使用不同 native path：

```text
ROS2 Acquisition
    → rcl/rmw serialized path
    → ROS2 runtime dependency allowed

FastDDS Acquisition
    → Fast DDS / Fast CDR direct path
    → DDS-Pipe / DDS Record & Replay as source-level references
    → no ROS2 runtime dependency
    → reconstruct ROS2-compatible wire semantics
```

两种 Plugin 都必须产生相同版本的 typed `Ros2Projection`，使 V1 MCAP Storage 不需要判断 Record 来自 ROS2 还是 FastDDS。

SocketCAN、Generic DDS、camera SDK 等不属于 V1 implementation/release gate，但继续作为架构扩展目标。公共 Contract 必须允许这些 Plugin 在不修改 Canonical Model 和 Runtime 基础语义的情况下接入。

Contract 同时不能依赖这些 backend 的具体类型。

Runtime generic interface 不得要求：

```text
rclcpp::SerializedMessage
rmw_gid_t
DDS::SampleInfo
DDS::InstanceHandle
can_frame
```

作为 Core 类型。

未来：

```text
camera SDK
LiDAR
radar
IMU
DAQ / ADC
audio
shared memory
Zenoh
eCAL
vendor transport
```

也应能够在不改变 Canonical Model 和 Runtime 基础 contract 的情况下接入。

---

## 1.2.1 Typed Interoperability Projection

Canonical Model 只表达长期稳定的数据语义，不承担某个外部生态的文件互操作细节。但 Storage Backend 为了生成标准 Artifact，可能需要 Acquisition 已经掌握、又不属于 Canonical identity/equality 的低频事实，例如 ROS2 publisher offered QoS。

V1 因此引入一个受控的 **typed interoperability projection** 机制：

```text
Acquisition native facts
        ↓
Canonical registration
        +
versioned InteropProjection
        ↓
Runtime-owned Projection Registry
        ↓
Storage Descriptor Readiness
```

该机制必须满足：

```text
not Canonical State
not part of Record Identity
not part of Stream/Descriptor equality
immutable after publication
bounded per descriptor and per activation
versioned by explicit projection identifier
Runtime-mediated; no Acquisition → Storage direct call
not queried on every Record hot path
```

V1 冻结的 projection identifier：

```text
rodaq.projection.ros2.v1
```

其 logical fields 至少包括：

```text
normalized ROS2 topic name
exact ROS2 type name
serialization format
publisher offered QoS representation
optional ROS2 type/hash identity when the selected ROS2 baseline provides it
```

Schema format / representation **不在 projection 中复制**。它们继续由 Canonical SchemaDescriptor 精确保存；projection 中与 StreamName / TypeIdentifier / PayloadEncoding 重叠的字段只用于 **cross-check**，不能成为另一套 Canonical authority。Storage 必须拒绝 projection 与 Canonical Descriptor 相互矛盾的情况。

Runtime 通过独立的 `rodaq.runtime.projection.v1` Service 注册和查询 projection。Runtime 对 projection payload 的业务语义保持 opaque，只负责：

```text
identifier/version validation
ownership/lifetime
immutability
bounds
(StreamDescriptor, projection-id) association
```

V1 MCAP Storage 要求每个要进入正式 ROS2-compatible Artifact 的 StreamDescriptor 都存在有效 `rodaq.projection.ros2.v1`。缺失或不完整时 Descriptor readiness 必须返回 Unsupported，而不是生成一个“看起来像 ROS2”的降级 Channel。

Descriptor publication returns the immutable `DescriptorHandle` before Storage readiness can
complete. Acquisition may then register its typed InteropProjection through
`rodaq.runtime.projection.v1`. Projection registration notifications are delivered after the
Projection Registry lock is released and may trigger another readiness evaluation for the same
Descriptor. Missing projection is a pending readiness condition, not permission to mutate the
already published Descriptor.

未来其它生态可以增加新的 typed projection，例如 CAN/MDF mapping；这不等于引入 Generic PropertyBag。

---

## 1.3 Plugin 与 Core 的责任边界

Acquisition Plugin 负责：

```text
native API ownership

native discovery

native identity interpretation

native source canonicalization

native schema/type extraction

native clock interpretation

native timestamp extraction

native sequence extraction

native payload preparation

native failure detection

native recovery

callback quiescence
```

Runtime 负责：

```text
Identity Scope

Canonical ID allocation

Descriptor Registry

Source interning

Clock / TimestampKeySet registration

Binding lifecycle

Admission Gate

AcceptanceSequence

mandatory ingress timestamp

bounded admission

RuntimeRecord

Storage dispatch
```

因此：

> **Plugin 提供 source-native facts，Runtime 决定 Canonical identity 和 Runtime operational state。**

---

## 1.4 Native Fact ≠ Canonical Object

Plugin 发现：

```text
native topic
DDS DataWriter
CAN identifier
native publisher GUID
native type
native clock
```

并不意味着这些对象直接成为：

```text
Stream
Source
SchemaDescriptor
ClockDescriptor
```

Plugin 必须把 native 信息转换为 Runtime registration 所需要的明确 facts。

Runtime 再根据：

```text
active Identity Scope
existing Registry
Profile rules
current Runtime state
```

建立 Canonical object。

---

## 1.5 Plugin 不分配 Canonical ID

Plugin 不得自行创建：

```text
StreamId
StreamDescriptorId
SchemaId
ClockId
TimestampKeySetId
SourceHandle
BindingHandle
```

Plugin 可以保存 Runtime 返回的 handle。

Canonical identity allocation authority：

> 始终属于 Runtime。

---

## 1.6 Plugin 实例、Native Flow 与 Canonical Stream

一个 Acquisition Plugin instance 可以对应：

```text
multiple native logical flows
multiple Streams
multiple Sources
multiple Bindings across different Streams
```

但是 Canonical Profile v0.6 对 V1 冻结：

> **每个 Canonical Stream 任一时刻最多只有一条 active acquisition path、一个 owning Acquisition Plugin 和一个 current Runtime Binding。**

因此不得建立：

```text
Plugin instance
==
Canonical Stream
```

也不得建立：

```text
one Canonical Stream
    ← Plugin A / Binding A
    ← Plugin B / Binding B
```

这种 multi-path V1 topology。

一个 Stream 仍可以关联多个 concrete Source，只要这些 Source 都通过该 Stream 唯一 active acquisition path / current Binding 提交。

---

### 1.6.1 Stream Registration Responsibility

V1 不引入跨 Plugin 的 generic logical-stream identity key，也不要求 Runtime 理解 ROS2 Topic、DDS Topic、future SocketCAN channel 等 source-specific equality semantics。

责任边界为：

```text
Acquisition Plugin / Mapping
    → identify continuity inside its own native logical-flow domain
    → provide typed Stream facts

Runtime Control Plane
    → validate V1 single-path topology
    → allocate / own StreamId
    → register Stream / Descriptor / Binding
```

对于同一个 Plugin instance / native mapping，只有 Plugin 能够根据其 Acquisition Contract 证明：

```text
rediscovered native flow
== same logical native flow
```

时，才可以请求继续使用已有 Runtime Stream mapping。

Runtime 不根据：

```text
StreamName equality
opaque native byte coincidence
descriptor content equality alone
```

自动推断 Stream continuity。

如果两个不同 Plugin instance 都发现可能表示同一外部数据流：

> **V1 Runtime 不执行跨 Plugin Stream 自动合并。**

它们在 V1 中必须保持为独立 Stream registration / acquisition path，除非 Application 通过 stop → reconfigure → new activation 明确选择唯一负责该 logical Stream 的 Plugin/path。

Plugin 本地 routing state 同样不得仅因为两个 native flow 的 `StreamName` 相同就自行合并。

这一规则避免为了尚未证明必要的 cross-plugin merge 引入：

```text
GenericStreamIdentity
AcquisitionPath registry
path failover
duplicate suppression
```

等额外抽象。

---

## 1.7 V1 拓扑约束

V1 Runtime activation 前固定：

```text
Acquisition Plugin instance set

per-Stream owning Plugin / active path
```

Running 期间不支持：

```text
add Plugin instance
remove Plugin instance
replace Plugin instance
hot-load Plugin
hot-unload Plugin
```

但是已经存在的 Plugin instance：

> 可以在 Running 期间动态发现、失去或重新发现 native Stream / Source。

因此必须区分：

```text
Plugin topology
```

和：

```text
native discovery topology
```

前者 V1 固定，后者允许动态变化。

Dynamic discovery 不改变 V1 single-path cardinality：一个新发现的 native flow 可以创建新 Stream，或者在同一 owning Plugin / mapping 已证明 continuity 时继续已有 Stream，但不能让第二个 Plugin/path 同时附着到已有 Stream。

---

# 2. Plugin 生命周期与运行模型

## 2.1 Plugin Operational Lifecycle

Acquisition Plugin 的 Plugin-wide 逻辑生命周期为：

```text
Configured
    ↓
Prepared
    ↓
Enabled
    ↓
Quiescing
    ↓
Stopped
```

Plugin-wide 异常状态为：

```text
Failed
```

`Failed` 表示：

> Plugin 整体当前无法满足 normal acquisition contract。

其 failure disposition 可以是：

```text
Recoverable
Terminal
```

不要求实现为独立 enum。

单个 Stream / Binding 的 failure：

> 不自动意味着 Plugin 进入 `Failed`。

例如：

```text
Source loss
Stream-specific registration failure
Binding switch timeout
```

可以只使受影响 Stream：

```text
Unavailable
Paused
SwitchFailed
```

其中 `SwitchFailed` 只是本文描述 operational condition 的术语：

> 不要求 C++ 实现新增同名 lifecycle state。

其他健康 Stream：

> 可以继续采集。

只有故障影响：

```text
Plugin shared native infrastructure
Plugin shared routing correctness
Plugin-wide synchronization
Plugin-wide quiescence safety
```

或者 Runtime 明确要求 escalation 时，

才升级为：

```text
Plugin Failed
```

---

## 2.2 Runtime Lifecycle 与 Plugin Lifecycle 映射

正常映射为：

```text
Runtime Constructed / Configured
        ↓
Plugin Configure
        ↓
Plugin Configured

Runtime Starting
        ↓
Plugin Prepare
        ↓
Plugin Prepared
        ↓
native discovery
        ↓
Canonical registration
        ↓
required Bindings ready
        ↓
Plugin remains unable to start
normal Runtime::Submit
        ↓
Runtime Activation Commit
publishes Running + Gate Open
        ↓
Runtime releases Plugin Enable permission
        ↓
Plugin Enabled
        ↓
normal acquisition / Submit

Runtime Stopping
        ↓
Plugin-wide Quiesce
        ↓
Plugin Quiescing
        ↓
quiescence established
        ↓
Runtime waits active_submission_calls == 0
and resolves accepted Payload / Storage obligations
required for safe native-resource destruction
        ↓
Runtime requests Plugin Stop
        ↓
Plugin Stopped
```

Starting activation abort 为：

```text
Runtime Starting aborts before Activation Commit
        ↓
Gate remains Closed
Plugin never receives normal Submit permission
        ↓
Runtime cancels / settles pending registration work
        ↓
Plugin Stop for every successfully Prepared Plugin
        ↓
Plugin Stopped or explicit failure / timeout
```

Prepared-but-never-Enabled Plugin 不需要先经历 normal data-path Quiesce，但 Stop 仍必须关闭 discovery/native resources 并满足 memory-safety contract。

failure cleanup 为：

```text
Runtime failure
or
Plugin terminal failure
        ↓
Admission restricted / closed
as required
        ↓
Plugin-wide Quiesce
        │
        ├── success
        │       ↓
        │     Plugin Stop
        │
        └── timeout / failure
                ↓
           memory-safe retention
           / quarantine
```

Stream-local failure 则为：

```text
one Stream fails
        ↓
affected Stream
Unavailable / Paused
        ↓
Plugin remains Enabled
for healthy Streams
        ↓
Runtime decides
recovery / cleanup / escalation
```

---

### 2.2.1 Lifecycle Transition Authority

以下 Plugin transition 主要由 Runtime 请求：

```text
Configure
Prepare
Enable
Quiesce
Stop
```

Plugin 可以主动向 Runtime Control Plane 报告：

```text
native failure
source loss
recovery
new discovery
semantic change
binding-switch progress failure
```

这些 report：

> 是 operational information，不是 Runtime lifecycle command。

Runtime 根据自身 lifecycle、配置和其他 subsystem state 决定：

```text
availability update
registration work
recovery
failure escalation
deferred response
```

Plugin：

> 不得假设 Runtime 必然按照 Plugin 希望的方式响应某个 report。

同时 Runtime：

> 不得通过忽略 report 改写 Plugin 已经进入的本地 operational state。

例如某个 Stream 已进入：

```text
Unavailable / SwitchFailed
```

则在 Runtime 尚未处理 report 时：

> Plugin 也不得继续向该 Stream normal Submit。

如果 Plugin shared infrastructure 已进入：

```text
Failed
```

则 Plugin 停止 Plugin-wide normal Submit。

Plugin 不自行决定：

```text
Runtime Running → Stopping
Runtime Running → Failed
```

等 Runtime lifecycle transition。

---

### 2.2.2 Stable Plugin ABI Lifecycle Mapping

本节中的：

```text
Configured
Prepared
Enabled
Quiescing
Stopped
```

是 Acquisition Plugin 的**领域运行状态**，不是跨 DSO 的 ABI method 列表。

V1 Stable Plugin ABI 只冻结：

```text
create
bind
configure
start
stop
query_interface
destroy
```

映射关系为：

```text
ABI create
    → 构造 Plugin-local instance
    → 不访问 Runtime、不启动 native resource

ABI bind
    → 通过 Service Registry 获取 Runtime Services
    → 建立 Plugin → Runtime capability binding

ABI configure
    → 建立本文的 Configured state
    → ConfigView 只在该调用期间有效

ABI start
    → 封装本文的 Prepare + required discovery/registration + Enable
    → 只有在 Runtime Activation Commit / Gate Open 允许后才能进入 normal Submit

required `rodaq.acquisition.control.v1::quiesce_plugin()`
    → Plugin-wide non-destructive quiescence barrier
    → 禁止所有当前/未来 normal Submit routing
    → 保留 Runtime-owned Payload anchor release 所需 native capability

ABI stop
    → 仅在 Runtime drain / Payload-anchor safety 已满足后执行 destructive native shutdown
    → 是 destructive-stop completion barrier，不再承担 pre-drain Plugin-wide quiescence

ABI destroy
    → 仅在 no-future-callback / no-live-originated-anchor 条件满足后调用
```

因此本文后续出现的：

```text
Prepare
Enable
Quiesce Binding
Quiesce Plugin
Report Recovery
```

都应理解为：

> Plugin 内部领域 capability / Runtime coordination step。

它们**不是**要求在 `rodaq_acquisition_plugin` core vtable 中继续增加同名 ABI slot。

V1 correctness 所必需的 Binding switch / stream-local quiescence 通过 required interface：

```text
rodaq.acquisition.control.v1
```

暴露，而不是继续扩张 core lifecycle。

其中：

```text
prepare_binding_switch() success == ReadyToCommit
commit_binding_switch() success  == CommitComplete
abort_binding_switch(disposition) == pre-commit abort with explicit KEEP_PAUSED / RESUME_OLD
quiesce_stream()                  == stream-local quiescence barrier
quiesce_plugin()                  == instance-wide non-destructive quiescence barrier
```

该 extension 对 V1 conforming Acquisition Plugin 是 REQUIRED；缺失属于 protocol incompatibility。

真正 optional capability 才使用 optional versioned Plugin Extension。

---

## 2.3 Configured

Configured 表示：

> Plugin 已获得并验证静态配置，但尚未进入 normal acquisition。

配置可以包括：

```text
native endpoints
interface names
topic / type filters
domain / participant settings
CAN interface
discovery policy
backpressure policy
timestamp policy
required startup targets
resource limits
```

配置验证失败：

> Runtime activation 失败。

---

## 2.4 Prepared

Prepared 阶段 Plugin 可以：

```text
open native transport
create participant / node
start discovery
enumerate native resources
prepare readers
read native type metadata
inspect clock capabilities
```

但：

> Prepared 不意味着允许 normal Submit。

---

### 2.4.1 Discovery 与 Identity Scope

native discovery：

> 本身不要求 active Identity Scope。

例如 Plugin 可以提前发现：

```text
ROS2 topic
DDS topic
CAN interface
```

但是：

> Canonical registration 必须发生在 active Identity Scope 建立之后。

因此：

```text
native discovery
```

和：

```text
Canonical registration
```

是两个不同阶段。

---

## 2.5 Enabled

Plugin 只有在 Runtime 已完成 Architecture/Runtime v0.5 定义的 Activation Commit 后，才允许进入 `Enabled`。

Enabled 表示：

> Plugin 已被 Runtime 允许进入 normal acquisition data path。

正常 activation prerequisite 为：

```text
Plugin Prepared
        ↓
native facts available
        ↓
required Canonical registration
        ↓
required Bindings ready
        ↓
Storage ready
        ↓
Admission ready
        ↓
Plugin Enabled
```

根据 backend 特性，Runtime 可以允许：

> 在 Plugin Enabled 前提前允许 native reader 进入 normal `Runtime::Submit` path。

Plugin 可以在 Prepared 阶段建立 native reader / discovery resources，但必须通过 local routing gate / callback barrier 保证 normal Submit permission 直到 Activation Commit 后才发布。

但 early sample 仍必须遵守 §2.6。

Plugin Enabled：

> 不意味着其所有 Stream 必然 Available。

Plugin 可以保持 Enabled，同时部分 Stream：

```text
Unavailable
Paused
SwitchFailed
```

其他健康 Stream继续工作。

---

## 2.6 Early Native Samples

某些 backend 在 native reader 建立后可能立刻产生 sample。

如果 sample 到达时：

```text
Plugin not Enabled

or

Binding not Active/current

or

Source not registered
```

Plugin 必须按 configured policy：

```text
Drop
BoundedBuffer
NativeBackpressure
```

之一处理。

Runtime：

> 不为这种 sample 自动执行 hot-path first-time registration。

---

### 2.6.1 Early-sample Timestamp

如果 Plugin 使用：

```text
BoundedBuffer
```

保存 early sample，

则该 sample 真正进入：

```text
Runtime::Submit
```

时，Runtime 才捕获：

```text
rodaq/acquisition_ingress
```

因此 ingress timestamp 表示：

> sample 实际进入 Runtime admission boundary 的时间。

它不是：

> native source 最早把 sample 交给 Plugin 的时间。

如果 native arrival time 有价值，

Plugin 可以通过已经注册的：

```text
Plugin-owned timestamp role
```

保存。

Plugin 不得：

> 为补偿 buffering 而伪造 `rodaq/acquisition_ingress`。

---

## 2.7 Quiescing

Quiescing 表示 Plugin 正在建立：

```text
no future normal submission
```

或 Binding-local：

```text
no future submission
through selected old Binding
```

的保证。

用于：

```text
Binding switch
Runtime Stop
Plugin failure cleanup
selected recovery transition
```

---

### 2.7.1 Quiescing 期间的 Native Sample

一旦对应 quiescing boundary 生效：

> Plugin 不得开始新的受影响 normal Submit。

期间 sample 按 configured pause policy：

```text
Drop
BoundedBuffer
NativeBackpressure
```

处理。

Binding-local Quiescing：

> 只影响目标 Stream / Binding。

其他 Stream 可继续 Submit。

Plugin-wide Quiescing：

> 所有 Plugin routing path 都停止开始新的 normal Submit。

---

### 2.7.2 Quiescing Progress

Plugin 在 Quiescing 期间：

> 必须继续响应 Runtime 的 Control Plane progress / completion coordination。

停止 data submission：

> 不得同时停止 Control Plane progress。

已经进入：

```text
Runtime::Submit
```

的调用：

> 由 Runtime Admission Contract 收敛。

---

## 2.8 Failed

`Failed` 表示：

> Plugin-wide normal acquisition contract 当前无法满足。

---

### 2.8.1 Recoverable Failure

Recoverable Failure 表示：

> Plugin shared path 当前 unavailable，但 backend 允许受控恢复。

进入 Failed 后：

- Plugin-wide normal Submit 停止；
- Runtime 更新受影响 Source / Stream availability；
- Runtime 仍可以请求 Plugin-wide quiescence；
- Plugin 可以在 Runtime 仍允许 recovery 时执行 bounded recovery；
- recovery 结果通过 Control Plane 报告。

如果恢复成功：

#### Same semantics

```text
Failed
    ↓
native path restored
    ↓
semantic continuity validated
    ↓
availability restored
    ↓
Enabled
```

#### Changed semantics

```text
Failed
    ↓
native path restored
    ↓
new facts
    ↓
new registration
    ↓
new Binding
    ↓
Binding switch
    ↓
Enabled
```

---

### 2.8.2 Terminal Failure

Terminal Failure 表示：

> Plugin 无法恢复 normal acquisition。

Plugin 保持：

```text
Failed
```

直到 Runtime teardown。

Runtime 仍可以请求：

```text
best-effort Plugin-wide Quiesce
best-effort Stop
```

如果无法安全 quiesce：

> 按 §6.14 采用 memory-safe retention / quarantine。

---

### 2.8.3 Runtime Failed 与 Plugin Recovery

如果 Runtime 已经进入 terminal：

```text
Failed
```

Plugin 不得通过 recovery：

> 重新进入 Enabled 或重新打开 normal Submit path。

Plugin 在一个 recovery attempt 开始前必须通过：

```text
rodaq.runtime.common.v1::get_lifecycle_snapshot()
```

读取一次 Runtime lifecycle/admission snapshot。只有：

```text
state == RUNNING
AND normal_acquisition_allowed == true
```

才允许开始 normal recovery。

如果 snapshot 显示：

```text
state != RUNNING
OR normal_acquisition_allowed == false
```

则不开始 normal recovery。查询之后发生的 Stop/Failure 仍由后续 Runtime registration/report/submit 的当前状态检查最终拒绝；snapshot 不是可长期持有的 permission token。

此时只允许：

```text
best-effort quiescence
cleanup
Stop
```

Plugin：

> 不应持续轮询 Runtime lifecycle state 作为正常工作机制。

正确语义是：

```text
one recovery attempt
    → one Runtime lifecycle confirmation
```

而不是：

```text
continuous Runtime state polling
```

该 query 已由 Stable Plugin Contract 冻结在 `rodaq.runtime.common.v1`。V1 不要求额外 terminal notification channel。

---

## 2.9 Stopped

Stopped 时 Plugin 必须保证：

```text
no new native callback
can reach Runtime::Submit

no Plugin worker
can begin a new Submit

no deferred task
can re-enter Runtime Submit path
```

之后才可以安全销毁 Plugin native resource。

---

## 2.10 Plugin Threading

Plugin 可以拥有：

```text
native callback threads
discovery thread
reader worker
deferred submit worker
native event thread
```

并允许多个线程同时：

```text
Runtime::Submit
```

Runtime Admission 负责：

```text
per-stream AcceptanceSequence ordering
```

Plugin 不需要为了 Runtime ordering：

> 把整个 Plugin 串行化成单线程。

Plugin 必须自己保证：

```text
native object thread safety
Binding routing state
callback quiescence
Payload owner lifetime
deferred task lifetime
```

正确。

---

# 3. Discovery、Registration 与 Canonical Facts

## 3.1 Discovery

Plugin discovery 负责识别：

```text
native logical data flow

native producers

native type/schema

native encoding

native clocks

available timestamps

native source sequence capability
```

Discovery 可以：

```text
startup discovery

continuous dynamic discovery

event-driven discovery
```

具体算法由 Plugin 决定。

---

## 3.2 Native Stream Facts

Plugin 需要为候选 native flow 提供 typed facts，并先在对应 Acquisition Mapping 内判断 native-flow continuity。

Plugin 可以向 Runtime 表达：

```text
new native logical flow

or

rediscovery / evolution of an already mapped
flow owned by this Plugin instance
```

Runtime 根据该 typed request、当前 Scope 和 Registry 决定 Canonical registration / Descriptor evolution，但：

> **不自行解释跨 Plugin source-native facts 来判断两个 Plugin 是否对应同一个 logical Stream。**

facts 可以包含：

```text
native logical name

native domain/context

native topic / channel identifier

native type identity

PayloadEncoding

schema availability

timestamp capability

clock facts
```

Runtime generic contract：

> 不使用通用 PropertyBag 表达这些 facts。

应使用明确 typed contract。

### 3.2.1 Stream Registration Identity

Stable Runtime Stream registration key 为：

```text
(PluginInstance, native_id bytes)
```

同一 activation 内重复注册相同 key 必须幂等并返回同一 StreamHandle/StreamId。不同 Plugin instance 的相同 native bytes 不自动合并。

Plugin 必须把 native logical Stream continuity 编码进 `native_id` evidence：

```text
same logical Stream
    → keep same native_id

logical Stream identity changed
    → new native_id
```

rename / Schema / PayloadEncoding / TimestampKeySet evolution 不改变 Stream identity，只产生新 StreamDescriptor。

---

## 3.3 StreamName

Plugin 可以提供：

```text
StreamName candidate
```

StreamName：

> 是描述性字段，不是 Canonical identity key。

因此：

```text
same StreamName
```

不意味着：

```text
same Stream
```

Runtime 不得仅根据 name 合并 Stream。

---

## 3.4 PayloadEncoding

Plugin 必须明确报告：

```text
PayloadEncoding
```

它描述：

> Payload logical bytes 的编码语义。

具体 mapping 可以定义：

```text
ROS2 serialized message encoding
DDS serialized sample encoding
CAN frame encoding
```

如果 PayloadEncoding 改变：

> 必须建立新的 StreamDescriptor / Binding。

不得在同一个 immutable StreamDescriptor 下静默改变 encoding。

---

## 3.5 Schema / Type

如果 native source 提供 schema/type information，

Plugin 必须按 Canonical SchemaDescriptor contract 提供：

```text
SchemaFormat

optional TypeIdentifier

exact representation bytes
```

Plugin 不得：

```text
parse schema
    ↓
regenerate semantically equivalent
but byte-different representation
    ↓
claim exact full fidelity
```

除非具体 backend mapping 明确只承诺 semantic equivalence。

---

### 3.5.1 Schema Absent

Canonical Schema 是 optional。

如果 backend：

```text
does not expose schema
```

或者：

```text
schema not yet known
```

Plugin 可以注册：

```text
schema-absent StreamDescriptor
```

只要 PayloadEncoding 本身仍然合法、可解释。

---

### 3.5.2 Late Schema

schema 晚于数据发现时，

V1 允许两种策略。

#### Buffer until schema available

```text
native sample
    ↓
bounded Plugin buffer
    ↓
schema discovered
    ↓
registration
    ↓
Submit
```

#### Emit schema-absent Binding

```text
schema unavailable
    ↓
register schema-absent descriptor
    ↓
Submit
    ↓
schema later available
    ↓
new StreamDescriptor
    ↓
Binding switch
```

不得：

> retroactively 修改旧 StreamDescriptor。

---

## 3.6 Source Identity

Canonical `SourceId` 是：

> Plugin canonicalized native producer identity。

如果 backend 存在稳定 native source identity，

Plugin 可以提供：

```text
non-empty opaque SourceId bytes
```

Plugin 负责：

```text
native GUID interpretation
endianness
native binary representation
normalization
canonical byte serialization
```

Runtime：

> 不解释 native SourceId bytes。

---

### 3.6.1 SourceId Requirements

同一 Plugin-defined native identity domain 内：

```text
same native source
    ⇒ same SourceId bytes

different native source
    ⇒ different SourceId bytes
```

同时 Canonical SourceId equality 的真正比较域是整个 active Identity Scope。

因此对应 Acquisition Contract / Mapping 还必须保证：

> **所有可能在同一 active Scope 中共存的 native identity domain，其 Canonical SourceId byte domain 彼此 disjoint。**

例如概念上可以编码：

```text
ROS2-domain-tag || canonical-gid
DDS-domain-tag  || canonical-guid
```

本文不冻结具体 tag/encoding，但禁止两个独立 identity domain 仅因为 native bytes 碰巧相同而被 Runtime byte-exact interning 合并。

如果 Plugin / Mapping 无法同时可靠满足 domain-local collision-free 与 Scope-wide domain disjointness：

> SourceId 必须 absent。

不得：

```text
hash unstable pointer
use callback address
generate random UUID
reuse AcceptanceSequence
```

伪造 SourceId。

---

### 3.6.2 SourceHandle Registration

第一次发现 SourceId 时：

> Plugin 必须通过 Control Plane 注册 / intern Source。

Runtime 返回：

```text
SourceHandle
```

之后 Data Plane submission 使用 SourceHandle。

Runtime Data Plane：

> 不执行 first-time Source interning。

---

## 3.7 Unregistered Source Sample

必须区分：

```text
Source identity truly absent

registration-pending known Source
```

### Source identity truly absent

如果 native Source identity：

```text
unknown
invalid
unstable
ambiguous
or not reliably available
```

则：

```text
SourceId = absent
SourceSequence = absent
```

这是合法 Canonical Record state。

### Registration-pending known Source

如果 Plugin 已经可靠获得并 canonicalize 出 SourceId bytes，但对应 SourceHandle 尚未完成 registration：

> **sample 不得进入 Admission，也不得临时改写为 SourceId absent。**

Plugin 可以：

```text
Drop / pre-admission reject
BoundedBuffer
NativeBackpressure
```

并通过 Control Plane 完成 Source registration。

不得：

```text
Submit known-but-unregistered Source as absent
Submit raw SourceId and ask Runtime to register it on hot path
mutate Source registry from Data Plane callback
```

这保持：

```text
Control Plane mutates Source Registry
Data Plane reads stable SourceHandle
```

同时避免因为 registration latency 丢失已经可靠获得的 Canonical Source identity。

---

## 3.8 SourceSequence

如果 native producer 提供真正的 source-originated sequence，

Plugin 可以映射为：

```text
SourceSequence
```

前提：

```text
SourceSequence
⇒
SourceId / SourceHandle exists
```

Profile v0.5 的 SourceSequence value domain 是完整 `uint64`，其中 `0` 合法。

如果 native API 使用某个数值作为：

```text
unsupported
unknown
invalid
```

sentinel，Plugin / Mapping 必须在进入 Runtime 前把该 native state 转换为：

```text
SourceSequence = absent
```

不得要求 Runtime 通过 `0`、`UINT64_MAX` 或其他 numeric value 猜测 native sentinel。

以下不得直接映射为 SourceSequence：

```text
Runtime callback counter
RMW receive counter
local reader index
AcceptanceSequence
```

除非 backend mapping 能证明它就是 source-originated sequence。

---

## 3.9 Clock Facts

Plugin 负责识别 backend-native clock。

native clock registration facts 至少包括：

```text
resolution

origin

native clock identity evidence
```

Runtime：

> 分配 ClockId。

Plugin 不分配 ClockId。

---

### 3.9.1 Clock Continuity

如果 native clock：

```text
reset
restart
reinitialize
lose continuity
```

且 continuity 不再能够证明，

则必须视为：

> new clock instance。

通常需要：

```text
new ClockDescriptor
        ↓
new TimestampKeySet
        ↓
new StreamDescriptor
        ↓
new Binding
```

不得继续复用旧 ClockId 语义。

---

### 3.9.2 Clock Registration Identity

Stable Runtime registration key 为：

```text
(PluginInstance, native_id bytes)
```

同一 activation 内：

```text
same key + identical origin/resolution + same continuity generation
    → idempotent registration
    → same ClockHandle

same key + conflicting immutable clock facts
    → CONFLICT
```

不同 Plugin instance 的相同 `native_id` 不自动合并。Clock reset/restart/continuity loss 后必须生成新的 native-id generation evidence，不能用旧 key 注册成新的 ClockId；相同 resolution/origin 也不能作为跨 concrete Clock instance 的 identity merge 依据。

---

### 3.9.3 Clock Wrap

native clock wrap normalization：

> 属于 Plugin-specific responsibility。

只有 Plugin 能够根据 backend contract 明确重建连续 tick domain 时：

> 才能执行 unwrap。

Runtime：

> 不猜测 native clock wrap。

---

### 3.9.4 ClockReading Scalar Domain

Profile v0.5 冻结：

```text
ClockReading = int64 signed tick value
```

`0` 和负值在 Canonical domain 中都可以是合法 reading。Plugin 必须对 native timestamp / counter conversion 使用 checked arithmetic，不得 truncate、wrap 或把 numeric zero/negative 自动解释为 absent。

---

## 3.10 Timestamp Capability

Plugin 可以提供：

```text
native timestamp roles
+
associated native clocks
```

Runtime negotiation 决定：

```text
accepted TimestampKeySet
```

V1 Profile 的 timestamp-key capacity：

> 必须在 registration 时验证。

Plugin 不能在 Record hot path：

```text
invent new timestamp key
```

---

### 3.10.1 Mandatory Ingress Timestamp

```text
rodaq/acquisition_ingress
```

由 Runtime 写入。

Plugin：

> 不提供、不覆盖、不修改该 timestamp。

---

### 3.10.2 Optional Timestamp State

对 Binding TimestampKeySet 中每一个：

```text
Plugin-owned timestamp key
```

Plugin 必须为每条 sample 显式提供：

```text
Present(reading)
```

或者：

```text
Absent
```

Runtime：

> 不会因为 Plugin 没填写某个 Plugin-owned key 而自动补 `Absent`。

因此：

```text
omission
≠
Absent
```

如果 Plugin 缺失 Binding 要求的 timestamp state：

1. Runtime 必须在 Gate pass 前的 Admission Structural Validation 中检测；
2. submission 返回：

```text
PreAdmissionRejected
```

3. 该行为属于 Acquisition Plugin Contract violation；
4. Runtime SHOULD 产生 operational diagnostic。

缺失 state 属于：

```text
IncompleteSubmission
```

或等价诊断类别。

而以下行为：

```text
write rodaq/acquisition_ingress

supply undefined timestamp slot
```

属于：

```text
InvalidSubmission
```

或等价的越权 / 非法输入类别。

具体 diagnostic enum：

> 不在本文冻结。

但 Runtime SHOULD 能够区分：

```text
missing required state
```

和：

```text
illegal supplied state
```

不得把 missing timestamp state 延迟到 Gate pass 后处理。

---

### 3.10.3 Timestamp Capability Change

如果 timestamp key：

```text
not part of current Binding
```

Plugin 不得在 Record 中临时增加。

如果需要启用：

```text
renegotiate
        ↓
new TimestampKeySet
        ↓
new StreamDescriptor
        ↓
new Binding
```

如果 optional key 已包含在 current Binding：

> 每 Record 可以在 Present / Absent 之间变化。

---

# 4. Binding 与 Runtime Integration

## 4.1 Binding 的作用

Runtime Binding 是：

> Plugin native acquisition path 与 immutable StreamDescriptor interpretation 之间的 Runtime operational relationship。

Binding 至少固定：

```text
Stream
StreamDescriptor
PayloadEncoding
Schema association
TimestampKeySet
compiled timestamp slots
ingress slot
Plugin routing identity
```

Binding：

- 不是 Canonical object；
- 不持久化；
- 不要求 Plugin 定义同名 class。

---

## 4.2 Registration Flow

候选 native stream 的 registration 流程为：

```text
Plugin discovers native facts
        ↓
Plugin reports capabilities
        ↓
Runtime validates Profile
        ↓
Runtime resolves
existing/new Canonical objects
        ↓
Runtime registers descriptor graph
        ↓
Runtime creates Prepared Binding
        ↓
Storage descriptor readiness
        ↓
Binding Active
        ↓
Plugin may Submit
```

Plugin 不得假设：

> registration 永远创建新的 Stream 或 Descriptor。

---

## 4.3 Capability Negotiation

Plugin 提供：

```text
available PayloadEncoding
schema capability
timestamp roles
clock facts
source capability
source-sequence capability
```

Runtime 根据：

```text
V1 Profile
current Registry
Storage capability
configured policy
```

选择接受的配置。

Capability negotiation：

> 不等于 Canonical registration。

但二者可以在实现中由一个 Control Plane transaction 完成。

---

## 4.4 Binding Activation

Plugin 只能使用：

```text
Active
```

且：

```text
current
```

Binding 开始新的 normal submission。

以下 Binding 不得开始新的 Submit：

```text
Prepared
Draining
Retired
```

Runtime Admission：

> 会再次检查这一 invariant。

Plugin-side routing 正确性：

> 不能依赖 Runtime rejection 作为 normal routing mechanism。

Runtime rejection：

> 是 safety net。

---

## 4.5 Required Startup Binding

Plugin configuration 可以声明：

```text
required startup acquisition target
```

例如：

```text
required ROS2 topic
required DDS data flow
required CAN interface
```

Runtime activation 只有在：

```text
all required startup Bindings ready
```

后才能满足 acquisition startup prerequisite。

---

### 4.5.1 Required-target Discovery State

Plugin 必须通过 Control Plane 暴露：

```text
Pending
Found
NotFound
```

`Pending`：

> discovery 尚未达到可判定状态。

`Found`：

> native target 已发现，可以进入 registration。

`NotFound`：

> 当前 discovery cycle 已明确未发现目标。

Runtime 根据：

```text
configured activation deadline
required/optional policy
Plugin discovery progress
```

决定：

```text
continue waiting
or
fail activation
```

Plugin：

> 不因为一次 `NotFound` 自行触发 Runtime terminal failure。

---

## 4.6 Dynamic Binding

Running 期间已经存在的 Plugin 可以发现新的 native Stream。

Plugin 可以执行：

```text
discovery
    ↓
registration
    ↓
Prepared Binding
    ↓
Storage readiness
    ↓
Active Binding
```

无需增加 Plugin instance。

因此 V1 支持：

> dynamic Stream discovery

但不支持：

> dynamic Plugin topology。

---

## 4.7 Binding-local Cached State

Data Plane 不应反复查询 Canonical Registry。

Plugin 应缓存 Runtime 提供的：

```text
BindingHandle
SourceHandle
compiled timestamp slot mapping
PayloadEncoding decision
native routing state
```

使 callback hot path 保持简单。

---

## 4.8 Descriptor Evolution

以下变化必须产生新的 StreamDescriptor：

```text
StreamName change
Schema change
PayloadEncoding change
TimestampKeySet change
native clock continuity change
```

Plugin：

> 不得原地修改 Binding interpretation。

必须：

```text
register new descriptor
        ↓
prepare new Binding
        ↓
Binding switch
```

---

## 4.9 Binding Switch Protocol

V1 使用 quiescent two-phase Binding switch。

```text
new semantics discovered
        ↓
new Descriptor registered
        ↓
new Binding Prepared
        ↓
Storage descriptor ready
        ↓
Runtime requests
old-path quiescence
        ↓
Plugin blocks
new old-Binding routing
        ↓
Plugin reaches
ReadyToCommit
        ↓
Stream-local
submission pause
        ↓
Runtime atomically switches:
old → Draining
new → Active/current
        ↓
Runtime signals
CommitComplete
        ↓
Plugin publishes
new routing state
        ↓
new-Binding Submit resumes
```

---

### 4.9.1 ReadyToCommit

Plugin 报告：

```text
ReadyToCommit
```

时必须保证：

> **从该 point 以后，不会有新的 Plugin-side execution context 开始 `Submit(old_binding, ...)`。**

Plugin-side execution context 包括：

```text
native callback thread
Plugin internal worker
deferred submit task
Plugin-owned async task
native event dispatch task
```

已经在 ReadyToCommit 前进入：

```text
Runtime::Submit(old_binding)
```

的 synchronous call：

> 属于 Runtime-managed submission。

它可能：

```text
already Gate-passed
```

或者：

```text
still pre-admission
```

Runtime 根据 Admission Contract 收敛。

Plugin 不应为了等待所有 Runtime-owned Submit 返回：

> 与 Runtime Binding switch 建立循环等待。

---

### 4.9.2 ReadyToCommit → CommitComplete Pause Window

从：

```text
ReadyToCommit
```

到：

```text
CommitComplete
```

之间：

> **该 Stream normal submission 必须暂停。**

Plugin：

- 不得开始新的 old-Binding Submit；
- 不得开始 new-Binding Submit。

期间 native sample 使用 configured pause policy：

```text
Drop
BoundedBuffer
NativeBackpressure
```

Runtime：

> 不要求 Plugin 必须缓存这些 sample。

pause window 内错误提交到 old/new Binding：

> 不属于合法 normal path。

---

### 4.9.3 Pause Window Progress

Binding switch pause：

> 必须具有 bounded progress。

Runtime / Plugin Contract 必须最终提供：

```text
CommitComplete
```

或者：

```text
SwitchFailed / TimedOut
```

结果。

timeout 可以来自：

```text
binding_switch_timeout
```

或者 Runtime 统一 control-operation deadline。

如果 deadline 内 `CommitComplete` 未到达，

Plugin 必须：

1. 保持**受影响 Stream** normal submission paused；
2. 不恢复 old Binding；
3. 不自行启用 new Binding；
4. 向 Runtime 报告：

```text
Stream-local
Binding-switch progress failure
```

5. 将该 Stream 置为：

```text
Unavailable / SwitchFailed
```

或等价的 stream-local operational condition；
6. 继续维持其他健康 Stream 的 normal acquisition。

timed-out `prepare/commit/abort/quiesce_stream` call 在真正返回前仍视为 in-flight。Runtime/Plugin 不得对同一 transition 或同一 Stream routing state 发起重叠 mutation；如果无法等到原调用收敛，则保持目标 Stream 隔离/paused，必要时对 instance 做 quarantine。

该 failure：

> **默认不使整个 Plugin 进入 Failed。**

Runtime 决定后续：

```text
stream-local cleanup
retry switch
stream recovery
stream retirement
Plugin-wide escalation
Runtime failure cleanup
quarantine
```

只有当 failure 影响：

```text
Plugin shared routing state
Plugin shared native infrastructure
Runtime/Plugin synchronization integrity
```

时，

Runtime 才应升级为：

```text
Plugin-wide Quiesce
```

或：

```text
Plugin Failed
```

Plugin：

> 不得因为单个 Stream 的 Binding switch timeout，在 Runtime 未要求 escalation 时自行停止其他健康 Stream。

Plugin 也不得：

> 为了恢复数据而擅自回滚 Runtime Binding state。

---

### 4.9.4 CommitComplete

Runtime 完成：

```text
old Binding → Draining
new Binding → Active/current
```

以后发送：

```text
CommitComplete
```

Plugin 此后：

```text
publish new routing state
```

并恢复：

```text
new sample → new Binding
```

---

### 4.9.5 Old Binding

old Binding：

```text
Draining
```

后，

Plugin 不得开始新的 old-Binding Submit。

已经 Gate-pass 的 attempt：

> 按 Admission Contract 继续完成。

已经进入 Runtime、但尚未 Gate-pass 的 old submission：

> 可以因 final revalidation 发现 old Binding non-current / Draining 而 `PreAdmissionRejected`。

---

# 5. Data Submission、Payload Ownership 与 Backpressure

## 5.1 Submission Preconditions

Plugin 开始 normal Submit 前必须满足：

```text
Plugin Enabled

Stream operationally available

Binding Active/current

valid Payload input

complete timestamp state

registered SourceHandle
when applicable

SourceSequence valid
when present
```

Plugin 不在 Submit hot path：

```text
register Stream
register Source
register Schema
register Clock
create Binding
```

---

## 5.2 Submission Data

逻辑 submission 至少包括：

```text
BindingHandle

optional SourceHandle

optional SourceSequence

Plugin timestamp state

Payload input
```

Runtime 添加：

```text
AcceptanceSequence
rodaq/acquisition_ingress
```

并构造 RuntimeRecord。

---

## 5.3 Payload

Plugin 使用：

```text
Copy
Adopt
Retain
```

之一。

### Copy

适用于：

```text
callback-local buffer
short-lived loan
small payload
simple fallback
```

### Adopt

适用于：

> native buffer ownership 可以完整转移给 Runtime。

### Retain

适用于：

> native owner 可以给 Runtime 一个独立 lifetime anchor。

不得：

> 把普通 raw borrow 伪装成 Retain。

---

## 5.4 Native Fragmented Payload

如果 native backend 提供：

```text
scatter
fragments
segmented data
```

而 V1 Runtime Payload 需要 contiguous bytes，

Plugin 必须 flatten 为：

> exact logical byte string。

该 flatten：

> 不改变 Payload semantics。

---

## 5.5 Submission Result

Acquisition-facing Submit 的正常返回结果必须正式映射到 Plugin ABI：

```text
AdmissionOutcome
RejectionReason
PayloadDisposition
```

对应 ABI：

```text
rodaq_acquisition_submit_result
```

其中：

```text
AdmissionOutcome
    PRE_REJECTED
    REJECTED
    ACCEPTED

RejectionReason
    operational reason
    ACCEPTED 时必须 NONE

PayloadDisposition
    COPY:
        NOT_APPLICABLE

    ADOPT / RETAIN:
        CALLER_RESPONSIBLE
        or
        RUNTIME_RESPONSIBLE
```

这三个维度不能互相推导。

---

### 5.5.1 Terminal Submit Result

本文中的：

> **Terminal Submit Result**

表示 Runtime-facing Submit 已正常返回控制权给 Plugin，并且 AdmissionOutcome 已经确定为：

```text
PreAdmissionRejected
AdmissionRejected
Accepted
```

之一。

对应 ABI：

```text
RODAQ_ADMISSION_PRE_REJECTED
RODAQ_ADMISSION_REJECTED
RODAQ_ADMISSION_ACCEPTED
```

语义与 Admission Specification 完全一致。

#### PreAdmissionRejected

```text
Gate pass never happened
AcceptanceSequence not consumed
RuntimeRecord not Accepted
```

这是普通：

```text
GateClosed
ResourceUnavailable
WaitTimeout
invalid operational eligibility
```

等 pre-gate rejection 的承载结果。

#### AdmissionRejected

```text
Gate pass happened
AcceptanceSequence consumed
RuntimeRecord not Accepted
Admission/Runtime commit path no longer trustworthy
```

它不是普通 backpressure outcome。

#### Accepted

```text
Gate pass happened
AcceptanceSequence consumed
ordered Accepted publication complete
```

一旦 Accepted：

> 普通 Runtime/Storage resource pressure 不得再把该 Record 改写成 rejection。

---

### 5.5.2 Rejection Reason

RejectionReason 提供 operational diagnosis，例如：

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

但是：

> **RejectionReason 不表示 Payload ownership / anchor responsibility。**

相同：

```text
GateClosed
```

可以发生在：

```text
Adopt Transfer Point 之前
    → CallerResponsible

Adopt Transfer Point 之后
    → RuntimeResponsible
```

因此 Plugin 不得根据 rejection reason 自行推断 release responsibility。

---

### 5.5.3 PayloadDisposition

Plugin System 正式定义：

```text
NOT_APPLICABLE
CALLER_RESPONSIBLE
RUNTIME_RESPONSIBLE
```

#### Copy

```text
PayloadDisposition = NOT_APPLICABLE
```

Runtime 从不取得 Plugin original Copy input buffer 的 ownership。

#### Adopt

在 Adopt Transfer Point 之前：

```text
CALLER_RESPONSIBLE
```

越过 Transfer Point 之后：

```text
RUNTIME_RESPONSIBLE
```

一旦 RuntimeResponsible：

> ownership 不回滚。

Plugin 不得：

```text
access
release
reuse
retry with the same adopted anchor
```

#### Retain

Retain 的 disposition 描述：

> **本次 Submission 创建的 independent lifetime anchor 谁负责 release。**

它不描述 Plugin 原 native owner。

在 Runtime acquisition point 之前：

```text
CALLER_RESPONSIBLE
```

之后：

```text
RUNTIME_RESPONSIBLE
```

即使 RuntimeResponsible，Plugin 仍可以继续拥有原 native owner，但不能再次 release 同一个 submission-specific retained anchor。

---

### 5.5.4 Runtime Ownership Invariant Failure

如果 Runtime 内部发生 correctness failure，使 Runtime 已无法证明 ownership/lifetime acquisition point 是否越过：

> ownership uncertainty 不得作为 ordinary API state 转嫁给 Plugin。

只要 Runtime 仍能返回控制权：

```text
PayloadDisposition
    = RUNTIME_RESPONSIBLE
```

采用保守解释，并同时：

```text
AdmissionOutcome / RejectionReason
    表达 RuntimeFailure

Runtime
    进入 fail-closed / failure shutdown
```

如果 Runtime 无法证明 safe release：

```text
retention / quarantine
```

优先于可能 double-release 的激进 cleanup。

---

### 5.5.5 Function-level Status 与 AdmissionOutcome

Stable ABI 中：

```text
rodaq_status
```

表示 ABI/API call 本身是否可靠完成。

普通 Admission 结果必须通过：

```text
rodaq_status == OK
+
rodaq_acquisition_submit_result
```

表达。

例如：

```text
GateClosed
ResourceUnavailable
WaitTimeout
```

应该是：

```text
OK
+
PRE_REJECTED
+
corresponding RejectionReason
```

而不是函数级：

```text
RESOURCE_EXHAUSTED
TIMEOUT
```

这样：

```text
ABI failure
```

与：

```text
domain Admission outcome
```

保持分离。

如果 Runtime boundary 本身返回 non-OK：

> Plugin 必须把它视为 Runtime contract/failure path，而不是普通 backpressure。

对于 ADOPT / RETAIN，只要 Runtime 可能已经越过 transfer/acquisition point：

```text
out_result.payload_disposition
    必须仍然有效并保守为
    RUNTIME_RESPONSIBLE
```

防止 double release。

---

### 5.5.6 Invalid Normal Return

以下属于 Runtime Contract violation：

```text
ADOPT / RETAIN
+
正常返回
+
没有确定 PayloadDisposition
```

不存在合法普通状态：

```text
UnknownOwnership
MaybeTransferred
Indeterminate
```

Plugin / C++ SDK 必须：

```text
fail closed
report Runtime contract failure
```

如果 disposition 不可信但 backing 可能已转移：

```text
保守按 RuntimeResponsible 处理
```

---

### 5.5.7 Exception Boundary

Conforming Runtime-facing Submit：

> **不得让 C++ exception 穿过 Stable C ABI。**

Runtime 内部异常应被转换为：

```text
terminal domain result
or
function-level error + fail-closed path
```

C++ Plugin SDK 同样必须通过 trampoline：

```text
catch std::exception
catch (...)
```

阻止 Plugin exception 穿越 ABI。

---

### 5.5.8 Retry 与 PayloadDisposition

Retry 是新的 Submission。

#### Copy

只要 original source buffer 仍有效：

```text
可以重新构造新的 Copy Submission
```

#### Adopt

如果：

```text
CALLER_RESPONSIBLE
```

且 native sample 仍有效，可以重新构造新的 Submission。

如果：

```text
RUNTIME_RESPONSIBLE
```

Plugin 不得复用原 backing。

Retry 必须创建：

```text
new Copy
or
new Adopt
or
new Retain
```

#### Retain

如果本次 retained anchor：

```text
CALLER_RESPONSIBLE
```

Plugin 负责释放该未被 Runtime 接管的 anchor。

如果：

```text
RUNTIME_RESPONSIBLE
```

Runtime 最终释放该 anchor。

无论哪一种情况，如果 Plugin 仍拥有 native object：

> retry 都必须为新的 Submission 创建新的 independent Retain anchor。

---


## 5.6 Backpressure Selection

Plugin / configuration 可以选择：

```text
Reject
Wait
```

Admission backpressure。

Plugin 不应假设所有 native callback：

> 都可以安全阻塞。

mapping 可以根据：

```text
callback threading
native receive queue
source flow control
latency requirement
```

选择 policy。

---

## 5.7 Wait Contract

如果使用 Wait，

Plugin 不得在 Runtime Submit wait 期间持有阻碍：

```text
Plugin quiescence
Binding switch
Stop
native reader shutdown
Source state update
```

的锁。

否则可能形成：

```text
Runtime waits Plugin quiescence
Plugin callback waits Admission resource
quiescence waits Plugin lock
```

死锁。

推荐：

```text
read routing snapshot
        ↓
construct local state
        ↓
release Plugin routing lock
        ↓
Runtime::Submit
```

---

## 5.8 Reject Contract

如果 Submit 返回：

```text
PreAdmissionRejected
```

Plugin 根据 configured policy：

```text
drop native sample
count diagnostic
propagate native backpressure
bounded retry
```

之一处理。

不得：

> 把 rejected sample 当作记录成功。

对于 Adopt：

> 必须另外检查 ownership disposition。

---

## 5.9 Retry

`PreAdmissionRejected` 表示：

```text
no Admission Attempt
no AcceptanceSequence
```

但：

> 不表示 Payload ownership 一定没有变化。

如果允许 retry：

- native sample semantics 必须仍有效；
- Payload ownership/lifetime 必须重新满足 contract；
- retry 是新的 Submission；
- Runtime 重新捕获 ingress timestamp；
- retry 次数必须有界。

---

### 5.9.1 Retry after Copy

如果 original buffer 仍有效：

> 可以重新 Copy。

也可以重新构造新的 Adopt / Retain。

---

### 5.9.2 Retry after Adopt

必须检查：

```text
PayloadDisposition
```

#### CallerResponsible

Plugin 仍负责原 adopted backing。

如果 native sample 仍有效：

> 可以重新构造新的 Submission。

#### RuntimeResponsible

Plugin：

> 不得访问、release 或复用原 adopted backing。

retry 必须提供：

```text
new Copy
or
new Adopt
or
new Retain
```

---

### 5.9.3 Retry after Retain

Retain 的 disposition 只描述：

> 本次 Submit 创建的 independent lifetime anchor。

如果：

```text
CallerResponsible
```

Plugin 必须负责释放该未被 Runtime 接管的 anchor。

如果：

```text
RuntimeResponsible
```

Runtime 最终负责 release 该 anchor；Plugin 不得再次 release 同一个 anchor。

无论哪一种情况，只要 Plugin 仍持有原 native owner：

> 都可以进行新的 retry。

但 retry 必须：

> 为新的 Submit 重新创建一个新的 independent Retain anchor。

不得复用上一次 Runtime-side lifetime anchor。

---

### 5.9.4 AdmissionRejected

如果：

```text
AdmissionRejected
```

Runtime Admission path 已进入 failure semantics。

Plugin：

> 不得 retry。

---

## 5.10 SourceSequence 与 Retry

如果 retry 的确仍是：

> 同一个 native source sample，

则：

```text
SourceSequence
```

可以保持相同。

但 Runtime：

```text
rodaq/acquisition_ingress
```

重新捕获。

AcceptanceSequence：

> 只属于真正 Gate-pass 的 Admission Attempt。

---

## 5.11 Submission Concurrency

Plugin 可以：

```text
multiple callback threads
        ↓
Runtime::Submit
```

并发提交同一 Stream。

Plugin 不自行分配 AcceptanceSequence。

Runtime Admission：

> 负责同 Stream ordered commit。

---

## 5.12 No Direct Storage Access

Acquisition Plugin：

> 不直接调用 Storage Plugin。

禁止：

```text
Acquisition → MCAP
Acquisition → SQLite
Acquisition → Storage-specific queue
```

正常 data path 必须为：

```text
Acquisition
    ↓
Runtime Admission
    ↓
Runtime Dispatch
    ↓
Storage
```

---

# 6. Dynamic Evolution、Failure 与 Recovery

## 6.1 Dynamic Discovery

Running 期间 Plugin 可以发现：

```text
new native Stream
new Source
new Schema
new timestamp capability
new native clock
```

这些变化：

> 不要求重启整个 Runtime。

但必须通过：

```text
Control Plane registration
```

建立新的 Runtime state。

---

## 6.2 New Stream

```text
discover
    ↓
collect native facts
    ↓
Runtime registration
    ↓
Binding Prepared
    ↓
Storage readiness
    ↓
Binding Active
    ↓
Submit
```

Binding Active 前 sample：

> 按 early-sample policy 处理。

---

## 6.3 New Source

新的 native producer：

1. canonicalize native source identity；
2. Control Plane intern Source；
3. 获得 SourceHandle；
4. Data Plane 使用 SourceHandle。

Source registration：

> 不要求新 StreamDescriptor。

除非同时改变：

```text
schema
encoding
clock
timestamp interpretation
```

---

## 6.4 Source Loss

native Source 消失：

> Plugin 报告 Source unavailable。

Runtime 根据当前唯一 active acquisition path 内是否仍有其他 eligible Source 可以通过 current Binding 合法供数：

> 决定 Stream availability。

因此：

```text
Source unavailable
≠
Stream unavailable
```

---

## 6.5 Plugin-wide Failure

如果 Plugin shared infrastructure 整体无法继续 acquisition：

```text
Plugin reports failure
        ↓
Plugin → Failed
        ↓
all Sources owned by this Plugin unavailable
        ↓
all Streams whose unique active acquisition path
is owned by this Plugin → Unavailable
```

Plugin-wide report 必须指出：

```text
Recoverable
or
Terminal
```

单个 Stream failure：

> 不使用 Plugin-wide Failed 表达，除非影响已经扩展到 shared Plugin state。

---

## 6.6 Recovery with Same Semantics

Plugin-wide Failed 或 Stream-local unavailable 都可以在对应范围内 recovery。

只有能够证明：

```text
same logical Stream interpretation
same PayloadEncoding
same Schema
same TimestampKeySet
same native clock continuity
same Source identity
where applicable
```

时，

才可复用 current Active Binding。

Plugin-wide recovery：

```text
Plugin Failed → Enabled
```

Stream-local recovery：

> Plugin 可以始终保持 Enabled，仅恢复该 Stream。

---

## 6.7 Recovery with Changed Semantics

如果：

```text
Schema changed
PayloadEncoding changed
clock continuity lost
timestamp capability changed
logical interpretation changed
```

必须：

```text
new facts
    ↓
new registration
    ↓
new StreamDescriptor
    ↓
new Binding
    ↓
Binding switch
    ↓
resume affected Stream
```

不得继续旧 Binding。

---

## 6.8 Uncertain Continuity

如果 Plugin 无法证明：

```text
identity continuity
clock continuity
schema equality
semantic continuity
```

必须采用保守策略。

例如：

```text
uncertain Source identity
    → absent or new Source

uncertain clock continuity
    → new Clock

uncertain schema equality
    → new SchemaDescriptor /
      StreamDescriptor
```

不得：

```text
uncertain
→ assume same
```

---

## 6.9 Native Reconnection

Plugin 可以执行：

```text
socket reconnect
DDS reader recovery
ROS2 graph rediscovery
CAN interface reopen
```

native transport 恢复：

> 不自动证明 Canonical semantics continuity。

backend-specific mapping 必须定义可用于 same-semantics recovery 的：

```text
deterministic validation method
```

例如：

```text
native identity comparison

exact schema identity /
representation comparison

PayloadEncoding comparison

clock generation /
continuity evidence

timestamp capability comparison
```

默认规则：

> **无法确定性证明 same semantics，就走 changed-semantics path。**

如果某 backend-specific mapping：

> 没有定义某项 continuity 的 deterministic validation method，

则该 continuity：

```text
cannot be proven
```

必须默认：

```text
new registration /
new Binding
```

不得因为：

```text
mapping did not specify
```

而假定 continuity。

---

## 6.10 Binding Evolution

当 Plugin 检测到 descriptor-affecting change：

1. 暂停受影响 Stream normal submission；
2. 收集新 facts；
3. 注册新 Descriptor；
4. 等待 new Binding ready；
5. 执行 §4.9 Binding switch；
6. 恢复 new-Binding submission。

不得：

```text
change semantics
while continuing to Submit
through old Binding
```

---

## 6.11 Quiescence

Plugin 必须支持：

```text
Binding-local Quiescence

Plugin-wide Quiescence
```

---

### 6.11.1 Binding-local Quiescence

用于：

```text
Binding switch
```

目标：

```text
no future old-Binding
Submit can begin
```

其他 Stream：

> 可以继续 acquisition。

目标 Stream期间 sample：

```text
Drop
BoundedBuffer
NativeBackpressure
```

---

### 6.11.2 Plugin-wide Quiescence

Stable ABI 对应：

```text
rodaq.acquisition.control.v1::quiesce_plugin()
```

该调用是 instance-wide、non-destructive barrier：必须同时覆盖当前 Stream、并发 discovery 产生的新 Stream、callback/worker/deferred task 的所有 normal Submit routing；成功后仍保留 Runtime-owned Payload anchor release 所需 native capability，直到后续 destructive `stop()`。


用于：

```text
Runtime Stop
Plugin terminal failure cleanup
Runtime failure cleanup
```

目标：

```text
no future Runtime::Submit
from this Plugin can begin
```

Plugin-wide Quiescing 后：

> 不得开始任何新 normal Submit。

sample 按：

```text
Drop
BoundedBuffer
NativeBackpressure
```

处理。

不得因此建立 unbounded buffer。

---

## 6.12 Quiescence Progress

Quiescence 只建立：

```text
no future normal Runtime::Submit can begin
```

它不表示 Runtime 中所有已经进入的 Submit call 已返回；后者由 Runtime `active_submission_calls` barrier 负责。

Quiescence：

> 必须具有 bounded progress condition。

Runtime 必须最终获得：

```text
Quiesced
```

或者：

```text
Failed / TimedOut
```

结果。

Runtime：

> 不无限等待 Plugin callback 停止。

Binding switch pause：

> 同样属于 bounded control-operation progress。

---

## 6.13 Callback Quiescence

Plugin 报告 Quiesced 时必须保证：

```text
no new native callback
can enter submission routing

no Plugin worker
can start new Submit

no deferred task
can start new Submit

routing state update
visible to all submit contexts
```

已经进入：

```text
Runtime::Submit
```

的调用：

> 由 Runtime Admission shutdown / Binding-switch contract 收敛。

---

### 6.13.1 Plugin Stop Integration

正常 Stop 固定顺序为：

```text
Gate close / wake Admission waiters
→ quiesce_plugin()
→ active Submit convergence
→ Runtime / Storage drain
→ all Plugin-origin Payload anchors safely terminalized/released
→ destructive stop()
→ destroy / unload
```

Plugin-wide Quiesced 后，Runtime 仍可能正在处理：

```text
active pre-gate Submit cleanup
accepted RuntimeRecord
retained / adopted Payload lifetime
Storage drain
```

Runtime 只有在满足 Plugin Stop 所需的 Payload-anchor / native-resource safety 前置条件后，才请求 destructive `Plugin Stop`。

Plugin Stop 必须保证：

```text
no new callback / worker can reach Runtime
all native acquisition resources safely closed or retained as required
no already-transferred Runtime Payload anchor becomes unreleasable
```

Stop 必须具有 bounded progress，并终止为：

```text
Stopped
or
Failed / TimedOut
```

Stop failure / timeout 由 Runtime 升级为 terminal failure disposition；Plugin 不自行把 Runtime 标记为 Stopped。

---

## 6.14 Quiescence Failure

如果 Plugin 无法证明：

```text
no future callback / Submit
```

Runtime：

> 必须进入或保持 failure-cleanup path。

Runtime 不得销毁仍可能被 Plugin execution context 访问的：

```text
Binding state
Payload owner
Runtime callback target
Plugin-facing Runtime state
```

如果无法建立 memory-safe quiescence：

> 保留 / quarantine 相关 state 到更长 lifetime 或 process teardown。

memory safety：

> 优先于 cleanup completeness。

---

# 7. Interface 与实现约束

## 7.1 Stable ABI 与 Runtime-facing Capability Mapping

本规范不重新定义 Plugin Binary ABI；Stable ABI 由：

```text
docs/design/rodaq_plugin_system.md
```

冻结。

Acquisition Plugin core vtable 只包含：

```text
create
bind
configure
start
stop
query_interface
destroy
```

Plugin → Runtime 通过 Service Registry 获取：

```text
rodaq.runtime.common.v1
rodaq.runtime.acquisition.v1
```

其中 `rodaq.runtime.acquisition.v1` 除 registration/submit 外还正式提供：

```text
report_source_availability()
report_stream_availability()
report_plugin_state()
```

这些 operational state report 不是普通 diagnostic，不能用 logging/report_diagnostic 替代。

以及按需获取：

```text
rodaq.runtime.canonical.v1
```

本文中的领域 capability：

```text
Prepare
Discover / report facts
Register / update runtime mapping
Enable
Submit
Quiesce Binding
Quiesce Plugin
Report Failure
Report Recovery
```

映射为：

```text
configure/start/stop 内部流程
+
Runtime Acquisition Service calls
+
required rodaq.acquisition.control.v1
+
optional versioned Extensions
```

Binding switch 的 ReadyToCommit / CommitComplete / abort / stream quiescence 不得只存在于实现内部；它们必须通过 required control interface 跨 DSO 可调用。

配置通过 Runtime-owned：

```text
ConfigView
```

传入；Plugin 不得在 ABI 中暴露 `YAML::Node` 或其他 parser-specific type。

Data submission 使用正式：

```text
rodaq_record_submission
rodaq_acquisition_submit_result
```

并严格遵守：

```text
AdmissionOutcome
RejectionReason
PayloadDisposition
```

三个正交语义维度。

---

## 7.2 Control Plane 与 Data Plane

### Control Plane

```text
discovery
registration
source interning
schema registration
clock registration
Binding creation / switch
availability update
failure / recovery
quiescence
```

### Data Plane

```text
native callback
        ↓
lookup cached Binding / Source
        ↓
construct Payload
        ↓
construct timestamp state
        ↓
Submit
```

Data Plane：

> 不执行 first-time Registry mutation。

---

## 7.3 Data Plane Cached Mapping

normal callback path 应使用预先建立的：

```text
native routing state
        ↓
BindingHandle
optional SourceHandle
timestamp slot mapping
Payload strategy
```

直接 Submit。

不要求存在：

```text
NativeRoute
```

同名 class。

---

## 7.4 Error Reporting

Plugin operational error 至少需要区分：

```text
configuration failure

native discovery failure

registration failure

native source unavailable

Stream-local unavailable

Binding-switch progress failure

Plugin-wide unavailable

quiescence failure

Contract violation

internal Plugin failure
```

Plugin-wide failure 还必须指出：

```text
Recoverable
Terminal
```

Contract violation diagnostics SHOULD 至少能够区分：

```text
IncompleteSubmission
```

例如：

```text
missing required timestamp state
```

与：

```text
InvalidSubmission
```

例如：

```text
illegal timestamp slot
attempt to write ingress slot
```

具体 error enum：

> 不在本文冻结。

这些 error：

> 属于 operational Control Plane information。

不得写入普通 Canonical Record metadata。

---

## 7.5 Diagnostics

Plugin SHOULD 提供：

```text
discovered Streams

active Bindings

unavailable Streams

known Sources

native callbacks received

samples submitted

PreAdmissionRejected count

AdmissionRejected count

native samples dropped

native samples buffered

native backpressure events

retry attempts

schema changes

clock resets

source joins / losses

Binding switches

Binding-switch pause samples

Binding-switch timeouts

native reconnects

Plugin-wide failures

Stream-local failures

recoveries

quiescence duration / failure
```

diagnostic counters。

这些：

> 不改变 Canonical semantics。

---

## 7.6 Native Metadata Discipline

每一种 native metadata 都必须分类为：

```text
Stream identity evidence

Source identity evidence

Schema / Type

PayloadEncoding

Payload bytes

Timestamp / Clock

SourceSequence

operational diagnostic

unsupported structural requirement
```

如果 metadata 对正确 Canonical interpretation 必需，但现有模型无法表达：

> 必须升级 Profile / Canonical design。

不得：

```text
silently discard critical metadata
```

也不得：

```text
stuff metadata into Payload
```

绕过模型缺口。

---

## 7.7 No Generic Property Bag

V1 generic contract 不引入：

```text
PropertyBag
AnyValue
GenericMetadata
NativeObject
```

作为默认 escape hatch。

Backend-specific mapping：

> 应通过 typed mapping contract 表达。

`rodaq.runtime.projection.v1` 是这一规则的显式实现：projection 必须具有已登记的 identifier、version、独立规范和严格 size/lifetime bound；Runtime 不提供任意 key/value 查询，也不允许 Plugin 把未知 native object 塞进 projection 逃避设计审查。

只有出现明确跨 backend 的共同需求后：

> 才考虑增加新的通用 primitive。

---

## 7.8 Memory 与 Resource

Plugin 必须有界管理：

```text
native sample buffers

early-sample buffer

Binding-switch pause buffer

discovery cache

pending registration work

callback tracking

retry state
```

Runtime bounded resource：

> 不替代 Plugin 自身的 native resource bound。

Plugin 可以采用：

```text
one shared Plugin-level
sample-buffer budget
```

或者：

```text
separate bounded budgets
```

但必须同时保证：

```text
every individual buffer bounded
```

以及：

```text
aggregate Plugin-owned
retained sample memory bounded
```

不得出现：

```text
each queue bounded
but total Plugin memory
effectively unbounded
```

的配置。

Plugin 不得因为：

```text
Admission Wait
Binding switch pause
Quiescing
```

建立无界 native buffering。

---

## 7.9 Locking

Plugin 不应在 Runtime Submit 期间持有：

```text
global Plugin lifecycle lock

Binding-switch exclusive lock

quiescence completion lock
```

等会阻碍：

```text
Stop
Binding switch
failure recovery
```

的长生命周期锁。

推荐：

```text
read immutable /
atomically published
routing state
        ↓
construct local
submission state
        ↓
release Plugin lock
        ↓
Runtime::Submit
```

ReadyToCommit / Quiescence：

> 必须覆盖所有可能调用 Submit 的 execution context，而不仅是 native callback thread。

---

## 7.10 Implementation Restraint

本文中的：

```text
Discovery
Registration
Source tracking
Binding routing
Quiescence
Recovery
```

是逻辑职责。

不要求实现：

```text
DiscoveryManager
SourceManager
ClockManager
BindingManager
RecoveryManager
QuiescenceManager
```

一个合理 implementation 可以保持为：

```text
class Ros2AcquisitionPlugin

class DdsAcquisitionPlugin

class SocketCanAcquisitionPlugin
```

配合少量 private state / helper。

设计复杂度：

> 应体现在 contract 和 invariant 中，而不是机械扩张对象数量。

---

# 8. Invariants 与 Conformance

## 8.1 Acquisition Plugin Invariants

### Canonical Authority

```text
Plugin never allocates
Canonical IDs
```

---

### Lifecycle

```text
normal Submit
requires Plugin Enabled
AND Runtime Activation Commit completed
```

并且：

```text
Runtime Stopping
⇒
Plugin-wide Quiesce
⇒
Runtime submission / Payload safety barriers
⇒
Plugin Stop
```

---

### Plugin-wide vs Stream-local Failure

```text
one Stream failure
does not automatically imply
Plugin Failed
```

其他健康 Stream：

> SHOULD continue acquisition。

---

### Runtime Failed

```text
Runtime terminal Failed
⇒
Plugin recovery
cannot reopen normal Submit
```

---

### Registration

```text
no normal Submit
without registered
Active/current Binding
```

---

### Data Plane

```text
Data Plane does not perform
first-time Canonical registration
```

---

### Source

```text
SourceSequence
⇒
registered SourceHandle
```

并且：

```text
unknown / unstable Source identity
⇒
no fake SourceId
```

---

### Schema

```text
Schema change
⇒
new StreamDescriptor / Binding
```

---

### Encoding

```text
PayloadEncoding change
⇒
new StreamDescriptor / Binding
```

---

### Clock

```text
clock continuity lost
⇒
new Clock
⇒
new TimestampKeySet
⇒
new StreamDescriptor / Binding
```

---

### Timestamp

```text
Plugin never writes
rodaq/acquisition_ingress
```

每一个 Plugin-owned Binding timestamp key：

```text
Present
or
Absent
```

必须显式给出。

missing state：

```text
⇒ PreAdmissionRejected
+ IncompleteSubmission
Contract violation
```

---

### Payload

```text
Payload input obeys
Copy / Adopt / Retain contract
```

---

### Adopt / Retain Ownership

```text
AdmissionOutcome
≠
PayloadDisposition
```

并且：

```text
RejectionReason
≠
PayloadDisposition
```

正常 ADOPT / RETAIN Terminal Submit Result：

> 必须具有确定 disposition。

对于 RETAIN，该 disposition 只作用于本次 Submission 的 independent lifetime anchor，不改变 Plugin 对原 native owner 的 ownership。

---

### Ownership Failure

如果 Runtime ownership bookkeeping 无法证明：

```text
CallerResponsible
```

则对 Plugin：

```text
RuntimeResponsible
```

是保守结果。

Plugin 不处理 ordinary：

```text
Unknown ownership
```

状态。

---

### Binding

```text
new normal Submit
uses only
Active/current Binding
```

---

### Binding Switch

```text
ReadyToCommit
⇒
no future old-Binding
Submit can begin
```

以及：

```text
ReadyToCommit
→ CommitComplete

⇒
no normal old/new Binding Submit
```

pause window：

> 必须 bounded。

---

### Binding Switch Timeout

```text
one Stream
Binding-switch timeout
```

默认：

```text
affected Stream
Unavailable / SwitchFailed
```

而不是：

```text
Plugin-wide Failed
```

除非 failure 影响 shared Plugin / synchronization correctness。

---

### Quiescing

```text
affected Quiescing scope
⇒
no new normal Submit
can begin
```

---

### Retry

```text
retry
⇒
new Submission
```

Adopt：

```text
reuse original backing
only if
CallerResponsible
```

Retain：

```text
retry
⇒
new Runtime lifetime anchor
```

---

### Admission

```text
PreAdmissionRejected
does not mean
sample recorded
```

也不意味着：

```text
Adopt ownership
necessarily remained
with Plugin
```

---

### Failure

```text
Plugin / Source failure
is operational state

not Canonical Record
```

---

### Recovery

```text
reuse existing Binding
only when semantics
and continuity
can be deterministically proven
```

mapping 缺少 validation method：

```text
⇒ continuity not proven
⇒ changed-semantics path
```

---

### Quiescence

```text
Plugin-wide Quiesced
⇒
no future Submit
can begin
```

---

### Resource

```text
all Plugin-owned buffering
must be individually
and aggregately bounded
```

---

## 8.2 Required Conformance Scenarios

V1 Acquisition Plugin implementation 至少必须覆盖以下测试。

### Runtime / Plugin Lifecycle Mapping

验证：

```text
Runtime Configured
→ Plugin Configured

Runtime Starting
→ Plugin Prepare
→ discovery / registration
→ required Bindings ready
→ no normal Submit yet

Runtime Activation Commit
→ Running + Gate Open
→ Plugin Enable permission
→ Plugin Enabled
→ normal Submit

Runtime Stopping
→ Plugin-wide Quiesce
→ Runtime active_submission_calls == 0
→ required Payload / Storage safety barrier
→ Plugin Stop
→ Plugin Stopped
```

---

### Failure Cleanup Lifecycle

Plugin 或 Runtime failure。

验证：

```text
best-effort Quiesce
→ Stop when safe
```

无法 quiesce：

> memory-safe quarantine。

---

### V1 Single Active Acquisition Path / No Cross-Plugin Merge

两个 Plugin 报告相同 `StreamName`，并分别发现看起来可能表示同一外部数据流的 native facts。

验证：

- 两个 Plugin local routing state 不会因为 name 相同自行合并；
- Runtime 不执行跨 Plugin native-fact heuristic merge；
- 两个 Plugin 不会同时获得同一个 Stream 的 active acquisition path / current Binding；
- 如二者都被配置采集，则保持为独立 Stream registration。

再构造同一 Plugin 对同一 native logical flow 的 rediscovery：

> Plugin / Mapping 必须证明 continuity，Runtime 才允许继续已有 Stream mapping / Descriptor evolution。

---

### SourceId Domain Disjointness

构造两个可同时存在于同一 Scope 的 Plugin identity domain，并故意让其 native identity payload bytes 相同。

验证 Mapping canonicalization 后：

```text
SourceId bytes differ
```

或者无法保证 disjointness 的 Plugin 报告：

```text
SourceId = absent
```

Runtime 不依赖 Plugin type 额外参与 SourceId equality。

---

### Activation Commit Race

让 Plugin 在 Enable permission 发布后立即产生 callback。验证：

```text
callback may call Runtime::Submit
⇒
Runtime Running
AND Gate Open
```

不得出现 Plugin 已 Enabled 但 Runtime 仍因 `Starting` 把正常首条 sample lifecycle-reject 的窗口。

---

### Required Startup Binding

验证：

```text
Pending
→ Found
→ registration
→ Binding Active
```

---

### Required Target Not Found

Plugin：

```text
NotFound
```

Runtime 根据 configured deadline：

```text
continue waiting
or
fail activation
```

---

### Early Native Sample

Binding 未 Active 时产生 sample。

验证：

```text
Drop
BoundedBuffer
NativeBackpressure
```

之一。

---

### Buffered Early Sample Timestamp

bounded-buffer 后再 Submit。

验证：

```text
rodaq/acquisition_ingress
```

对应真正 Submit 时刻。

---

### Dynamic Stream Discovery

Running 中发现新 Stream。

验证完整：

```text
discovery
→ registration
→ Binding Active
→ Submit
```

---

### Source Registration

稳定 SourceId：

```text
Control Plane intern
→ SourceHandle
→ Data Plane use
```

---

### Registration-pending Known Source Sample

native Source identity 已可靠获得，但 SourceHandle 尚未注册：

```text
must not enter Admission
must not be rewritten as SourceId absent
```

验证 bounded buffer / backpressure / pre-admission drop-reject policy，并确认 Control Plane registration 完成后才携带 SourceHandle Submit。

---

### Truly Unknown / Unstable Source Identity

native identity truly unavailable / invalid / unstable / ambiguous：

```text
SourceId absent
SourceSequence absent
```

该 Canonical absent state 与 registration-pending known Source 严格区分。

---

### Unstable Source Identity

验证：

```text
SourceId absent
```

---

### SourceSequence

真正 producer sequence：

> 正确映射。

reader-local counter：

> 不映射。

---

### Schema Present

验证：

```text
SchemaFormat
optional TypeIdentifier
exact representation bytes
```

---

### Late Schema — Buffered

验证：

> bounded buffer + schema registration + subsequent Submit。

---

### Late Schema — Schema Absent

验证：

```text
schema-absent Binding
→ records
→ schema appears
→ new Descriptor
→ new Binding
```

---

### PayloadEncoding Change

验证：

> new Descriptor / Binding。

---

### Clock Reset

验证：

```text
new Clock
new TimestampKeySet
new StreamDescriptor
new Binding
```

---

### Clock Continuity Uncertain

验证：

> new Clock path。

---

### Timestamp Present / Absent

同一 optional key：

```text
Record A → Present
Record B → Absent
```

均合法。

---

### Missing Timestamp State

Plugin 缺失 Binding-required Plugin-owned timestamp key。

验证：

```text
PreAdmissionRejected
no AcceptanceSequence
IncompleteSubmission diagnostic
```

---

### Undeclared Timestamp

验证：

```text
PreAdmissionRejected
InvalidSubmission diagnostic
```

---

### Plugin Writes Ingress Timestamp

验证：

```text
PreAdmissionRejected
InvalidSubmission diagnostic
```

---

### Copy Payload

callback-local buffer 使用 Copy。

callback 返回后 Runtime Payload 仍有效。

---

### Adopt Payload

验证 ownership transfer 后：

> Plugin 停止访问 backing。

---

### Retain Payload

验证 independent lifetime anchor。

---

### Retain Rejected Before Anchor Acquisition

构造 Runtime 在 Retain acquisition point 前拒绝。

验证：

```text
PreAdmissionRejected
+
CallerResponsible
```

Plugin 负责 release 本次 retained anchor；原 native owner 语义不变。

---

### Retain Rejected After Anchor Acquisition

构造 Runtime 已取得本次 retained anchor 后发生 Gate/Binding rejection。

验证：

```text
PreAdmissionRejected
+
RuntimeResponsible
```

Runtime exactly-once release 本次 retained anchor；Plugin 仍可继续持有原 native owner。

---

### Invalid Retain

验证：

> fallback Copy，而非 fake Retain。

---

### Fragmented Payload

验证 flatten 后 logical bytes exact。

---

### Reject Backpressure

验证：

```text
PreAdmissionRejected
```

及 Plugin configured policy。

---

### Wait Backpressure

验证：

- Wait 可以被 cutoff 中断；
- Plugin 不持有 quiescence-blocking lock；
- resource 恢复后正常继续。

---

### Adopt Rejected Before Transfer

构造 Runtime 在 Adopt Transfer Point 前拒绝。

验证：

```text
PreAdmissionRejected
+
CallerResponsible
```

Plugin 可以安全管理原 backing。

---

### Adopt Rejected After Transfer

构造：

```text
Adopt ownership acquired
        ↓
Gate / Binding state changes
        ↓
PreAdmissionRejected
```

验证：

```text
RuntimeResponsible
```

Plugin 不访问原 backing。

---

### Same Rejection Reason, Different Ownership Disposition

构造两个：

```text
GateClosed
```

结果。

场景 A：

```text
Gate closed
before ownership acquisition
→ CallerResponsible
```

场景 B：

```text
ownership acquired
then Gate closes
→ RuntimeResponsible
```

验证：

> reason 不能推导 ownership。

---

### Invalid Adopt / Retain Disposition Return

模拟 Runtime 正常返回 ADOPT 或 RETAIN result，但 disposition 无效。

验证 Plugin：

```text
treats as
RuntimeResponsible
```

并产生：

```text
Runtime Contract violation
```

不得 release / reuse backing。

---

### Runtime Ownership Invariant Failure

模拟 Runtime 无法证明 Adopt transfer point，但仍能正常返回。

验证：

```text
RuntimeResponsible
+
RuntimeFailure
```

Plugin 不接触 backing。

---

### Exception Crossing Runtime Boundary

使用 test-only non-conforming Runtime 注入异常跨 boundary。

验证 Plugin 若重新获得控制权：

```text
assumes RuntimeResponsible
reports Runtime Contract violation
```

不释放 backing。

---

### Retry after Copy

原 buffer 仍有效时：

> 可以重新 Copy。

---

### Retry after Adopt — CallerResponsible

验证：

> 可以重新构造新的 Submission。

---

### Retry after Adopt — RuntimeResponsible

验证：

- 不访问原 backing；
- retry 使用新 Payload；
- 无 double release / UAF。

---

### Retry after Retain

验证：

> new Retain anchor。

---

### No Retry after AdmissionRejected

验证：

> Plugin 不 retry。

---

### SourceSequence on Retry

验证：

```text
same SourceSequence
new ingress timestamp
```

---

### Concurrent Same-stream Callback

验证：

> Plugin 不自行分配 AcceptanceSequence。

---

### Source Loss

同一 Stream 的唯一 active acquisition path 内存在多个 Source，其中一个 Source loss。

验证：

> 只要仍有其他 eligible Source 能通过 current Binding 合法供数，Stream 可保持 Available。

---

### Last Source Loss

验证：

```text
Stream → Unavailable
```

---

### Recoverable Plugin Failure

验证：

```text
Plugin → Failed / Recoverable
```

停止 Plugin-wide Submit，

随后 bounded recovery。

---

### Stream-local Failure

一个 Plugin 管理多个 Stream。

只令其中一个 Stream 失败。

验证：

```text
affected Stream
→ Unavailable

other Streams
continue Submit

Plugin remains Enabled
```

---

### Same-semantics Recovery

验证：

```text
continuity proven
→ reuse Binding
```

---

### Changed-semantics Recovery

验证：

```text
new registration
→ new Binding
→ Binding switch
```

---

### Missing Backend Continuity Validation

构造 backend mapping 未定义 clock continuity validation。

reconnect 后验证：

```text
continuity cannot be proven
→ new Clock / new Binding path
```

不得假设 same semantics。

---

### Terminal Plugin Failure

验证：

> 不重新进入 Enabled。

---

### Recovery after Runtime Failed

Runtime terminal Failed 后 backend 恢复。

验证：

> Plugin 不重新打开 Submit。

---

### Binding Switch

验证：

```text
Prepared
→ ReadyToCommit
→ pause
→ atomic switch
→ CommitComplete
→ new routing
```

---

### ReadyToCommit Execution-context Coverage

同时存在：

```text
native callback
internal worker
deferred task
```

验证 ReadyToCommit 后：

> 没有任何 context 可以开始 old-Binding Submit。

---

### Binding Switch Pause Sample Handling

Pause window 产生 sample。

验证：

```text
Drop
BoundedBuffer
NativeBackpressure
```

之一，

且不进入 old/new Binding normal Submit。

---

### Binding Switch Timeout — Stream-local

一个 Plugin 管理：

```text
Stream A
Stream B
```

让 A 的 Binding switch timeout。

验证：

```text
Stream A
→ Unavailable / SwitchFailed

Stream B
continues acquisition

Plugin remains Enabled
```

除非 Runtime 明确执行 Plugin-wide escalation。

---

### Binding Switch Timeout Escalation

模拟 timeout 原因来自：

```text
shared Plugin routing corruption
or
Runtime/Plugin synchronization failure
```

验证 Runtime 可以：

```text
request Plugin-wide Quiesce
or
escalate Plugin Failed
```

---

### Existing old Submit at Switch

ReadyToCommit 前已经进入 Runtime Submit。

验证：

- Gate-pass → 正常 Admission 收敛；
- pre-admission → 可以 PreAdmissionRejected。

---

### Binding-local Quiescence

只暂停一个 Stream。

验证其他 Stream 继续采集。

---

### Plugin-wide Quiescence

验证 Quiesced 后：

```text
no callback
no worker
no deferred task
```

可以开始 Submit。

---

### Quiescence Timeout

验证：

> Runtime failure cleanup，不无限等待。

---

### Quiescence Memory Safety

验证无法 quiesce 时：

> 不销毁 callback-visible Runtime state。

---

### Aggregate Buffer Bound

同时填充：

```text
early-sample buffer
Binding-switch pause buffer
other native buffers
```

验证：

```text
individual limits
+
aggregate Plugin budget
```

均不突破。

---

### Fatal Plugin Failure

验证：

```text
Control Plane failure report
affected Sources unavailable
Streams reevaluated
no fake Canonical Record
```

---

### Internal Reconnection

验证：

> reconnect 本身不证明 continuity。

---

### Dynamic Source Rediscovery

continuity 可证明：

> 恢复原 Source semantics。

不能证明：

> 新 Source registration / conservative path。

---

### No Direct Storage Call

验证：

> Acquisition data path 不直接调用 Storage Plugin。

---

### No Hot-path Canonical Registration

高频 callback 下验证 normal Data Plane 不执行：

```text
first-time Stream registration
first-time Source registration
Schema registration
Clock registration
```

---

## 8.3 V1 Acquisition Plugin Baseline

RoDAQ V1 Acquisition Plugin 的 normal path：

```text
Native Backend
        ↓
Configure
        ↓
Prepare / Discovery
        ↓
Native Facts
        ↓
Canonical Registration
        ↓
Runtime Binding ready
        ↓
Runtime Activation Commit
Running + Gate Open
        ↓
Plugin Enable permission
        ↓
Native Callback
        ↓
cached Binding / Source
        ↓
Timestamp State
+
Payload Input
        ↓
Runtime::Submit
        ↓
Admission
```

Runtime / Plugin lifecycle：

```text
Runtime Starting
    → Plugin Prepare
    → discovery / registration
    → Binding ready
    → no normal Submit

Activation Commit
    → Runtime Running + Gate Open
    → Plugin Enable permission
    → Plugin Enabled
    → normal Submit

Runtime Stopping
    → Plugin-wide Quiesce
    → Runtime active_submission_calls / Payload safety barriers
    → Plugin Stop

Runtime failure
    → best-effort Quiesce
    → safe Stop
      or quarantine
```

Plugin 永远不承担：

```text
Canonical ID allocation
AcceptanceSequence allocation
Runtime ingress timestamp
Storage persistence
```

当 concrete mapping 需要外部生态互操作信息时，Plugin 可以在 Control Plane 为 StreamDescriptor 发布 versioned typed InteropProjection；该 projection 不进入 per-record Submit hot path，也不改变 Canonical equality。

Source identity：

```text
stable native identity
+ Scope-wide disjoint domain encoding
    → canonicalized SourceId

unknown / unstable /
domain collision safety not provable
    → SourceId absent
```

Schema：

```text
immutable descriptor semantics

late schema
    → bounded buffer

or

schema-absent descriptor
    → later new descriptor
```

Clock：

```text
continuity proven
    → same Clock semantics

continuity lost / uncertain
or mapping lacks validation method
    → new Clock
    → new TimestampKeySet
    → new StreamDescriptor
    → new Binding
```

Timestamp：

```text
every Plugin-owned
Binding timestamp key

must be explicitly:

Present
or
Absent
```

Stream topology：

```text
one Stream
    → one owning Plugin / active acquisition path / current Binding
    → zero or more Sources
```

Runtime 不做跨 Plugin Stream 自动合并。

Submission：

```text
Plugin Enabled
        ↓
Stream Available
        ↓
Active/current Binding
        ↓
optional SourceHandle
        ↓
explicit timestamp state
        ↓
Copy / Adopt / Retain
        ↓
Runtime Admission
```

Payload responsibility：

```text
AdmissionOutcome
RejectionReason
PayloadDisposition
```

是三个不同维度。

对于 ADOPT，Plugin 必须只根据：

```text
CallerResponsible
or
RuntimeResponsible
```

决定原 backing 是否还能访问。

对于 RETAIN，相同 disposition 只决定本次 Submission 的 retained anchor 谁负责 release；原 native owner 的 ownership 不随之转移。

如果 Runtime ownership 状态无法证明：

```text
conservative external result
=
RuntimeResponsible
```

而不是把 `Unknown` 交给 Plugin。

Backpressure：

```text
Reject
or
Wait
```

Wait：

> 不持有阻碍 Stop / quiescence / Binding switch 的锁。

Retry：

```text
PreAdmissionRejected
        ↓
optional bounded retry
        ↓
new Submission
```

其中：

```text
Copy
    → original buffer
      may be reused
      if still valid

Adopt
    → original backing
      reusable only when
      CallerResponsible

Retain
    → new Runtime
      lifetime anchor
```

Descriptor evolution：

```text
new facts
        ↓
new Descriptor
        ↓
new Binding Prepared
        ↓
Binding-local Quiescence
        ↓
ReadyToCommit
        ↓
Stream-local Pause
        ↓
Runtime switch
        ↓
CommitComplete
        ↓
new routing
```

Binding switch timeout：

```text
one Stream timeout
        ↓
that Stream
Unavailable / SwitchFailed
```

默认：

> **不停止整个 Plugin。**

其他健康 Stream：

> 继续 acquisition。

只有 shared Plugin correctness / synchronization 受影响时：

```text
Runtime escalation
        ↓
Plugin-wide Quiesce
or
Plugin Failed
```

Recovery：

```text
same semantics
+
continuity proven
    → reuse current Binding

changed semantics
or
continuity cannot be proven
    → new registration / Binding
```

Plugin-wide Quiescing：

```text
stop new Submit routing
        ↓
bounded sample policy
        ↓
drain / disable callbacks
        ↓
Quiesced
```

Plugin resource：

```text
every buffer bounded
+
aggregate retained
sample memory bounded
```

如果 quiescence 无法证明：

```text
memory-safe
retention / quarantine
```

优先于 cleanup completeness。

最终 V1 Acquisition Plugin Contract 提供：

> **以 ROS2、FastDDS 两个 V1 required implementation 验证完整 lifecycle、discovery、identity、schema、clock、timestamp、payload、backpressure、dynamic evolution、failure/recovery、InteropProjection 和 quiescence 能力，同时保持 Contract 对未来 Generic DDS、SocketCAN 等 backend 可扩展；M3 先实现 ROS2，M4 再实现 FastDDS，并继续把单 Stream failure 与 Plugin-wide failure 解耦，使 Payload ownership 在所有正常和异常提交路径上明确、保守且 memory-safe。**

ROS2 与 FastDDS Acquisition 默认是 alternative deployment profiles。V1 不要求它们在同一 process namespace 同时激活；若底层 Fast DDS/Fast CDR dependency closure 未通过 ABI/SONAME/symbol coexistence qualification，Runtime/PluginManager 必须将 simultaneous activation 视为 unsupported combination，而不是依赖动态链接器的偶然解析结果。
