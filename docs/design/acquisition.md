# RoDAQ Acquisition Plugin 设计

| 文档属性 | 内容 |
| --- | --- |
| 文档类型 | 角色模块详细设计 |
| 状态 | V1 详细设计基线 |
| 适用范围 | 所有 RoDAQ Acquisition Plugin 的 Source、Stream、Descriptor、Record Submission、Payload、Callback 与 Quiescence 合同 |
| 上位设计 | [系统架构设计](architecture.md) |
| 公共插件合同 | [Plugin](plugin.md) |
| 关联设计 | [Pipeline](pipeline.md) · [Task](task.md) · [Storage](storage.md) |

## 1. 目标与职责边界

Acquisition Plugin 负责把外部数据源转换为 RoDAQ 可接纳的 Source、Stream、Descriptor 和 Record Submission 事实。

它负责：

- 外部协议连接、订阅或接收；
- Source/Session 识别；
- Stream 与 Stream Descriptor 事实；
- Payload 和来源时间事实；
- 向 Host 提交 Record attempt；
- Source reconnect / disconnect；
- 有界协议缓存；
- callback concurrency；
- quiesce。

它不负责：

- 分配 `acceptance_sequence`；
- 打开/关闭 Pipeline Admission Gate；
- 决定 Task Desired State；
- 直接写 Storage；
- 宣称 Record durable；
- 在 Host 拒绝后私自认为该 Record 已接纳。

## 2. 数据源、Stream 与配置合同

### 2.1 数据关系、实例与配置

**数据流关系。**

```mermaid
flowchart LR
    Source["External Source"] --> Protocol["Acquisition Backend"]
    Protocol --> Stream["Stream / Descriptor Facts"]
    Protocol --> Submit["Record Submission"]
    Stream --> Host["RoDAQ Pipeline"]
    Submit --> Host
    Host --> Result["Accepted / PreAdmission Reject"]
```

Acquisition 与 Storage 不存在直接依赖。

**Acquisition Instance 生命周期。**

公共 Plugin 生命周期之上，Acquisition 角色至少包含：

```mermaid
stateDiagram-v2
    [*] --> Created
    Created --> Configured
    Configured --> Prepared: validate source / build subscriptions
    Prepared --> Running: start callbacks
    Running --> Quiescing: quiesce
    Prepared --> Quiescing: abort
    Quiescing --> Quiescent: callbacks settled
    Quiescent --> Destroyed
```

Prepared 可以已经创建协议对象或 subscription，但 Host Admission Gate 仍可能关闭。

Running 只表示插件可以产生 callback；Record 是否被接纳仍由 Pipeline Admission 决定。

**配置合同。**

Acquisition-specific configuration 由插件定义，例如：

- source endpoint；
- subscription/topic/service；
- message/type/encoding；
- protocol QoS；
- source readiness policy；
- reconnect/backoff；
- protocol-specific bounded buffers；
- source identity policy；
- timestamp extraction；
- payload holding preference。

配置描述通过 Plugin Configuration Schema 发布。

插件语义校验必须拒绝：

- 协议不支持的组合；
- 超出声明最大记录；
- 无法形成有界资源计划的配置；
- Required Source/Descriptor 信息缺失；
- 可能造成未声明无界缓存的配置。

**Acquisition Capability。**

Acquisition Capability 至少表达：

| 能力 | 说明 |
| --- | --- |
| source identity | 是否能稳定识别实际 producer |
| source sequence | 是否提供 native sequence，以及 sequence scope 类型 |
| source timestamp | 可用时间角色和 clock domain |
| payload encoding | 交付给 Host 的字节语义 |
| max record | 最大单 Record |
| payload modes | Copy / Transfer / Retain |
| callback concurrency | 最大并发 callback |
| descriptor behavior | static / runtime change |
| reconnect behavior | 是否形成新 Source Session |
| readiness | 能否判断 source/subscription ready |

不能提供的能力必须显式缺失，不得填入伪值。

### 2.2 Source Identity 与 SourceSequence

**Source Identity。**

Data Source 表示实际 producer，不等价于逻辑 Stream。

例如同一 Topic 可能：

- 只有一个 publisher；
- publisher 重启；
- publisher 被另一个进程替换；
- 同时存在多个 publisher。

插件只有在协议事实足够时才提供稳定 `SourceId`。

无法稳定识别时：

```text
SourceId = absent
```

而不是根据地址、Topic 或时间戳伪造稳定身份。

**Source Session。**

协议重连不一定等于同一 Source Session。

如果 native sequence 或 timestamp continuity 只在连接期间成立，插件必须建立新的 source-session boundary，并把该作用域随 Record 提交。

**SourceSequence。**

SourceSequence 只有在 sequence scope 明确时才可用于顺序和缺口。

允许的 scope 示例：

- stable SourceId lifetime；
- connection/session；
- protocol producer epoch。

插件提交：

```text
native_sequence
sequence_scope
optional source/session identity
```

如果 scope 不明：

- 可以保存 sequence 作为 diagnostic fact；
- 不允许 Host 用它报告 source loss/duplicate；
- 多 producer 的本地序号不能拼成单序列。

### 2.3 Stream 与 Descriptor

**Stream。**

Stream 是 RoDAQ Task 配置中的稳定逻辑数据流。

Acquisition Plugin 不分配 `stream_id`。

Host 在 create/configure 时建立 Stream identity，并在实例创建时把已解析的 Stream 关联信息交给插件。

插件用 Host 提供的 stream handle/id 报告：

- Descriptor；
- Source；
- timestamp；
- payload。

插件不得通过“发现了一个新 native topic”无界创建 Stream。

动态发现只允许：

- 映射到预配置 Stream；
- 在明确支持的未来能力中经过有界 admission。

V1 默认采用配置驱动 Stream 集合。

**Stream Descriptor。**

Acquisition 提供 Stream 的解释事实。

Descriptor 至少可以形成：

- payload encoding；
- schema/type；
- source clock/time role；
- namespaced metadata extension。

Descriptor 不可变。

解释语义变化时，插件提出新 Descriptor，而不是原地修改旧对象。

**Descriptor Proposal。**

流程：

```text
Acquisition observes new interpretation
  ↓
propose Descriptor
  ↓
Host validates/bounds
  ↓
Storage prepare
  ↓
Pipeline Descriptor Cut
  ↓
new submissions use new descriptor generation
```

插件在 Cut 前不能把新语义 payload 标记为旧 Descriptor。

**Prepare 期间数据。**

若协议已经产生新格式数据，而 Descriptor 尚未 ready：

- 插件可使用 Resource Plan 中声明的有界 staging；
- staging 满时 PreAdmission Reject/drop with evidence；
- 不允许阻塞无限期；
- 不允许无界积累；
- 不允许错误使用旧 Descriptor。

## 3. Record 提交与 Payload 合同

### 3.1 Record Submission 与 Callback

**Record Submission Contract。**

Acquisition callback 向 Host 提交的是一次 **Record Attempt**。

最小事实包括：

- bound Stream；
- Descriptor generation/id；
- optional Source identity/session；
- optional SourceSequence + scope；
- source timestamps；
- payload view/handle；
- plugin diagnostics metadata；
- callback instance identity。

Host 返回的结果至少区分：

- Accepted；
- GateClosed；
- StaleGeneration；
- DescriptorNotReady；
- InvalidRecord；
- RecordTooLarge；
- ResourceExhausted；
- InstanceObsolete；
- Internal/Unavailable。

只有 Accepted 才表示 Record 已越过 Pipeline Admission Commit。

**Callback 线程模型。**

Acquisition 可以从多个协议线程产生 callback，但必须在 Capability/Resource Plan 中声明最大并发。

Host 依据该值预留：

- callback execution capacity；
- commit-domain concurrency；
- temporary submission metadata；
- payload accounting。

插件不得在声明 `max_callback_concurrency=N` 后产生超过 N 个同时在途 callback。

若后端无法自然限制，插件必须在内部做有界调度。

**Callback 与 Host 的重入。**

默认：

```text
Plugin callback -> Host submit
```

Host submit 不同步调用该 Acquisition instance 的控制接口。

插件 callback 不应持有会阻塞：

- quiesce；
- reconnect；
- plugin destroy；

的全局协议锁。

禁止依赖 Host submit 内部执行 Storage I/O；submit 只承担接纳路径的有界 Core 工作。

### 3.2 Payload Holding

**Payload Holding。**

**Copy。**

所有 V1 Acquisition 必须支持 Copy baseline。

插件保证 payload pointer 在 submit 调用返回前有效。

Host 若选择 Copy：

- Commit 前复制到 host-owned bounded memory；
- callback 返回后插件可立即复用/释放原内存。

**Ownership Transfer。**

若 Capability 声明支持 Transfer：

- 提供唯一 owner handle；
- 通过 `acquire()` 显式、可失败地转移 ownership；
- `acquire()` 成功后 Host 成为唯一 owner，并在最终结算时 exactly-once `release()`；
- `acquire()` 前 Reject 时 owner 仍归插件；
- `acquire()` 后但 Admission Commit 前 Reject 时 owner 已归 Host，由 Host 负责释放。

**Shared Retain。**

若 Capability 声明支持 Retain：

- 使用同一 `acquire()/release()` ABI 表达 Host 额外持有；
- callback stack 结束不影响 Host 已 acquire 的 payload；
- acquire/release 必须满足 V1 的线程安全、非阻塞要求；
- quiesce 不得使 accepted retained payload 失效。

**Payload Metadata。**

插件同时报告：

- logical payload bytes；
- known physical allocation bytes（若适用）；
- ownership mode；
- alignment/encoding constraints。

不能用 logical bytes 代替实际资源占用。

## 4. 时间、就绪与覆盖语义

### 4.1 时间与来源连续性

**Receive Timestamp。**

Host 在 callback/submit 入口尽早采集：

- monotonic；
- realtime；
- `host_boot_id`；
- current Clock Epoch。

Acquisition 可以额外提供协议来源时间。

插件不得用来源 timestamp 覆盖 Host Receive Timestamp。

**Source Timestamp。**

每个来源时间值必须带：

- time role；
- clock identity/domain；
- value；
- quality/presence；
- optional synchronization relation。

缺失时保持缺失。

不允许：

- `0` 表示 unknown；
- receive time 冒充 source time；
- 没有同步证据却声明跨 host 可比较。

**Clock/Source Discontinuity。**

插件负责报告它能够识别的 source clock discontinuity，例如：

- producer restart；
- device clock reset；
- connection/session change；
- protocol epoch change。

Host Clock Epoch 由 Core 的 realtime↔monotonic correlation 管理，不由 Acquisition 插件私自生成。

### 4.2 Readiness、Backpressure 与 Coverage

**Readiness。**

Acquisition readiness 与数据活动分离。

至少区分：

- plugin configured；
- subscription/connection established；
- source discovered/ready；
- receiving data；
- `0 Hz` activity。

“没有收到 Record”不能自动解释为 plugin failure。

Task 可配置 Required readiness policy，例如：

- subscription created 即可；
- 至少发现一个匹配 source；
- 必须在 deadline 内收到首条数据。

Readiness policy 必须在 Start Prepare 阶段可有界判定。

**Backpressure。**

Pipeline Admission 不承诺接受每个 Record Attempt。

当 Host 资源不足：

```text
PreAdmission Reject(ResourceExhausted)
```

Acquisition 必须遵守插件自己的 overflow policy。

允许：

- protocol-native drop；
- bounded plugin queue；
- bounded retry where protocol semantics allow。

不允许：

- callback 内无限等待 Host；
- unbounded queue；
- 将被 Host Reject 的 Record 报成 accepted；
- 在不记录证据的情况下重排/重复协议数据。

**Coverage Evidence。**

Acquisition 负责提供来源侧能观察到的证据：

- source disconnect/reconnect；
- protocol sequence gap；
- plugin queue overflow；
- plugin-side drop；
- invalid payload；
- source/session change；
- descriptor-not-ready rejection；
- source timestamp discontinuity。

Pipeline 负责记录 Host 侧：

- PreAdmission Reject；
- Accepted；
- post-admission failure/Unknown。

两类证据不能混成单一“丢包数”。

### 4.3 Reconnect

Reconnect 必须有明确状态和有界退避。

重连后插件重新确认：

- source/session identity；
- subscription；
- Descriptor；
- native sequence scope；
- source timestamp continuity。

不能假设重连后 native sequence 一定连续。

如果 producer identity 无法证明连续，建立新 Source/Session boundary。

## 5. 生命周期、资源与实现设计

### 5.1 Quiesce、资源与后端边界

**Quiesce。**

Host 先关闭 Admission Gate，再调用 Acquisition `quiesce`。

quiesce success 必须保证：

1. 不再产生新的 callback；
2. 所有在途 callback 已返回；
3. 后台协议执行不会再次访问该 instance 的 callback context；
4. 插件不再向 Host 提交该实例 Record；
5. host 可以安全销毁 callback target。

quiesce 不要求已接纳 Record 写完 Storage；这些 Record 已由 Pipeline 持有并继续 drain。

**Timeout。**

quiesce timeout：

- instance 保留；
- callback context 保留；
- library 保留；
- Resource Plan 占用保留；
- 后续迟返仍归旧 instance；
- 达到局部清理 deadline 后进入 process recovery。

**Acquisition Resource Plan。**

角色专有资源至少包括：

- protocol receive buffers；
- subscription count；
- source/session entries；
- internal bounded queue；
- max callback concurrency；
- threads/executors；
- socket/FD；
- TLS/protocol memory；
- max payload；
- optional staging；
- retained payload upper bound；
- discovery metadata cardinality。

ROS 2/DDS、MQTT 等中间件内部资源无法由 Core 精确计费时，列为 Plugin-declared 或 Shared/External，并进入资格测试。

**Security。**

Acquisition 配置不得直接把 secret 导出到可移植 Task Configuration。

Task 保存 credential reference。

Deployment/secure source 解析：

- password；
- token；
- certificate；
- private key。

Acquisition 实例只获得运行所需 secret material。

不得：

- 写入 Segment Manifest；
- 写入普通日志；
- 返回 HTTP；
- 保存到 Task export。

可以保存不敏感的 credential version/fingerprint 用于 `effective_config_id` 和审计。

**后端映射要求。**

具体后端文档必须说明如何映射 Acquisition 合同。

**ROS 2。**

至少说明：

- subscription type；
- serialized payload lifetime；
- QoS；
- publisher identity；
- callback executor；
- source timestamp；
- discovery；
- multiple publishers；
- shutdown/quiesce。

**MQTT。**

至少说明：

- Broker/session；
- Topic；
- QoS；
- retained；
- reconnect；
- payload；
- packet/message identity；
- callback thread；
- TLS credential。

具体实现不能重新定义通用 Accepted/Descriptor/Payload 语义。

### 5.2 内部组成与 Role ABI

**内部组成。**

Acquisition 插件内部建议至少拆分：

| 对象 | 主要职责 |
| --- | --- |
| `AcquisitionInstance` | 角色 operation table 对应的实例根对象 |
| `SourceConnection` | 协议连接/会话生命周期 |
| `StreamSubscription` | 一个预配置 Stream 的协议订阅与 Descriptor 事实 |
| `SourceTracker` | SourceId/Session/Sequence scope 判定 |
| `DescriptorPublisher` | 产生 immutable Descriptor proposal |
| `RecordDispatcher` | 把协议 callback 转换为 Host RecordAttempt |
| `PayloadAdapter` | Copy/Transfer/Retain 所有权适配 |
| `AcquisitionSnapshot` | bounded readiness/source/receive/drop 快照 |

不得让协议 client object 直接调用 Pipeline/Storage 私有对象。

**Acquisition ABI operation table。**

在公共 Plugin ABI 之上，Acquisition role table 至少包含：

```c
typedef struct rodaq_acquisition_operations {
    rodaq_abi_header_t header;

    rodaq_result_code_t (*validate_config)(
        const rodaq_config_api_t* config_api,
        rodaq_config_node_t config,
        rodaq_error_buffer_t* error);

    rodaq_result_code_t (*get_configured_capability)(
        const rodaq_config_api_t* config_api,
        rodaq_config_node_t config,
        rodaq_acquisition_capability_t* out_capability,
        rodaq_error_buffer_t* error);

    rodaq_result_code_t (*get_resource_plan)(
        const rodaq_config_api_t* config_api,
        rodaq_config_node_t config,
        rodaq_acquisition_resource_plan_t* out_plan,
        rodaq_error_buffer_t* error);

    rodaq_result_code_t (*create)(
        const rodaq_host_services_t* host,
        rodaq_config_node_t config,
        const rodaq_acquisition_create_args_t* args,
        rodaq_plugin_instance_t* out_instance,
        rodaq_error_buffer_t* error);

    rodaq_result_code_t (*prepare)(
        rodaq_plugin_instance_t instance,
        const rodaq_acquisition_prepare_args_t* args,
        const rodaq_deadline_t* deadline,
        rodaq_error_buffer_t* error);

    rodaq_result_code_t (*start)(
        rodaq_plugin_instance_t instance,
        const rodaq_acquisition_start_args_t* args,
        const rodaq_deadline_t* deadline,
        rodaq_error_buffer_t* error);

    rodaq_result_code_t (*quiesce)(
        rodaq_plugin_instance_t instance,
        const rodaq_deadline_t* deadline,
        rodaq_error_buffer_t* error);

    void (*destroy)(rodaq_plugin_instance_t instance);
} rodaq_acquisition_operations_t;
```

具体命名在 ABI Freeze 评审时可微调，但职责不能重新合并。

configured capability 至少包含固定宽度字段：

```text
environment_fingerprint
max_record_bytes
payload_mode_bits
max_callback_concurrency
source_identity_capability bits
source_sequence_capability bits
source_timestamp_role bits
descriptor_behavior bits
readiness_capability bits
```

Resource Plan 至少包含：

```text
environment_fingerprint
host_buffer_bytes
plugin_buffer_bytes
max_inflight_records
max_inflight_bytes
max_retained_payload_bytes
max_staging_records / bytes
thread_count
executor_count
fd_count
max_callback_concurrency
max_sources
```

所有 count 使用 `uint32_t`，所有 byte size 使用 `uint64_t`。Capability 与 Resource Plan 的 `environment_fingerprint` 必须一致，否则 Host 拒绝本次 Prepare。

`rodaq_acquisition_create_args_t` 是准入后的不可变实例输入，至少包含：

```text
task/pipeline/instance identity
effective_config_id
control generation
Effective Pipeline Limits
role Host Services/context
optional secret bundle
configured Stream array + stream_count
```

Stream 数组和其中的 config node/string 只在 `create()` 调用期间 borrowed。插件必须在 `create()` 返回前复制需要跨调用保存的普通配置值；不能保存 Host 的 config node、数组或临时字符串指针。

角色调用顺序冻结为：

| 调用 | 可以执行的工作 | 返回成功后保证 |
| --- | --- | --- |
| `validate_config` | 纯配置语义检查 | 配置可由本插件解释 |
| capability / plan | 读取配置及必要环境事实，不创建运行实例 | 得到可准入的确定能力/资源事实 |
| `create` | 建立本地 instance 对象和已获准的固定资源 | 不启动外部连接/订阅，不产生 callback |
| `prepare` | 建立连接/订阅、准备 Source/Descriptor、执行 readiness 前置工作 | 外部资源已建立到可以 start 的状态 |
| `start` | 启用协议接收/callback，完成配置要求的 start readiness | callback 域已工作且受声明并发上限约束 |
| Core Open Admission | **不是插件调用** | 从此之后符合条件的 Record Attempt 才可能 Accepted |

因此 `prepare/start` 都属于 Task Prepare Barrier。插件在 `start` 成功后、Core Open Admission 之前可能已经观察到 native 数据；此时 submit 得到 `GateClosed` 属于**录制覆盖尚未开始**，不能被解释为“已开始录制后的数据丢失”。正式 Coverage 从 Pipeline Open Authorization/Open boundary 建立。

### 5.3 Host Services、RecordAttempt 与 Payload ABI

**Acquisition Host Services。**

角色 Host Services 至少提供：

```c
typedef struct rodaq_acquisition_host_services {
    rodaq_abi_header_t header;

    rodaq_result_code_t (*submit_record)(
        void* host_context,
        const rodaq_record_attempt_t* attempt,
        rodaq_submit_result_t* out_result);

    rodaq_result_code_t (*propose_descriptor)(
        void* host_context,
        const rodaq_descriptor_proposal_t* proposal,
        const rodaq_deadline_t* deadline,
        rodaq_descriptor_token_t* out_token,
        rodaq_error_buffer_t* error);

    rodaq_result_code_t (*report_source_event)(
        void* host_context,
        const rodaq_source_event_t* event);
} rodaq_acquisition_host_services_t;
```

Host callback 不允许同步调用同一 Acquisition instance 的 mutable control operation。`submit_record()` 是热路径有界调用，不执行 Storage I/O；`propose_descriptor()` 可能触发 Storage prepare，因此必须从非热回调锁域调用并携带 operation deadline。`report_source_event()` 必须是有界、非阻塞或短临界区状态更新。Host 对同一 Source/Session 的重复连接状态、活动状态和连续 gap 事件进行 coalesce/计数，而不是为每次协议抖动创建永久对象；如果事件无法记录则返回结构化错误，插件把“来源证据记录失败”合并到自身 bounded diagnostic/health，而不是静默假装 Coverage 完整。Host 通过 bounded source-state/cardinality 限制防止事件风暴形成无界内存。

**RecordAttempt ABI。**

Host 对一个合法 ABI submit 调用使用独立 disposition 返回业务接纳结果：

```c
typedef uint32_t rodaq_submit_disposition_t;

enum {
    RODAQ_SUBMIT_ACCEPTED = 0,
    RODAQ_SUBMIT_GATE_CLOSED = 1,
    RODAQ_SUBMIT_OBSOLETE_INSTANCE = 2,
    RODAQ_SUBMIT_STALE_DESCRIPTOR = 3,
    RODAQ_SUBMIT_DESCRIPTOR_NOT_READY = 4,
    RODAQ_SUBMIT_INVALID_RECORD = 5,
    RODAQ_SUBMIT_RECORD_TOO_LARGE = 6,
    RODAQ_SUBMIT_RESOURCE_EXHAUSTED = 7,
    RODAQ_SUBMIT_PAYLOAD_ACQUIRE_FAILED = 8
};

typedef struct rodaq_submit_result {
    rodaq_abi_header_t header;
    rodaq_submit_disposition_t disposition;
} rodaq_submit_result_t;
```

`submit_record()` 的函数返回码表示 Host callback 本身是否成功执行；对于正常的 PreAdmission Reject，函数返回 `RODAQ_RESULT_OK`，具体原因由 `disposition` 表达。函数返回非 OK 表示 Host/ABI 级异常，Record 同样视为未接纳，插件不得把它计为 Accepted。

`rodaq_record_attempt_t` 固定表达：

```text
struct header
stream handle
descriptor token/generation
source identity presence + value
source session presence + value
source sequence presence + value + scope kind
bounded timestamp array/count
payload descriptor
bounded flags
```

时间戳数量必须有 ABI 固定最大容量或由 Effective Limits 给出明确上限；V1 不跨 ABI 使用动态 vector。

**Payload ABI。**

角色 payload descriptor 至少表达：

```c
typedef struct rodaq_payload {
    rodaq_abi_header_t header;
    uint32_t mode;
    const uint8_t* data;
    uint64_t logical_size;
    uint64_t physical_charge;

    void* owner_context;
    rodaq_result_code_t (*acquire)(void* owner_context);
    void (*release)(void* owner_context);
} rodaq_payload_t;
```

- Copy：`acquire/release` 均为空，Host 只在 submit 调用内读取；
- Transfer：`acquire()` 成功即把唯一 ownership 转移给 Host，插件随后不得再访问或释放该 owner；Host 最终 exactly-once `release()`；
- Retain：`acquire()` 为 Host 增加一份稳定持有，插件保留自己的引用；Host 最终 exactly-once `release()` 自己的持有；
- `acquire()` 可以失败，失败时 Host 返回 `RODAQ_SUBMIT_PAYLOAD_ACQUIRE_FAILED`，不越过 Admission Commit；
- 如果 Transfer acquire 已成功但 Gate 在最终 commit recheck 时关闭，Host 负责立即 release 已取得 owner，并返回对应 PreAdmission Reject；插件不能假设原 native buffer仍归自己；
- `physical_charge` 用于资源收费，无法直接测量时使用已资格验证的保守规则。

V1 要求非 Copy 的 `acquire/release` **线程安全、非阻塞且不得反调 Host/插件控制接口**；Host 可以从 callback/admission/writer 相关执行域调用。无法满足这一要求的后端必须使用 Copy。

**Stream 配置映射。**

Host 在 create/prepare 时提供固定 Stream 关联表：

```text
stream handle
stable stream_id
plugin-specific address/config
initial descriptor expectation
effective limits
readiness policy
```

插件只能对这些 Stream 提交 Record。V1 不因为协议动态发现新 native source/topic 就自动新增 RoDAQ Stream。

### 5.4 Callback、Source 与 Descriptor 执行

**Descriptor Token。**

Descriptor Proposal 成功后，Host 返回稳定 token/generation。RecordAttempt 只携带 token，不在 callback 热路径复制 Schema。

插件内部 `StreamSubscription` 保存当前 token。新 token 只有 Storage prepare 与 Descriptor Cut 成功后才成为 Active；旧 token 能明确判定 Stale。

**Callback 热路径。**

```text
receive native message
→ identify configured stream/source/session
→ collect source metadata
→ build lightweight RecordAttempt
→ host submit_record
→ account local reject/drop
→ return
```

callback 禁止执行：

- 文件 I/O；
- Task control wait；
- Storage 调用；
- DNS/reconnect；
- 无界 metadata 创建；
- 逐条重日志格式化。

**Callback 并发控制。**

插件必须明确：

```text
max_native_callbacks
max_host_callbacks
internal_queue_records
internal_queue_bytes
overflow_policy
```

如果协议天然并发高于声明上限，插件内部用有界执行器/队列收敛；不能把超额并发直接推给 Host。

**SourceTracker。**

每个 Source entry 至少保存：

```text
source identity
session identity
sequence scope
last native sequence（若可解释）
source clock identity
last activity monotonic
bounded state flags
```

entry 数量受 `max_sources_per_pipeline` 限制。达到上限后不得无界增长 map；按配置拒绝、降级 SourceId absent 或保留 bounded diagnostic。

**Reconnect 状态。**

连接型协议使用等价于：

```text
Disconnected
Connecting
Ready
Backoff
Quiescing
Quiescent
```

backoff 使用 monotonic clock，配置 initial/max delay、jitter 和 retry budget。进入 Quiescing 后不得启动新的 reconnect。

**Readiness 实现。**

Readiness 发布结构化 snapshot：

```text
Configured
TransportReady
SubscriptionReady
SourceReady
Receiving
LastReceiveAge
```

Task Prepare 只读取定义好的 readiness condition。`0 Hz` 不自动把 SubscriptionReady 改为 false。

**Descriptor Change。**

```text
observe native schema/type change
→ deduplicate proposal
→ propose_descriptor()
→ bounded wait outside callback hot lock
→ atomically replace current descriptor token
→ subsequent records use new token
```

等待期间只能使用 Resource Plan 中声明的有界 staging，或立即 drop/reject；不得挂住 callback 等待 Storage prepare。

**PayloadAdapter。**

每个 backend 必须明确回答：

```text
native payload owner
callback-return lifetime
whether move is legal
whether retain across callback is legal
release-thread restriction
physical allocation accounting
```

任何一项无法证明时，回退 Copy。

### 5.5 Shutdown、Wrapper 与开发约束

**Quiesce 实现步骤。**

```text
mark stopping
→ block new reconnect/subscription work
→ stop protocol receive callbacks
→ wait plugin callback counter == 0
→ wait role-host callbacks return
→ stop protocol worker/executor participation
→ publish Quiescent
```

quiesce success 后 callback counter 必须恒为 0，且不再访问 host callback context。

**Deadline。**

Acquisition ABI 的 `prepare/start/quiesce` deadline 都是 **operation deadline**。插件接收 Host monotonic absolute deadline，多个子步骤共享同一总预算，不能每一步重新获得完整 timeout。`create()` 不执行协议连接或等待来源 ready；它只建立本地实例资源，因此不使用长操作 deadline。真正可能阻塞的连接、订阅和 readiness 工作放在 `prepare/start`。

**Destroy。**

`destroy()` 只清理已经 Quiescent 的实例，不得首次触发 unsubscribe、等待未知 callback、执行无界 join 或把 destroy 当 quiesce fallback。

**C++ Host Wrapper。**

Core 侧 wrapper 保持项目接口：

```cpp
class AcquisitionPlugin {
public:
    Result<AcquisitionPrepared> prepare(const AcquisitionPrepareRequest&);
    Result<void> start(const AcquisitionStartRequest&);
    Result<void> quiesce(Deadline deadline);
    AcquisitionSnapshot snapshot() const;
};
```

Record submission 通过构造时注入的 role Host Services 进入 Pipeline，不向其它模块公开 mutable submit 方法。

**Backend 实现表。**

每个 backend 文档必须回答：

| 项目 | 必须回答 |
| --- | --- |
| native callback thread | 哪些线程调用、最大并发 |
| payload lifetime | callback 返回后是否有效 |
| source identity | 如何获得、何时失效 |
| source sequence | scope 是什么 |
| source time | clock domain / role |
| reconnect | 是否形成新 session |
| descriptor change | 如何检测 |
| readiness | 哪个 native 事件表示 ready |
| quiesce | 哪个 API 能证明 callbacks stopped |
| external resources | middleware/kernel 哪些不由插件精确控制 |

**Test Seam。**

Acquisition conformance harness 必须能让测试插件主动制造：

- N 线程 submit；
- callback 迟返；
- quiesce race；
- Descriptor change storm；
- source/session 高基数；
- resource-plan 与实际不符；
- acquire/release failure；
- payload lifetime violation；
- reconnect storm。

**代码组织约束。**

```text
acquisition/
  acquisition_plugin.*
  acquisition_types.*
  source_state.*
  descriptor_contract.*
  acquisition_snapshot.*

plugins/
  ros2/
  mqtt/
```

通用 `acquisition/` 不包含 ROS 2、MQTT 第三方头。

## 6. 不变量与资格

### 6.1 核心不变量

1. Acquisition submit 是 attempt，不是 accepted。
2. Acquisition 不分配 AcceptanceSequence。
3. Acquisition 不直接调用 Storage。
4. Stream 集合 V1 有界且配置驱动。
5. Descriptor 语义变化必须经过 Descriptor Cut。
6. callback concurrency 不超过声明。
7. Copy baseline 对所有 V1 Acquisition 可用。
8. 非 Copy 模式必须显式 acquire/release 且资源计费。
9. SourceSequence 只有 scope 明确时才具有 loss 语义。
10. quiesce success 后不得再有该 instance callback。
11. quiesce timeout 不允许释放 callback context。
12. reconnect 不隐式证明 source/session 连续。

### 6.2 资格验证

**通用测试 Acquisition。**

实现可控 Fake Acquisition：

- 单线程 callback；
- N 线程 callback；
- Descriptor runtime change；
- Source reconnect；
- sequence gap；
- Copy/Transfer/Retain；
- callback late return；
- quiesce hang；
- declared resource overflow。

用于验证 Host 合同，不作为生产性能证据。

**真实后端。**

每个真实后端必须验证：

- 协议连接/断开；
- 配置非法；
- source idle；
- sustained load；
- callback concurrency；
- backpressure；
- descriptor change；
- payload ownership；
- reconnect；
- quiesce；
- process restart；
- resource HWM；
- source interference。

**Source Interference。**

采集插件作为读端也可能改变源系统行为。

资格必须证明协议配置不会不可接受地反向影响机器人控制，例如：

- ROS 2 reliable history/backpressure；
- MQTT broker flow control；
- socket buffer pressure；
- discovery storms；
- duplicate subscriptions；
- reconnect storms。

发布支持矩阵记录最终生效的协议/QoS 参数，而不是只记录 `system_default` 之类未解析字符串。

### 6.3 V1 完成条件

Acquisition 角色合同完成需要：

- Fake Acquisition conformance 全通过；
- ROS 2 Acquisition 能按通用合同实现而无需修改 Pipeline 核心语义；
- Source/Sequence/Time/Payload 每个可选事实都有明确 absence 语义；
- callback 与 quiesce race 可证明；
- sustained overload 时所有队列与 metadata 有界；
- Copy 路径通过正确性资格；
- 若 Copy 性能不足，至少一个非 Copy 模式在发布前通过所有权和真实负载资格。