# RoDAQ Storage Plugin 设计

| 文档属性 | 内容 |
| --- | --- |
| 文档类型 | 角色模块详细设计 |
| 状态 | V1 详细设计基线 |
| 适用范围 | 所有 RoDAQ Storage Plugin 的 Descriptor Prepare、Batch Write、Segment、Seal、Durability、Size 与 Failure 合同 |
| 上位设计 | [系统架构设计](architecture.md) |
| 公共插件合同 | [Plugin](plugin.md) |
| 关联设计 | [Pipeline](pipeline.md) · [Task](task.md) · [Acquisition](acquisition.md) |

## 1. 目标与角色边界

### 1.1 目标与职责边界

Storage Plugin 负责把 Pipeline 已接纳的 Record 写入某种具体格式，并为 Segment 封口和独立读取提供格式级事实。

它负责：

- 声明可表示的 payload/schema；
- Descriptor/Schema prepare；
- 有界 batch write；
- confirmed prefix / unwritten / Unknown；
- 格式内部索引；
- active segment 物理大小观测；
- seal/close；
- seal reserve；
- 产物文件清单和格式级 metadata；
- Storage 内部资源与 quiescence。

它不负责：

- 决定 Record 是否被 Admission 接纳；
- 分配 AcceptanceSequence；
- 决定 Task Desired State；
- 直接发布最终 RoDAQ 产物目录可见性；
- 把 write success 等价为 durable Segment commit；
- 在 Unknown 后缀上自行重试并制造重复记录。

**Storage 与 Core 的边界。**

```mermaid
flowchart LR
    Q["Pipeline Writer"] --> B["Storage Batch Write"]
    B --> F["Format files / indexes"]
    F --> S["Storage Seal"]
    S --> C["Core Commit Lane"]
    C --> P["Committed Segment"]
```

Storage 负责格式内部写入和封口；Core Commit Lane 负责 RoDAQ 持久提交：

```text
fsync files/manifest
→ atomic publish
→ fsync parent
→ publish durable fact
```

因此：

```text
Storage write success != Segment durable commit
```

**Storage Instance 生命周期。**

```mermaid
stateDiagram-v2
    [*] --> Created
    Created --> Configured
    Configured --> Prepared
    Prepared --> Writing: first segment opened
    Writing --> Sealing: rotate/stop
    Sealing --> Prepared: segment sealed
    Writing --> Failed: write unknown/fatal
    Sealing --> Failed: seal unknown/fatal
    Prepared --> Quiescing: close instance
    Failed --> Quiescing
    Quiescing --> Quiescent
    Quiescent --> Destroyed
```

具体实现可以不使用同名状态机，但必须提供等价可证明的生命周期事实。

## 2. 配置、Descriptor 与 Batch 写入合同

### 2.1 配置与 Capability

**配置合同。**

Storage-specific 参数由插件定义，例如：

- format options；
- compression；
- chunk size；
- index options；
- file layout；
- SQLite/MCAP-specific durability options；
- max segment size；
- backend-specific buffering。

Plugin Configuration Schema 负责静态结构约束，插件负责语义校验。

必须拒绝：

- 不支持的 payload/schema；
- 不能形成可靠 size/seal contract 的配置却请求物理大小滚动；
- 资源计划超限；
- backend option 导致无法独立读回；
- 会产生未声明无界内部缓存的配置。

**Storage Capability。**

Storage Capability 至少表达：

| 能力 | 说明 |
| --- | --- |
| payload encodings | 可写入的 payload 表示 |
| schema support | 可表示的 schema/type |
| max record | 单记录上限 |
| max batch | 单批次上限 |
| descriptor prepare | 是否支持动态 Descriptor |
| physical size | 是否能报告活跃 Segment 物理大小 |
| seal reserve | 是否能给出保守封口额外空间 |
| rotation | 支持哪些滚动边界 |
| durability | flush/close/durable 能力 |
| verification | 是否提供故障后格式核验能力 |
| extension requirements | Required namespaced metadata |

不能提供物理大小可靠观测的 Storage 不得声明支持 physical-size rotation。

### 2.2 Descriptor / Schema Prepare

在任何 Record 引用新 Descriptor 前，Pipeline 必须先让 Storage 完成对应 prepare。

prepare 的目标：

- 注册 channel/topic/table/schema；
- 验证 required extension；
- 预先分配必要格式结构；
- 确认当前 Segment 可以表示该 Descriptor。

prepare success 只表示：

```text
future writes can reference this descriptor
```

不表示任何业务 Record 已写入。

**Idempotency。**

同一 `descriptor_id` 的 prepare 必须幂等。

重复调用：

- 成功后再次调用返回相同成功语义；
- 不允许产生重复冲突的格式 schema；
- 不允许改变旧 Descriptor 解释。

同 ID 不同内容必须返回 Conflict/DataLoss 类错误。

**Timeout。**

Descriptor prepare timeout 时：

- Host 不执行 Descriptor Cut；
- 新 Descriptor Record 不被接纳；
- 调用仍可能迟返时，Storage instance 和相关资源保持所有权；
- 到局部清理 deadline 后按进程恢复合同处理。

### 2.3 Batch Write 与结果语义

**Batch Write Contract。**

V1 强制支持同步有界 batch。

Host 调用：

```text
write_batch(records[0..N))
```

Storage 返回：

```text
confirmed_prefix = K

remainder =
    DefinitelyUnwritten
    OR
    Unknown
```

其中：

```text
0 <= K <= N
```

成功：

```text
K == N
remainder = none
```

**Confirmed Prefix。**

confirmed prefix 表示 Storage 能证明：

- Record 已按照当前格式顺序写入；
- 格式内部不需要 Host 再重复提交这些 Record；
- Storage 不再把这些 Record 当作“未尝试”。

confirmed prefix 不是 durable commit。

文件系统缓存、插件内部 buffer、数据库 transaction 或格式 chunk 仍可能尚未完成最终持久提交。

**Definitely Unwritten。**

Storage 只有在能够证明 Record 没有被格式写入时，才允许把后缀标记 DefinitelyUnwritten。

例如：

- 在处理第 K 条前发现参数错误；
- 在任何后缀字节写入前明确失败；
- backend transaction 能确定未执行该范围。

Host 可以把这些 Record 结算为 AcceptedKnownFailed，但默认不会把它们重新塞回当前污染上下文重试。

是否允许业务级重放属于未来额外合同。

**Unknown。**

如果 Storage 无法证明后缀到底有没有写入：

```text
remainder = Unknown
```

典型来源：

- syscall/DB/API 返回状态不足以判断部分写入；
- timeout 后调用仍在执行；
- plugin crash；
- transaction 状态不可判定；
- malformed plugin result；
- format index/data state 失去一致性证明。

Unknown 的处理：

1. 关闭 Pipeline Admission Gate；
2. 当前 Segment 标记 contaminated；
3. 不对 Unknown Record 自动重写；
4. 不把该 Segment 正常提交；
5. 保存 confirmed prefix、Unknown 范围和故障证据；
6. 进入 quarantine；
7. Coverage 标记 PersistenceUnknown。

**Payload Consumption Boundary。**

Storage write 接收 Host 借用的 Record/Payload。

同步 V1：

- write_batch 实际返回前，Host 不释放传入 payload；
- confirmed prefix 不意味着插件已经停止访问所有输入内存，除非 ABI 明确提供更早 consume boundary；
- timeout 不允许 Host 假设调用已停止；
- pending/late call 的 payload owner 保持旧 instance。

未来异步 Storage 若加入，必须分别表达：

```text
payload_consumed
persistence_result_known
```

两个独立事件。

## 3. Segment 写入与提交协议

### 3.1 Writer 与 Segment 生命周期

**Writer Lane。**

同一 Storage instance 的所有变更调用默认由一个逻辑 Storage lane 串行：

```text
prepare_descriptor
open_segment
write_batch
seal
close
```

这样 V1 不要求 Storage 自己处理多线程写入顺序。

Storage 不得从内部线程异步修改当前 format session，除非该行为完全封装且不破坏 Host 单-lane 语义。

**Segment。**

Segment 是 RoDAQ 持久提交单元。

Storage 负责一个 Segment 内的格式文件；Core 负责最终可见性。

Segment identity 至少关联：

- `segment_id`；
- segment sequence；
- `task_id`；
- `pipeline_id`；
- `instance_id`；
- `control_generation`；
- `config_version`；
- `effective_config_id`。

`segment_id` 一旦分配即不复用，即使 Segment 最终 quarantine。

**Segment Open。**

Segment 在第一条待写 Record 到达 Writer 时创建，而不是 Start 时强制创建空文件。

Open 前 Storage 已拥有：

- output staging path；
- effective format config；
- prepared base descriptors；
- bounded resource budget；
- unique segment identity。

空 Recording Instance 不需要产生空 Segment，除非后端格式合同明确要求且通过验收。

### 3.2 Rotation、Physical Size 与 Seal

**Rotation。**

触发：

- duration；
- physical size target；
- Pause/Stop；
- Clock/format policy；
- storage/backend-specific safe boundary。

Record 不跨 Segment。

rotation target 不是严格文件最大字节数；允许完成已开始的有界 Record/Batch。

**Physical Size。**

Storage 必须区分：

- logical payload bytes；
- current physical artifact bytes；
- eventual seal overhead。

如果支持物理大小滚动：

```text
reported physical size
+ worst-case bounded batch overshoot
+ conservative seal reserve
<= volume safety budget
```

必须能测量或保守证明。

不能可靠报告 physical size 时只支持时间等其它滚动条件。

**Seal Reserve。**

seal reserve 覆盖可能发生的：

- index；
- footer；
- metadata；
- compression tail；
- database finalization；
- WAL/checkpoint；
- filesystem-level backend-specific final files。

Resource Plan/Capability 必须提供上界或明确由 Deployment Profile 提供后端资格值。

不能用“平均 seal overhead”做安全容量上界。

**Seal。**

Storage seal 负责：

- 完成格式内部结尾；
- 刷新必要 format buffers；
- 生成最终格式 metadata；
- 返回实际文件清单；
- 返回物理大小；
- 返回格式校验信息；
- 停止修改该 Segment 文件集合。

seal success 之后，该 Segment 才能交给 Core Commit Lane。

Storage seal 仍不等于 RoDAQ durable commit。

**Writer Lane 与 Commit Lane。**

V1 把数据写入和最终文件持久化分开：

```text
Writer Lane:
write N
→ seal N
→ enqueue sealed N
→ open N+1
→ continue writes

Commit Lane:
sealed N
→ sync files
→ write/sync manifest
→ atomic publish
→ parent fsync
→ durable fact
```

Commit queue 必须限制：

- max pending Segment count；
- max pending physical bytes；
- execution slots；
- volume-level fsync concurrency。

达到上限时不能继续无限创建 Segment。

Pipeline 应先关闭 Admission Gate 或进入保护，再结算已接纳数据。

### 3.3 Durable Commit 与 Recovery

**Manifest 贡献。**

Storage 向 RoDAQ Segment Manifest 提供格式级事实，例如：

- storage plugin id/version；
- format version；
- file list；
- physical size；
- checksums；
- backend metadata；
- schema/channel mapping；
- required external reader information。

Core 负责合并：

- Task/Pipeline/Instance identity；
- `effective_config_id`；
- Descriptor；
- host boot/time；
- Coverage；
- Data Impact；
- commit facts。

Storage 不拥有整个 RoDAQ Manifest 的权威。

**Durable Commit。**

正常提交：

1. Storage seal success；
2. Core 验证 file list；
3. 写 RoDAQ manifest temp；
4. fsync data/metadata files；
5. fsync manifest；
6. atomic publish temp→final；
7. fsync parent directory；
8. 发布 committed fact；
9. Catalog 可见。

只有步骤 8 后普通查询才把 Segment 作为正常已保存产物。

**Commit 失败。**

**文件 sync 失败。**

- Segment 不进入正常 catalog；
- 保留 staging/quarantine 证据；
- Pipeline Data Impact 按失败范围报告；
- 不删除无法解释的文件来“清理错误”。

**rename/publish 后 parent fsync 不确定。**

恢复时不能仅凭 final path 存在就认定 durable。

进入 VerificationRequired：

- 核验文件和 manifest；
- 补齐可证明的同步；
- 若无法证明完整性则 quarantine。

**Quarantine。**

quarantine Segment：

- 不进入正常产物查询；
- 占用卷容量；
- 保留 original segment id；
- 保存 failure reason；
- 保存 confirmed/unknown coverage；
- 不能自动删除来释放空间，除非明确人工/维护策略允许并保留审计。

V1 不承诺自动修复 contaminated format。

**Recovery。**

重启扫描只恢复可证明事实。

| 事实 | 行为 |
| --- | --- |
| temp/staging only | quarantine |
| final path + complete durable evidence | catalog |
| final path + commit result missing | verify, then catalog or quarantine |
| known committed segment missing | DataLoss/alert |
| contaminated/Unknown | quarantine |
| incomplete cleanup | continue maintenance |

Storage plugin 若提供故障后 verification extension，只能作为额外证据来源，不能绕过 Core commit contract。

**Storage Quiescence。**

Storage mutating calls 默认单 lane，因此正常 close 流程：

```text
stop new write dispatch
→ wait current call return
→ seal/abort current segment
→ settle commit handoff
→ close storage instance
→ quiescent
```

如果 write/seal call timeout：

- 该调用仍拥有传入 buffer/format state；
- Storage instance 不销毁；
- 当前 Segment 视为可能 Unknown；
- 达局部清理 deadline 后进入 process-level recovery。

**Storage Resource Plan。**

至少包括：

- internal write buffers；
- compression buffers；
- chunk cache；
- DB page/WAL related bounded resources；
- threads/executors；
- FD；
- max batch；
- max record；
- max active segment files；
- physical size observation overhead；
- seal reserve；
- verification workspace（若支持）；
- any retained input upper bound。

不能把 page cache、kernel I/O queue 或设备吞吐伪装成精确插件 reservation。

**Durability Capability。**

Storage 必须明确：

- write result 表示什么；
- flush 表示什么；
- seal 表示什么；
- 是否存在 backend durability primitive；
- Core 还需要同步哪些文件；
- 哪些文件共同构成可独立读取产物。

不同 backend 不允许用同一个模糊 `flush=true` 语义。

Durability 还依赖目标文件系统/挂载语义，而不仅是 Storage 格式。V1 只对**经过资格验证的本地文件系统与挂载配置**声明上述 commit 保证。Deployment/Volume 层至少记录并验证：稳定 volume identity、文件系统类型、staging/final 同一 mount、directory fsync 可用、同目录 rename 语义以及异常断电后的恢复测试证据。网络文件系统、FUSE 或其它未验证挂载即使 API 表面支持 `fsync/rename`，也不能自动进入生产支持矩阵。

Storage `prepare()` 对当前输出卷执行低成本 capability/identity 检查；破坏上述假设的 mount/volume 变化使 Prepare 失败或进入 Protected，不静默降低 durability。

## 4. 后端、容量与运行约束

### 4.1 Backend 特定要求

**MCAP。**

后端文档至少说明：

- Channel / Schema mapping；
- Chunk；
- compression；
- Summary/index；
- physical size；
- footer/seal reserve；
- independent reader validation；
- ROS 2 profile required extensions。

**rosbag2 SQLite3。**

至少说明：

- `.db3`；
- `metadata.yaml`；
- WAL/journal；
- checkpoint/seal；
- rosbag2 standard playback；
- directory-level atomic publication；
- all required files in manifest。

具体后端不得改变通用 Unknown、Segment commit 和 Payload consumption 语义。

### 4.2 Capacity Protection

Storage 与 Volume capacity 联动。

每卷至少有：

```text
stop watermark
low watermark
recovery watermark
seal reserve
active write budget
pending commit budget
```

关系：

```text
recovery > low > stop
```

空间不足时：

- 禁止新 Segment open；
- 关闭受影响 Pipeline Admission；
- 尽力 seal 已打开 Segment；
- 保留 Required seal reserve；
- 不通过删除 quarantine 隐藏问题。

### 4.3 Observability

Storage 需要提供：

- logical write records/s；
- logical bytes/s；
- physical bytes/s；
- batch latency histogram；
- batch size；
- confirmed prefix failures；
- Unknown count；
- current segment physical bytes；
- seal latency；
- seal overhead；
- pending commit segments/bytes；
- fsync/commit latency（Core）；
- last progress time；
- plugin resource HWM。

指标不能改变写入或提交顺序。

## 5. 开发实现设计

### 5.1 内部组成与 Role ABI

**内部组成。**

Storage 角色建议拆分：

| 对象 | 主要职责 |
| --- | --- |
| `StoragePlugin` | Core 侧 C++ ABI wrapper |
| `StorageInstance` | 一个 plugin storage instance 的角色状态 |
| `DescriptorCatalog` | prepared Descriptor/schema 映射 |
| `SegmentWriter` | 当前 active Segment 的格式写入 |
| `SegmentSealer` | 格式封口、文件清单和 seal facts |
| `CommitQueue` | sealed Segment 的有界交接 |
| `SegmentCommitter` | fsync/manifest/publish/parent fsync |
| `StorageSnapshotPublisher` | write/seal/commit/resource 快照 |

具体 backend 实现只实现 role ABI；Core SegmentCommitter 不依赖 MCAP/SQLite3 类型。

**Storage ABI operation table。**

Storage role table 至少包含：

```c
typedef struct rodaq_storage_operations {
    rodaq_abi_header_t header;

    rodaq_result_code_t (*validate_config)(
        const rodaq_config_api_t* config_api,
        rodaq_config_node_t config,
        rodaq_error_buffer_t* error);

    rodaq_result_code_t (*get_configured_capability)(
        const rodaq_config_api_t* config_api,
        rodaq_config_node_t config,
        rodaq_storage_capability_t* out_capability,
        rodaq_error_buffer_t* error);

    rodaq_result_code_t (*get_resource_plan)(
        const rodaq_config_api_t* config_api,
        rodaq_config_node_t config,
        rodaq_storage_resource_plan_t* out_plan,
        rodaq_error_buffer_t* error);

    rodaq_result_code_t (*create)(
        const rodaq_host_services_t* host,
        rodaq_config_node_t config,
        const rodaq_storage_create_args_t* args,
        rodaq_plugin_instance_t* out_instance,
        rodaq_error_buffer_t* error);

    rodaq_result_code_t (*prepare)(
        rodaq_plugin_instance_t instance,
        const rodaq_storage_prepare_args_t* args,
        const rodaq_deadline_t* deadline,
        rodaq_error_buffer_t* error);

    rodaq_result_code_t (*prepare_descriptor)(
        rodaq_plugin_instance_t instance,
        const rodaq_storage_descriptor_t* descriptor,
        const rodaq_deadline_t* deadline,
        rodaq_error_buffer_t* error);

    rodaq_result_code_t (*open_segment)(
        rodaq_plugin_instance_t instance,
        const rodaq_segment_open_args_t* args,
        const rodaq_deadline_t* deadline,
        rodaq_error_buffer_t* error);

    rodaq_result_code_t (*write_batch)(
        rodaq_plugin_instance_t instance,
        const rodaq_storage_record_t* records,
        uint32_t record_count,
        const rodaq_deadline_t* deadline,
        rodaq_storage_batch_result_t* out_result,
        rodaq_error_buffer_t* error);

    rodaq_result_code_t (*get_segment_size)(
        rodaq_plugin_instance_t instance,
        rodaq_segment_size_t* out_size,
        rodaq_error_buffer_t* error);

    rodaq_result_code_t (*seal_segment)(
        rodaq_plugin_instance_t instance,
        const rodaq_deadline_t* deadline,
        rodaq_segment_seal_result_t* out_result,
        rodaq_error_buffer_t* error);

    rodaq_result_code_t (*quiesce)(
        rodaq_plugin_instance_t instance,
        const rodaq_deadline_t* deadline,
        rodaq_error_buffer_t* error);

    void (*destroy)(rodaq_plugin_instance_t instance);
} rodaq_storage_operations_t;
```

V1 mutable operation 全部由 Host 单 lane 串行。

configured capability 至少包含：

```text
environment_fingerprint
max_record_bytes
max_batch_records
max_batch_bytes
payload/schema capability bits
descriptor_prepare capability
physical_size capability
rotation capability
durability capability
verification capability
seal_reserve_bytes
max_segment_file_count
```

Resource Plan 至少包含：

```text
environment_fingerprint
plugin_buffer_bytes
compression_workspace_bytes
seal_workspace_bytes
max_internal_inflight_bytes
thread_count
executor_count
fd_count
max_batch_records
max_batch_bytes
max_active_segment_files
```

所有 count 使用 `uint32_t`，所有 byte size 使用 `uint64_t`。Capability 与 Resource Plan fingerprint 不一致时 Host 拒绝创建实例。

`rodaq_storage_create_args_t` 至少固定：

```text
task/pipeline/instance identity
effective_config_id
control generation
Effective Pipeline Limits
resolved output-volume identity
role Host Services/context
optional secret bundle
```

`create()` 只建立准入后的本地实例，不打开业务 Segment、执行长时间 fsync 或等待外部设备恢复；这些工作由带 deadline 的后续 role operation 完成。

Storage 调用顺序冻结为：

| 调用 | 责任 |
| --- | --- |
| validate/capability/plan | 实例创建前完成语义与资源准入 |
| `create` | 建立本地 instance，不创建业务 Segment |
| `prepare` | 验证受管输出位置/volume identity、初始化 backend 运行资源、证明实例具备后续打开 Segment 的条件 |
| `prepare_descriptor` | 为初始/运行期 Descriptor 建立格式解释对象 |
| Prepare Barrier Ready | `prepare` 成功且所有 Required 初始 Descriptor 已成功 prepare |
| `open_segment` | 第一条实际待写 Record 到达或后续 rotation 时创建具体 Segment |

因此一个没有任何 Record 的已启动实例不需要生成空 Segment；同时路径、权限、backend 初始化等普通可预见错误必须尽量在 `prepare` 阶段暴露，而不是拖到第一条 Accepted Record。

### 5.2 Record、Batch 与 Segment ABI

**StorageRecord ABI。**

Storage 不接收 Acquisition 原始对象，而接收 Core accepted Record 的稳定 role representation：

```text
stream identity
acceptance sequence
descriptor identity/token
receive/recording/source timestamps
source facts
payload bytes + logical size
bounded flags/extensions
```

Storage 只能读取这些事实，不得修改 Record identity 或 Payload。

**BatchResult ABI。**

BatchResult 必须是结构化结果：

```c
typedef uint32_t rodaq_storage_tail_state_t;

enum {
    RODAQ_STORAGE_TAIL_NONE = 0,
    RODAQ_STORAGE_TAIL_DEFINITELY_UNWRITTEN = 1,
    RODAQ_STORAGE_TAIL_UNKNOWN = 2
};

typedef struct rodaq_storage_batch_result {
    rodaq_abi_header_t header;
    uint32_t confirmed_prefix;
    rodaq_storage_tail_state_t tail_state;
} rodaq_storage_batch_result_t;
```

函数返回的 `rodaq_result_code_t` 是 write operation 的唯一状态码；BatchResult 只表达“哪些 Record 已确认写入、剩余部分的确定性”，不再包含第二个 status。

约束：

- `confirmed_prefix <= record_count`；
- full success：函数返回 `OK`，`confirmed_prefix == record_count`，`TAIL_NONE`；
- 函数返回非 OK 且尚未进入任何 backend write 时：`confirmed_prefix == 0`，`TAIL_DEFINITELY_UNWRITTEN`；
- 一旦 backend write 已经开始，任何非 OK 返回都必须给出有效 BatchResult；
- partial known failure：`confirmed_prefix < record_count` + `TAIL_DEFINITELY_UNWRITTEN`；
- 无法证明剩余部分未写入：`TAIL_UNKNOWN`；
- timeout 且 backend call 可能继续执行：必须视为 Unknown；
- ABI 参数本身无效导致无法安全解释输出时，Host 不读取 BatchResult，但这种错误只能发生在进入 backend write 之前；
- Host 对其它非法组合整体提升为 Unknown 并认为插件违反合同。

**Descriptor Prepare 表示。**

`rodaq_storage_descriptor_t` 至少提供：

```text
descriptor_id
stream identity
encoding
schema bytes/identity
source/time interpretation summary
bounded namespaced extensions
required-extension flags
```

Storage prepare 返回 success 前必须完成所有会导致“这个 Descriptor 根本无法写”的验证。

允许把昂贵但可延迟的格式对象创建放到 prepare 内，但必须受 operation deadline。

**Segment Open Args。**

Core 为 Storage 创建 staging location，并向 plugin 传：

```text
segment_id
instance identity
staging directory/path capability
effective storage config
known descriptor set
size/rotation limits
seal budget
```

V1 不让 Storage 自行选择最终全局目录，也不允许它绕过受管 output root 写任意位置。

文件路径参数必须是受管、规范化后的 host-provided location。

Storage 对 staging directory 的写入必须受以下路径合同约束：

- Host 提供的 staging root 是唯一允许写入根；
- plugin 产生的文件名/SealResult 只允许相对路径；
- 禁止绝对路径、`..`、空路径和路径规范化后逃逸 staging root；
- Core 在接收 SealResult 后重新验证每个文件仍位于该 Segment staging root；
- symlink/hard-link 是否允许由 V1 platform policy 明确，默认 **不允许插件通过链接引用 staging root 外文件**；
- 最终 publish 只移动/发布经过核验的受管目录，不采用插件返回的任意 final path。

实现优先使用目录句柄/受管 path helper 封装路径解析，避免每个 backend 自行拼接安全检查。

为保证目录 rename 的原子性和 crash consistency，V1 还规定：

- 每个 Segment staging directory 直接创建在对应 final Segment parent 下，使用不可冲突的临时名称；
- staging 与 final 必须位于同一文件系统/挂载点；Prepare 阶段发现无法保证时拒绝该输出位置；
- publish 使用同一 parent 内的 directory rename，不跨文件系统 copy；
- staging/final parent 的受管身份在 `effective_config_id` 中固定，运行中不能静默切换到另一 mount。

**Segment 文件所有权。**

active Segment 生命周期内：

- Storage 拥有格式文件的写入权；
- Core 拥有 staging namespace 和最终 publish 决策；
- seal success 后 Storage 永久放弃对 sealed files 的修改权；
- Commit Lane 取得 sealed files 的 durability/publish owner；
- Catalog 只读取 committed facts。

同一文件不能同时由 Storage 和 Committer 修改。

### 5.3 Writer、Rotation 与 Seal

**Write Batch Loop。**

Core wrapper 的单次 batch：

```text
build contiguous bounded record descriptors
→ establish payload borrow lifetime
→ call write_batch(deadline)
→ ABI validate BatchResult
→ record format progress
→ on success/known failure settle payloads
→ on Unknown close admission and contaminate Segment
```

Storage plugin 不能持有 records array pointer 超过同步 call return，除非未来异步扩展另行声明。

**Deadline。**

`prepare/open_segment/write_batch/prepare_descriptor/seal/quiesce` 的 deadline 都是 **operation deadline**。`get_segment_size()` 不允许执行可能长期阻塞的 durable I/O；它返回 Storage 在正常写路径维护的已知物理大小/保守观测值。

Storage backend 内的多子步骤共享一个 absolute monotonic deadline。

如果 backend API 不支持取消：

- Host deadline 到期后停止等待/标记 timeout；
- 实际 backend call 可能仍执行；
- instance、payload、format state 和 execution slot 继续归旧 call；
- 不得同时对同一 instance 发新的 mutable call。

因此 timeout 后该 Storage lane 进入 PendingCleanup/Failed，不是简单继续下一 batch。

**SegmentWriter。**

SegmentWriter 是 Core/Pipeline 私有编排对象，至少维护：

```text
segment_id
segment sequence
active descriptor set
logical records/bytes
reported physical bytes
rotation state
contamination state
last storage progress
```

SegmentWriter 不执行最终 fsync/publish。

**Rotation 判定。**

每次 batch 前后根据：

```text
duration target
reported physical size
max batch overshoot
seal reserve
stop/pause request
clock/format boundary
```

判断是否 seal。

必须确保：

```text
current physical
+ worst-case next accepted batch/record overshoot
+ conservative seal reserve
<= hard volume safety limit
```

如果无法保证，提前停止接纳，而不是写到硬边界后再发现无法 seal。

**SealResult ABI。**

Seal result 至少返回：

```text
file count
bounded file descriptors/names
final physical sizes
format metadata/checksum contribution
seal overhead
backend verification facts
```

file list 不能是插件动态堆 vector 跨 ABI 返回。使用 caller-owned bounded output，或两阶段 required-capacity + fill。

Seal success 后 Storage 不再修改这些文件。

**SealedSegment。**

Writer → Commit Lane 的内部 move-only handoff 建议包含：

```text
segment identity
staging directory
sealed file list
storage manifest contribution
descriptor set
coverage/write outcome
logical/physical sizes
effective_config_id
commit reservation token
```

handoff 完成后 Writer 不再拥有 sealed file publish 权。

### 5.4 Commit、Manifest 与 Recovery

**CommitQueue。**

CommitQueue 在 Prepare 时固定：

```text
max_pending_segments
max_pending_physical_bytes
max_commit_jobs
```

**Commit capacity 必须在 Segment open 前预留，而不是 seal 后再抢。** 每个将要成为 active 的 Segment 先取得 move-only `CommitReservation`：

```text
1 pending-segment token
+
segment_hard_physical_limit + conservative seal reserve
```

只有 reservation 成功后才允许 `open_segment`。因此当前 Segment 完成 seal 后，把 SealedSegment + reservation 交给 CommitQueue 是正常情况下不可因“queue full”失败的 handoff。

seal 返回实际 physical bytes 后：

- actual <= reserved：释放多余 byte reservation；
- actual > reserved：视为 Storage size/seal-reserve 合同违约，立即关闭 Admission 并进入保护/故障；已经产生的文件仍由原 Segment owner 结算，不能丢弃；
- Commit terminal 后归还该 Segment 的剩余 reservation。

当 Segment N 已 sealed/commit pending，而 Writer 想打开 Segment N+1 时，必须先取得新的 CommitReservation。若没有容量，Pipeline 关闭或保持 Admission Gate 保护状态，等待已有 Commit 释放容量；不得在没有 reservation 的情况下继续创建新 Segment，也不得把 sealed 文件无界堆在 staging 目录。

Commit byte budget 与卷级实际空间预算是不同层次：前者限制进程内 pending commit backlog，后者保证磁盘还有写入和封口空间，两者都必须满足。

**SegmentCommitter。**

Committer 的正常路径：

```text
validate sealed file list
→ build RoDAQ manifest temp inside staging
→ fsync required data files
→ fsync format metadata files
→ write/fsync manifest temp
→ rename manifest temp to final manifest name inside staging
→ fsync staging directory
→ atomic rename staging directory to final Segment name in same parent
→ fsync Segment parent directory
→ publish in-memory/rebuildable committed fact
```

每一步都产生内部 progress marker/diagnostic，但 **durable commit linearization point 是 final directory rename 后对应 parent directory 的 fsync 成功返回**。最后的 committed fact/Catalog publish 是可重建内存或索引事实，不再引入第二个 durability 边界。若 rename 已成功但 parent fsync 结果未知，则进入 VerificationRequired，不能直接报告 Committed。

**Segment Manifest 字段合同。**

RoDAQ Segment Manifest 是 Core-owned Strict JSON。Storage 只提供其中的 `storage` / `files` / format contribution，不能自行生成另一份权威 Manifest。

字段至少包括：

| 字段 | 类型 | 语义 |
| --- | --- | --- |
| `format_version` | string | Manifest 格式版本 |
| `segment_id` | string | 永不复用的 Segment 身份 |
| `task_id` / `pipeline_id` / `instance_id` | string | 运行归属 |
| `segment_sequence` | decimal string | Pipeline/实例内产物顺序事实，不作为 Record Identity |
| `control_generation` | decimal string | 创建该 Segment 的控制代次 |
| `config_version` | string | Task 不可变配置版本 |
| `effective_config_id` | string | 本实例完整运行合同身份 |
| `acquisition` / `storage` | object | 精确 plugin id/version/build/environment fingerprint |
| `descriptors` | bounded array | 本 Segment 实际引用的 Descriptor 完整解码事实 |
| `files` | bounded array | 受管相对路径、物理大小、checksum/format role |
| `record_coverage` | bounded object/array | 每 Stream sequence/time coverage 与 known/unknown outcome |
| `host_boot_id` / `clock_epochs` | string / bounded array | 接收时间解释证据 |
| `logical_bytes` / `physical_bytes` | decimal string | 统计与容量事实 |
| `commit` | object | seal/commit 结果和必要 durable evidence version |
| `content_digest` | string | Manifest 自身规范化内容摘要 |

所有 64 位计数、sequence 和纳秒时间使用十进制字符串，避免 JSON/JavaScript 精度差异。`files[].path` 必须是已验证的受管相对路径。Manifest 不保存 secret、credential material 或无法长期解释的内存地址/native handle。

Manifest 的 `descriptors` 必须足以让独立 reader 在不依赖当前运行插件内存状态的情况下解释该 Segment；只保存 `descriptor_id` 而不保存对应 schema/encoding 事实是不够的。

**Crash Recovery 边界。**

恢复不能依赖“最后执行到了哪行代码”，而依据文件系统与持久事实。

需要能识别：

```text
staging only
sealed but unpublished
final path + manifest
final path + uncertain parent sync
quarantine
known committed fact
```

Core recovery scanner 只输出结构化 Evidence，Catalog 决策遵循本章前述 **Recovery** 规则，不从目录时间戳或未验证的文件存在性推断 Committed。

**Commit 并发。**

同 Pipeline 可以 Writer 与 Commit 并行，但：

- 同一个 Segment 只有一个 Commit owner；
- 同卷 Commit concurrency 有硬上限；
- 不能多个 thread 对同一 parent directory 无限制 fsync；
- Pause/Stop 等待该 instance 所有 pending Commit terminal；
- Reset 必须在所有相关 commit/read owners 静止后执行。

### 5.5 Wrapper、后端与开发约束

**Storage C++ Wrapper。**

Core 侧 wrapper：

```cpp
class StoragePlugin {
public:
    Result<void> prepare(const StoragePrepareRequest&, Deadline);
    Result<void> prepare_descriptor(const StreamDescriptor&, Deadline);
    Result<void> open_segment(const SegmentOpenRequest&, Deadline);
    StorageBatchResult write_batch(const StorageBatch&, Deadline);
    Result<SegmentSize> segment_size() const;
    Result<SegmentSealResult> seal_segment(Deadline);
    Result<void> quiesce(Deadline);
};
```

Wrapper 负责 ABI 校验、Status 映射、deadline/latency metrics 和 error buffer，不让 Pipeline 直接接触 C function table。

**Storage Backend Adapter。**

具体 backend 至少分离：

```text
role ABI adapter
backend writer
format/schema mapper
backend config
```

例如 MCAP backend 可以有内部 writer/channel/schema 组件，但这些类型不得泄漏到通用 Storage wrapper。

**SQLite3/WAL 额外约束。**

SQLite backend 详细实现必须明确：

- transaction boundary；
- journal/WAL mode；
- checkpoint timing；
- seal 时哪些文件仍 required；
- crash 后如何证明 DB 可独立读取；
- metadata.yaml 与 DB publication 的一致性边界。

不能只测试正常 close 后能播放。

**MCAP 额外约束。**

MCAP backend 详细实现必须明确：

- Schema/Channel identity mapping；
- chunk target；
- compression workspace；
- index/summary/footer；
- seal reserve 计算；
- writer close 的内存/文件访问终止；
- ROS 2 profile extensions；
- independent MCAP reader 验证。

**Failure Mapping。**

Storage wrapper 将后端错误映射为：

| 后端事实 | Core Data Impact |
| --- | --- |
| prepare 在任何 Record 使用前失败 | None / PreAdmissionOnly |
| batch tail definitely unwritten | AcceptedKnownFailed |
| batch tail unknown | PersistenceUnknown |
| seal failure with known no publication | AcceptedKnownFailed 或 ArtifactQuarantined |
| commit uncertainty | ArtifactQuarantined / PersistenceUnknown |
| committed file later missing/corrupt | HistoricalArtifactLostOrCorrupt |

映射依据结构化事实，不解析 message string。

**Quiesce 与 Destroy。**

Storage quiesce：

```text
prevent new mutable calls
→ wait current write/prepare/seal call end
→ settle/abort active Segment according to known result
→ stop backend workers
→ prove no file/payload access
→ Quiescent
```

`destroy()` 只释放 Quiescent 实例，不负责首次 flush/seal。

**Test Seam。**

Storage 详细实现需要：

- fake filesystem sync/publish layer；
- fault injection at each commit syscall；
- Fake Storage plugin with controlled prefix/Unknown；
- fake volume capacity；
- deterministic Commit executor；
- controlled slow fsync；
- canary payload；
- file-list corruption injection。

这样 Segment/Commit 单元测试不依赖真实 MCAP。

**代码组织约束。**

```text
storage/
  storage_plugin.*
  storage_types.*
  segment_writer.*
  segment_sealer.*
  sealed_segment.*
  commit_queue.*
  segment_committer.*
  storage_snapshot.*

plugins/
  mcap/
  sqlite3/
```

通用 `storage/` 不包含 MCAP/SQLite3 第三方头；文件系统持久提交能力可以形成明确的 platform/storage support 文件，但不放入宽泛 `utils/` 或 `detail/`。

## 6. 不变量与资格

### 6.1 核心不变量

1. Storage 不决定 Admission。
2. write success 不等于 durable commit。
3. batch 结果只允许 confirmed prefix + DefinitelyUnwritten/Unknown。
4. Unknown 不自动重写。
5. Unknown Segment 不进入正常 catalog。
6. payload 在 Storage 可能访问期间保持有效。
7. mutating Storage calls V1 单 lane。
8. Descriptor 在 Record 使用前必须 prepare。
9. seal success 后 Storage 不再修改该 Segment 文件集合。
10. pending commit count/bytes 有硬上限。
11. final path 存在不自动等于 committed。
12. quarantine 保留原身份和故障证据。
13. Storage plugin 不拥有 Task Desired State。
14. Core Commit 是正常产物可见性的唯一边界。

### 6.2 资格验证

**Fake Storage Contract。**

可控 Storage 覆盖：

- full success；
- K-prefix + DefinitelyUnwritten；
- K-prefix + Unknown；
- malformed K；
- write hang；
- seal hang；
- descriptor prepare fail；
- wrong physical size；
- resource plan mismatch；
- late return。

**Crash Matrix。**

对以下位置注入 process termination：

- batch before/after backend write；
- seal before/after footer；
- manifest write；
- file fsync；
- manifest fsync；
- rename；
- parent fsync；
- catalog publish。

恢复后每个 Segment 只能归入：

```text
Committed
Quarantined
VerificationRequired
Missing/Corrupt
```

不能凭猜测进入 Committed。

**Real Backend。**

真实 MCAP/SQLite3 必须验证：

- independent reader；
- standard playback where applicable；
- descriptor evolution；
- rotation；
- physical size；
- compression；
- disk full；
- I/O error；
- slow fsync；
- long-run commit backlog；
- crash recovery；
- exact file set；
- resource HWM。

### 6.3 性能资格

同步 Batch 是 V1 baseline，必须直接参加真实 ROS 2→MCAP 性能资格。

如果同步 Storage：

- queue/commit lag 不能达到有界稳态；
- CPU/latency 不满足生产画像；
- Pause/Stop convergence 超出预算；

则 V1 发布前引入经过独立设计的异步 Storage extension。

不能用更大的无界 queue 掩盖同步路径不合格。

### 6.4 V1 完成条件

Storage 角色合同完成需要：

- Fake Storage conformance 全通过；
- MCAP 实现无需修改 Pipeline 核心语义；
- Unknown 和 timeout 资源所有权可证明；
- physical-size/seal-reserve 合同通过真实数据验证；
- Writer/Commit backlog 在峰值负载达到有界稳态；
- crash matrix 没有“路径存在但无法判断是否 committed”的未定义恢复；
- 独立读回与标准工具互操作通过；
- 72h/168h 资格中文件数、FD、RSS、pending Segment 和 commit lag 无未解释增长。