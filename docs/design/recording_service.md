# RoDAQ Recording Service 与长期运行架构设计

**状态：目标设计规范（非插件；尚未实现）**

**范围：** 本文定义 DAQ Runtime 之外的录制任务、Artifact 生命周期、磁盘准入、崩溃恢复、健康监督与生产资格验证。现有 V1 的 Canonical、Admission、Payload、Plugin ABI 和 Storage 一致性契约继续有效；本文**不表示**对应服务/API 已存在，也不通过应用层推断修改 Storage Plugin 的成功语义。

**规范关系：** `rodaq_v1_system_design.md` 定义跨模块约束；`architecture.md` 定义职责分层；`runtime.md`、`admission.md`、`canonical_profile.md` 定义不可绕过的底层不变量；本文件是 Recording Service / Production Operational Plane 的具体规范。所有需要扩展 Storage ABI 的条款必须在后续专门决策中同步变更，不得直接实现成私有绕过接口。

---

## 1. 目标、能力分级与系统边界

### 1.1 工业生产目标

长期运行系统必须同时满足以下五种相互独立的性质：

- **Safety：** 错误不被伪装成成功；未证明 no-future-access 前不得释放由 Plugin 使用的资源；历史 Artifact 不被误删、重写或隐式合并。
- **Liveness：** 每一受监督的工作都有进展期限，失去进展时可以超时、故障终止或交给外部监督处理；不得以无界等待声称“可靠”。
- **Durability：** 明确区分写入调用成功、Backend PersistenceSucceeded、NormalComplete 和断电后仍存活的 Durability；RPO 必须由可执行的刷盘策略证明。
- **Availability：** 进程、Plugin 或设备故障后的恢复策略、可恢复边界、重启预算和最大允许服务中断须独立于 Runtime 的内存安全保证。
- **Observability：** 用户可以判断系统是否录制、是否取得数据、是否出现未知丢失、剩余容量、故障等级和 Artifact 完整性，不能以“进程存活”等同于“录制健康”。

上述性质以目标环境、目标负载、故障模型和 SLO 衡量；不存在不附带条件的“绝不丢数据”承诺。

### 1.2 两种部署 Profile

| Profile | 合同 | 当前核心能力 | 生产准入 |
| --- | --- | --- | --- |
| `BoundedRecording` | 有最大任务时长、容量预算和明确结束点的一次或多次独立录制 | 一次 Activation/Identity Scope → 一个逻辑 Artifact；允许顺序停止并创建下一任务，存在可测量切换间隙 | 可以在当前 Runtime + 已验证 MCAP Storage 上实现，但任务服务、容量治理、恢复和观测仍须开发 |
| `ContinuousRecording` | 长期无人值守、持续采集、由容量策略自动形成可管理分片 | **现有 V1 不支持无缝 rolling、跨 Artifact continuity 或不中断切换** | 不得标记支持，直到跨 Segment 合同、Storage 能力、故障恢复和回放资格验证通过 |

`BoundedRecording` 的服务进程可以 7×24 小时在线，并按需运行多个任务；这不等于 7×24 小时无间断录制。多个 Activation 之间的间隙是明确的**未覆盖区间**，不能假装是连续数据。`ContinuousRecording` 不应通过在控制平面高频 Stop/Start 来冒充无缝分片。

### 1.3 非目标

本设计不重新分配 Canonical ID，不跨 Scope 复用 AcceptanceSequence，不修改 Runtime Admission Commit，不让 Service 直接调用 Storage Plugin，不在 Runtime 内实现任务数据库/文件删除，不引入 Plugin 多进程隔离，不规定 Web UI 的框架实现。网络 API、认证和实时下载属于后续 Server/Application 实现，但必须满足 §6 的接口边界。

---

## 2. 组件职责与数据流

```text
External Operator / REST / CLI
              │ command / status
              ▼
       Recording Service                 systemd / external supervisor
       ├─ Task Lifecycle                         │
       ├─ Durable Task Ledger                    ├─ process health
       ├─ Artifact Catalog                       └─ restart / alert
       ├─ Disk Admission / Retention
       └─ Health & Alert Policy
              │ DAQ public control / snapshots
              ▼
          RoDAQ DAQ
       ├─ Activation & Canonical
       ├─ Admission & Payload
       └─ Ordered Storage handoff
              │
              ▼
       MCAP Storage Plugin
              │
              ▼
     Backend Artifact on disk
```

- **DAQ Runtime：** 只负责一次 Activation 内的数据/控制平面不变量，以及已提供的只读快照、错误信息和 Stop/Failure disposition。Service 不持有 Runtime 私有句柄或 Plugin ABI 指针。
- **Recording Service：** 拥有任务状态、目标目录分配、任务级幂等性、持续性意图、容量准入、任务观察、外部 API 编排和跨 Activation 的 Artifact 索引。
- **Storage Plugin：** 拥有 Backend 文件打开/写入/flush/close、Payload/Persistence completion 和真实 NormalComplete 判定。Service 不能根据文件名、大小或 DAQ Stop API 返回值独立捏造 NormalComplete。
- **Process Supervisor：** 进程级启动、Watchdog、超时处置、重启退避和熔断。进程重启并不恢复原 Activation 或原有内存中的 Canonical State。
- **Artifact Reader/Export：** 以只读方式消费已封口或被明确允许读取的产物。读写并发资格验证、访问租约和跨文件组合属于 Service/Reader，不由热路径负责。

这些名称是**逻辑职责**，并非要求为每项增加 `Manager`、`Controller` 或独立线程。首次实现优先一个服务进程、一个 durable task ledger 与一个只读 catalog 视图，直到并发任务和故障模型证明拆分有必要。

### 2.1 Identity 与资源域

- `TaskId`：Application 分配的跨进程、跨重启持久身份；不得与 Canonical StreamId/ScopeId 混用。
- `ActivationRef`：任务对一次 DAQ Activation 的引用，只用于编排与诊断；其 Canonical Scope 由 Runtime 独立分配。
- `ArtifactRef`：目录/文件集合及其校验、状态、backend mapping 和所属 Activation 的索引；不得将路径字符串当作唯一事实身份。
- `SegmentRef`：仅在未来 `ContinuousRecording` 合同冻结时启用。当前 MCAP Chunk/Attachment 不是可单独完成的 Segment。
- 任务可以包含多个独立 Activation；每次都要显式记录 Scope 身份、目标 Artifact、启动/停止时间、失败原因与**覆盖间隙**。跨 Scope 聚合属于解释层，不得改写原始 Canonical identity。
- 线程、FD、磁盘配额、网络连接和 reader lease 均由其所属组件单独计费；在 `DAQConfig` 内增加一个“全局资源上限”并不能替代 Service 的资源核算。

---

## 3. 任务状态机与持久化事实

### 3.1 状态与合法转换

```text
Created ──→ Preparing ──→ Recording ──→ Stopping ──→ Completed
                │             │             │
                └─────────────┴─────────────└──→ Failing ──→ Failed / Partial

Unclean process exit while Task nonterminal
          ↓ (next process generation)
    RecoveryRequired
          ├──→ Completed  (Backend verified, terminal intent reconciled)
          ├──→ Partial    (recovery-valid subset, explicitly incomplete)
          └──→ Failed     (no safely consumable normal Artifact)
```

- `Created`：任务参数校验通过但尚未分配 DAQ Activation/目标产物。
- `Preparing`：持久化任务意图、原子分配唯一目录、持有单 Writer 排他权、完成磁盘准入、DAQ Starting。
- `Recording`：只有 DAQ 实际进入 Running 且任务启动事实已安全记录，才能向客户端承诺“录制已开始”；它不代表当前所有 Source 都有数据。
- `Stopping`：禁入新的预期任务、调用 DAQ Stop 并等待受监督的终止/close 结果；Stop 超时不能自动进入 Completed。
- `Completed`：目标 Artifact 经 Backend 验证为 NormalComplete，且任务完成事实已持久发布；并**不意味着已经 fsync、掉电安全或连续无丢失**。
- `Failing/Failed/RecoveryRequired/Partial`：任务与 Artifact 状态分开。Failure 后的 Artifact 可以是 Partial 或 Unknown；恢复扫描不得把它简单提升为 Completed。

不得引入没有冻结语义的 Pause/Resume。重复 Start/Stop 必须幂等或显式 Conflict；以 `TaskId + OperationId` 去重，迟到的异步回执必须核对任务 generation，不得作用于新 Activation。一个目标目录同一时间只有一个可写任务，跨进程用排他锁及唯一命名保障。

### 3.2 Durable Task Ledger（非 Canonical Journal）

Service 至少持久记录：

```text
TaskCreated(TaskId, config digest, target, generation)
ArtifactReserved(TaskId, ArtifactRef, ActivationRef)
ActivationStarted(TaskId, ActivationRef, scope reference if available)
StopRequested(TaskId, operation id, reason)
ActivationTerminated(TaskId, disposition, errors, known counts)
ArtifactVerified(TaskId, ArtifactRef, verified status)
TaskTerminal(TaskId, terminal status)
RecoveryClassified(TaskId, action, reason)
```

这些是 Task/Artifact **运行事实**，不是原始 Record、Canonical Descriptor 或 AcceptanceSequence 的第二套真相。Ledger 可以采用 append-only journal + 原子 Task Snapshot，必须有版本、长度、checksum 或同等 torn-write 防护、单调 event sequence、事件幂等性和可重放算法。只在持久性策略确认时向外宣称该事件 durable；不允许先对外发布 Completed 后再异步补写唯一持久事实。

对于需要重启后保持的 `TaskCreated`、目录分配、Artifact 完成和恢复决策，应定义文件数据、rename 与父目录 fsync 的具体提交顺序。一次写入成功、`rename()` 返回成功、目录项掉电后持久分别是不同性质；不能只凭 POSIX rename 的原子可见性推断掉电存活。Task Ledger 的持久性不能证明 MCAP Record 的持久性，两者独立计费。

### 3.3 Artifact 状态与双真相防护

```text
Reserved → Open/Writing → Finalizing → VerifiedComplete
                  │               └──────────→ Incomplete
                  ├────────→ RecoveryRequired
                  └────────→ Corrupt / Unreadable
```

- Ledger 是**任务意图/历史**的权威；MCAP 本身及只读 Validator 是**文件实际完整性**的权威。冲突时采取更保守状态，不可只凭 Ledger Completed 认定文件可读。
- `NormalComplete` 需要后端定义的 Footer、Metadata、Sidecar、Descriptor 和回放语义全部验证；仅拥有文件或写出 Finalization 字段不足以证明。
- `PersistenceSucceeded` 按现有 Backend contract 解释，**不自动提供掉电持久性**。对外另报 `DurabilityProfile` 及其已验证的保证，不能改变现有 completion 枚举含义。
- 运行中 Artifact 不在普通目录扫描中被提升为 Complete；读取中的 Artifact 及用户保留/导出状态影响删除资格。

---

## 4. 存储容量、负载与分片设计边界

### 4.1 Disk Admission 与 Headroom

Service 在启动任务时至少检查：存储目录可写性、文件系统类型与目标可用空间、磁盘配额/目录保留、单文件上限、预计峰值写入速率、最大目标时长、异常停顿和 Close/Finalization 预留。不得仅用 `statvfs` 的瞬时值或“剩余空间大于 0”判断可启动。

对于有最大时长的 `BoundedRecording`，必须先预算**整个任务预计占用**，再单独保留故障检测/停止/封口余量。只核算 Stop 缓冲窗口并不能保证任务能够完成：

\[
D_{\mathrm{plan}} \ge R_{\mathrm{peak}}\,T_{\mathrm{max}}\,F_{\mathrm{size}}
+ H_{\mathrm{writer\_metadata}}
\]

\[
H_{\mathrm{emergency}} \ge
R_{\mathrm{peak}}(T_{\mathrm{detection}}+T_{\mathrm{stop}}+
T_{\mathrm{finalize}}+T_{\mathrm{stall}})F_{\mathrm{safety}}
+H_{\mathrm{close}}
\]

\[
C_{\mathrm{available\_for\_task}} \ge D_{\mathrm{plan}}+H_{\mathrm{emergency}}
\]

`C_available_for_task` 以目标设备当前真实可写剩余空间为基数，扣除其他 Writer 尚未落盘的预约、文件系统保留、配额约束和管理员设置的最小安全空间；既有 Artifact 已占用空间不能凭预计未来删除而重复计作可用。峰值速率必须覆盖真实 MCAP Metadata/Index 与配置允许的最坏 Payload；压缩率无可靠上界时不能用平均压缩率作安全承诺。各时间预算和安全因子由部署 Profile 提供，不能在设计里写死“通用安全常数”。无最大录制时长、预留不足、无法确认目录/配额，或 Retention 无法按计划释放已验证空间时，应 fail-closed 拒绝新的 `BoundedRecording` 任务，而不是寄望运行中回收正在写的文件。

运行期间用两个带滞回的阈值观察 `Healthy → Pressure → Critical`，并周期性更新预计可持续写入时间；Pressure 触发告警/限制新任务，Critical 按有限 Stop 期限进入失败或有计划终止。若设备继续高速写入可能越过 Hard Stop 界限，必须在开始任务前缩短允许时长或拒绝启动。磁盘一旦 ENOSPC，不能把已经 Accepted 但失败的 Record 重新归类为 PreAdmissionRejected。

### 4.2 Retention 与安全删除

Retention 由 Service 管理，删除前必须同时确认：Artifact 已封口、catalog 与后端验证一致、不属于当前 Writer、没有活跃 Reader lease、没有故障恢复保留标记、满足用户授权/保留期与审计要求。优先按最旧**已验证可删除**的 Artifact 回收；不能默默删除正在写的文件、Partial Artifact 或未判明所有权的文件。

删除采用可重试、幂等、可审计的状态转换；失败时保留原始完整性声明并告警。目录扫描仅用于 reconcile，不能通过文件名模式决定可删除性。跨文件系统移动、上传或归档须用复制→校验→原子发布→再删除的顺序，不能将未确认复制的删除当作回收成功。

### 4.3 ContinuousRecording 的必要能力（当前未实现）

若最终产品要求无中断连续录制，必须在正式开发前冻结以下跨层扩展：

1. 同一 Identity Scope 下的 Segment Boundary：关闭旧 Segment 与开启新 Segment 的可观察线性化点；已 Accepted 的 Record 不能丢失、重排或重复归属。
2. 每个 Segment 独立的 Descriptor/Projection、Metadata Sidecar、回放/索引完整性，及 Task-level Manifest 的 Segment 顺序、覆盖/间隙、失败状态。
3. 旧 Segment flush/close 未完成、磁盘空间不足、下一 Segment open 失败时的 Admission、Stop、Failure 和 Duration 边界；不得无限缓存或假定下一文件必定成功。
4. 段轮换时 Writer 居留内存、Index/Metadata O(N) 增长、文件句柄数和生命周期均有上限。
5. 原地崩溃、异常重启、写入中断和跨系统迁移后的 Reader/`ros2 bag play`/PlotJuggler/Foxglove 资格验证。
6. Runtime/Storage 的必要 capability 与 ABI 变更须先完成独立审查；Service 绝不直接操作 Plugin 内部 MCAP Writer。

在现有单 Artifact / Activation 契约下，`Stop → New Activation` 只能被标为**有间隙的独立录制周期**。这可以用于长期在线的定期采集，不能用于对外宣称无缝 `ContinuousRecording`。

---

## 5. 故障模型、恢复、时间连续性

### 5.1 故障分类与恢复动作

| 故障 | Runtime 行为 | Service / Supervisor 必须补足的行为 |
| --- | --- | --- |
| Admission 长期压力 | 按统一 Reject/Wait、WaitTimeout 和 Gate cutoff 执行 | 记录 upstream/native loss 与 Runtime reject 的区别；触发告警或提前停止 |
| Storage submit/flush/close 失败 | 维持 Accepted outcome、PersistenceFailed/Unknown，failure/quarantine | 停止当前任务，标识 Partial，防止覆盖和错误 Complete |
| Plugin 永久阻塞 | Timeout + 保留可能被引用的资源；不能杀掉任意线程 | 外部监督按明确策略重启**进程**，记录服务中断和未确定数据 |
| 进程 SIGKILL / crash / OOM | Runtime 没有机会正常 Close | 启动时重放 Ledger、扫描 Artifact 并验证可恢复性；创建新 Activation |
| 主机断电 / 文件系统错误 | 已确认的写入仍可能丢失 | 区分已证明的 Durable 范围与 Unknown；保留损坏证据 |
| 数据源失联/时钟跳变 | Stream/Source 依现有状态和 Clock contract 演化 | 按 Source 质量 SLO 告警、记录覆盖间隙和时间置信度 |
| Watchdog 重启循环 | 不改变 Runtime 模型 | 有界退避、最大次数、熔断、人工干预或备用节点策略 |

对于长时间卡死的 in-process Plugin，不存在能同时保证其继续持有的指针安全和立即回收全部内存的通用解法。`Quarantine` 是 fail-safe 机制，**不是**高可用隔离机制。重启后所有 Runtime handles/Scope 都重新建立；不得把内存中的 Plugin 状态或 pending Completion 当作已经恢复。

### 5.2 启动恢复顺序

1. 获得任务目录排他权，识别上次进程 generation，确认没有并发 Writer。
2. 校验/重放 Task Ledger，截断或隔离 torn tail；按 event sequence 重建可解释的任务状态。
3. 查找 Ledger 引用和目录中未归档的 Artifact，执行**只读**结构/Sidecar 验证，标记 Complete、Partial、Corrupt、Unknown；初始扫描不能自动重写原文件。
4. 对不确定终态产生 `RecoveryRequired`，由部署策略决定隔离、离线修复、保留供人工诊断或开启新的独立 Activation；不允许自动提升为 NormalComplete。
5. 满足磁盘准入和重启预算后才可启动新 Task。记录上一次有效数据结束到本次新 Scope 生效之间的**未知覆盖间隙**。
6. 对后台修复/上传/删除使用幂等状态机，可中断、可续跑，不阻塞 DAQ hot path 或在异常场景持有 Plugin 对象。

### 5.3 时间与跨 Scope Coverage

Coverage 是 Application-level 的时间区间解释，不是 Canonical Record 或 ClockCorrelation。每个 Activation 独立保存 Scope identity、host boot identity、启动/终止的可追溯 wall-clock/monotonic 观测及精度/不确定性来源。

同一 Scope 内按既有 ClockId 和 AcceptanceSequence 解释 Record；**不同 Scope、不同 Host/boot、不同 ClockId 默认不可直接比较**。只有被资格验证的相关性与误差界才能声称跨文件时间连续。没有足够数据时 Coverage = Unknown，而不是“零间隙”。Reader 组合跨文件时间线必须保留 Scope 边界，不得重写原 ID。

---

## 6. 运行观测、控制与安全

### 6.1 健康度与指标域

至少区分三个独立健康域：

- `RuntimeHealth`：State、Gate、队列 high-water、Payload size-class、dispatch progress、completion backlog、Stop、Plugin health、Quarantine。
- `AcquisitionQuality`：Source/Stream available、native gap/drop（若源提供足够证据）、Plugin-local staging rejected、Runtime PreAdmissionRejected、Accepted、Unknown。**来源不提供序号或可靠统计时不得报告“0 loss”。**
- `StorageHealth`：storage submit latency、PersistenceSucceeded/Failed/Unknown、backend pressure、disk free/headroom、预计剩余可录制时长、最后一次有效进展、Artifact/Task status、durability profile。

指标必须标注 `TaskId/ActivationRef/Scope` 的归属与 generation；任务间不能隐式合计 AcceptanceSequence。对快照轮询提供有限时间预算与 bounded cardinality，不能因导出 Metrics 阻塞热路径。日志与指标的丢失/采样应独立标识，不能用日志作为唯一 Task Ledger。

### 6.2 Watchdog 与自动化策略

Watchdog 需要综合控制循环、存储写入进展和任务阶段期限，不可只看 PID 存活。**没有预期 Source 负载时，Accepted 计数不增长不是故障**；须由任务的 expected-source policy 决定“超时无数据”是否告警。

Restart 应拥有最大次数、间隔、退避和永久失败后的熔断；失败期间禁止并发重启创建多个 Writer。`systemd` 的进程管理与 Watchdog 可作为部署实现，但只有服务真实地上报健康心跳和处理信号时才可以声明 Watchdog 生效。采集任务不能依赖 In-Process Quarantine 实现无限自愈。

### 6.3 API 与控制平面

Service 可将 CLI、REST/Web、任务状态推送和受限数据导出作为独立适配器，但不向远程客户端暴露 DAQ 私有线程/句柄。生产部署必须定义：

- 任务控制的身份认证、最小授权、可审计 OperationId、幂等提交、并发冲突；
- 默认只在可信接口监听，外部网络访问采用 TLS/等价安全通道和访问控制；不得借用 ROS2/DDS 发布端可信假设；
- Reader 下载对运行中目录和损坏 Artifact 的可见性策略、下载租约、带宽/连接上限；
- Web UI/HTTP/API 失效不影响当前有界录制任务的安全继续或受控终止；
- 数据导出和 Retention 必须遵守任务级权限与审计，删除前验证 active reader / ongoing export。

这些是产品接口**要求**，不是对现有 `apps/` 的实现声明。

---

## 7. 生产验收与交付依赖

### 7.1 运行 Profile 与 SLO 必填项

每个目标部署必须显式提供：数据源与压缩前/后峰值速率、预计任务时长、最小磁盘余量与保留策略、允许的前端/Plugin/Runtime 数据损失、最大接受/持久化延迟、RPO、RTO、合法的重启/故障预算、时间同步精度要求和允许的未知 Coverage。无定义则不能以“工业生产合格”发布，也不得用一个统一阈值代表所有 Camera/Joint/IMU。

### 7.2 资格验证矩阵

| 组别 | 最低应验证的事实 |
| --- | --- |
| Task lifecycle | Start/Stop/Retry/late callback、并发任务/目录锁、Crash 后幂等操作、代际隔离 |
| Storage capacity | Near-full、ENOSPC、Quota、写入速率低于源端、后台保留回收失败、读写并发 |
| Crash/Recovery | SIGKILL、OOM、Power-loss simulation、torn Ledger、Partial/Corrupt Artifact、重复恢复 |
| Resource trend | RSS/FD/线程、Queue/Pool、MCAP writer index/sidecar、文件数量、磁盘空间长期趋势 |
| Fidelity | 完整的 MCAP+Sidecar Decode、Descriptor/Record count、时钟映射、ROS2 回放与 Consumer 资格 |
| Supervision | Plugin deadlock、无数据、Watchdog、服务重启、熔断、实际恢复耗时与间隙 |
| Interoperability | ROS2 Acquisition 和 FastDDS Acquisition 的独立运行 Profile，输出均满足消费者兼容性 |
| Stress / soak | 真实磁盘上的 Camera+Joint Mixed Profile、故障穿插、长期内存与性能稳定性 |

建议将当前 M5 的 30–60 分钟混合负载视为**短时性能门禁**，而不是 7×24 长期稳定性证明。无人值守目标建议先完成至少 72 小时持续 Soak，并以 168 小时或与目标维护周期匹配的时长作为发布前资格验证；这些是建议 Profile，不是通用标准规定的神奇数字。所有 PASS 都须附 workload、设备、磁盘、数据量、p99/p99.9、资源增长斜率、失败处理记录和实际 Artifact 核验结果。

### 7.3 实施分层与冻结门槛

**A — 立即设计收敛（不改 Plugin ABI）**：任务/Artifact 的职责与状态、Ledger、disk headroom、Crash 恢复、Health/SLO、进程监督、`BoundedRecording` 合同。现有 `DAQConfig`、`DAQStats` 不提供的指标/控制，应列为实现缺口，不能从文档推定已存在。

**B — M3/M4 真实链路闭环**：在 ROS2/FastDDS + MCAP 上测量真实写入、资源、回放和失败行为；证明当前单 Storage Lane 与全局 AdmissionMode 在声明 Profile 内可用，失败则通过设计决策修订，不得偷偷引入无界队列或 per-Stream override。

**C — 生产可用性门禁**：实现 `BoundedRecording` 的任务服务、磁盘准入、持久 Ledger、保守 Recovery、健康监督和长时测试，再对外宣称“可长期在线按需录制”。

**D — ContinuousRecording 扩展（依赖后续 Storage/Runtime 合同修订）**：只有完成 `4.3` 的原子分段、Segment/Task Manifest、回放、失败矩阵和资源验证，才可对外宣称“长期不中断自动滚动录制”。在此之前既不修改现有 Canonical 模型，也不将 `Stop → Start` 粉饰为无缝 rolling。

本文件本身不是交付证据。生产合格取决于后续代码、故障注入、真实设备和长期观测。
