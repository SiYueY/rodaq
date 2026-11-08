# RoDAQ 术语表

RoDAQ 是面向工业机器人的数据采集应用。操作者可以查询录制覆盖和可交付产物。

## 录制

**录制任务（Recording Task）**

操作者对数据来源、录制策略和交付要求的持久定义，也是录制控制与历史查询的基本对象。

**任务配置（Task Configuration）**

RoDAQ 为一个录制任务保存的来源、录制链路和输出等参数。

**任务配置文件（Task Configuration File）**

描述录制任务如何采集、保存和保留数据的 JSON 文件，可用于任务导入和导出。

**Stream Identity State**

按 Task 保存稳定 Stream 身份和 `acceptance_sequence` 持久预留上界的独立状态。它跨 Recording Instance 存续，不属于当前实例恢复事实，也不能从已提交 Segment 反推。

**恢复记录（Recovery Record）**

用于核对录制中断、续录资格以及未完成的产物清理的独立记录。它保存执行与清理进度，不拥有 Task 的 Run/Reset 控制意图。

**诊断模式（Diagnostic Mode）**

启动时无法校验本机任务配置目录或必要的恢复记录，应用仅提供本机健康查询与故障诊断的运行模式。

**部署配置（Deployment Configuration）**

运维为一台运行节点定义的服务接入、允许使用的资源与全局约束。RoDAQ 自有部署配置采用严格 JSON 表示。

**严格 JSON（Strict JSON）**

RoDAQ 自有 Deployment Configuration、Task Configuration、Stream Identity State、Task Control Record、Recovery Record 和 Segment Manifest 统一采用的边界表示。禁止注释、尾随逗号、NaN/Infinity 和重复对象键，并对输入大小与结构深度设置资源上限。不同文档仍保持独立版本、校验规则、强类型领域模型和权威职责；rosbag2 `metadata.yaml` 等第三方原生格式不属于该规则的转换范围。

**部署代次（Deployment Generation）**

一次通过校验并被守护进程采用的不可变部署配置身份。运行中的录制实例绑定其启动时的部署代次，后续修改 `deployment.json` 不会原地改变该实例。

**生效录制配置（Effective Recording Configuration）**

Start、Resume 或自动恢复时，由 Task 配置、Deployment Generation、精确插件版本、输出卷身份、解析后的非敏感参数、能力与资源计划等共同形成的本次录制实例运行合同，以稳定 `effective_config_id` 标识。

**链路资源档位（Pipeline Resource Profile）**

运维在部署配置中为一条录制链路定义的具名资源额度。任务配置按名称引用该档位。

**录制链路（Pipeline）**

任务内从采集到存储的一条路径，由一对采集器与存储器组成，可承载多条数据流。

**录制实例（Recording Instance）**

一个录制链路的一次运行，用于区分新旧执行、资源归属、迟返隔离以及关联其录制结果。录制实例不是跨数据流的记录排序空间。

**准备屏障（Prepare Barrier）**

Start 或 Resume 开放接纳前的任务级边界。各 Pipeline 先完成所有可能失败或阻塞的准备并保持 Admission Gate 关闭；只有启动策略要求的 Pipeline 全部 Ready 后才允许执行 Open Admission。

**任务重置（Task Reset）**

清空同一任务的运行状态和全部录制产物、使任务回到停止态的控制操作。任务定义和身份保留。

## 插件

**采集器插件（Collector Plugin）**

为 RoDAQ 提供一类数据源接入能力、交付数据流及其记录的插件。

**存储器插件（Storage Plugin）**

为 RoDAQ 提供一种录制格式的写入与封口能力的插件。

**录制组合（Backend Combination）**

一条录制链路选用的采集器、数据流载荷编码和存储器产物约定。受支持的录制组合有对应的读回或回放证据。

**插件配置描述（Plugin Configuration Schema）**

插件随版本提供的机器可读参数结构与约束，用于在创建实例前检查插件专有配置。它描述字段结构，不替代插件自身的协议语义校验。

**插件资源计划（Plugin Resource Plan）**

插件根据已经验证的当前配置给出的有界资源申请，包括内部缓冲、线程或执行者、文件描述符、最大在途数据及其它硬资源。它必须落在部署允许的链路资源档位内。

**链路生效约束（Effective Pipeline Limits）**

由链路资源档位、Core 限制以及采集器和存储器的能力与资源计划共同得到的本次录制实例有效上限和可用能力。开放接纳前必须确定。

## 数据与产物

**数据流（Data Stream）**

以 Topic 等业务地址和稳定流身份标识、具有可解释载荷的逻辑记录序列。每条数据流独立维护接纳序号。

**数据来源（Data Source）**

实际产生记录的生产者身份。来源可以在逻辑数据流保持不变时发生重启或替换；协议无法稳定识别生产者时明确标记来源身份缺失。

**来源序号作用域（Source Sequence Scope）**

来源提供的 native sequence 可被解释为顺序、重复或缺口的身份范围。只有能够把序号绑定到明确 Source、连接会话或其它记录下来的生产者域时，RoDAQ 才用它推断来源缺口；作用域不明时该值只作为诊断事实。

**Stream Descriptor**

对一条数据流某一版解释语义的不可变描述，包含载荷编码、Schema、来源时钟/时间角色以及必要的有界扩展信息。解释属性变化时创建新的 Descriptor。

**记录接纳序号（Acceptance Sequence）**

一条数据流在接纳提交点为已接纳记录分配的单调序号。它只在所属数据流内排序，与录制实例的 Writer 交付顺序不是同一个概念。RoDAQ 通过持久区间预留保证同一 `stream_id` 跨 Recording Instance 和守护进程重启不复用序号；允许出现未使用的 gap。

**接纳提交点（Admission Commit Point）**

一次记录提交从“可以被接纳前拒绝”变成“必须作为已接纳记录结算”的唯一逻辑切点。普通队列和内存资源必须在该切点前取得。

**Pipeline Admission Gate**

每条录制链路用于建立 Start/Open、Pause、Stop 和 failure cutoff 全链路线性化边界的轻量门。它与各 Stream 自己的 Descriptor/Sequence 同步域配合，不承担插件或 Storage 工作。

**描述切换（Descriptor Cut）**

同一数据流从旧 Stream Descriptor 切换到新 Descriptor 的唯一运行期边界。切换前已接纳的旧描述记录继续结算，切换后新的提交只能使用新描述。

**原始载荷（Raw Payload）**

采集器收到并交给 RoDAQ 录制的原始序列化应用字节。

**载荷持有方式（Payload Holding Mode）**

RoDAQ 在记录接纳后保持载荷有效的所有权方式，包括复制持有、所有权转移和共享持有。任何方式都必须满足 exactly-once 释放、静止证明和物理资源计费。

**保留样本（Retained Sample）**

MQTT 采集器在订阅时收到、由 Broker 标记为保留消息的录制样本。

**接收时间（Receive Timestamp）**

RoDAQ 收到记录时采集主机的壁钟时间，用作默认查询和回放时间轴。

**来源时间（Source Timestamp）**

来源提供的记录时间，与产生该时间的时钟域相关。

**主机启动标识（Host Boot ID）**

标识采集主机一次启动的稳定值。monotonic 时间、Clock Epoch 和 realtime↔monotonic 相关性只在同一 Host Boot 内直接可比。

**时钟区段（Clock Epoch）**

同一 Host Boot 内 realtime 与 monotonic 的相关性发生超过允许误差的正向或反向跳变时建立的新区段。不同区段的接收壁钟范围可以重叠；跨 Host Boot 不因 Epoch 编号相同而可比较。

**分片（Segment）**

一次持久化提交形成、可独立验证和读取的录制产物。

**分片滚动（Segment Rotation）**

在任务保持录制意图期间封口当前分片，后续记录进入新分片。

**连续录制（Continuous Recording）**

持续接收和保存数据、默认保留已提交历史的录制模式。

**滚动录制（Rolling Recording）**

持续录制并维护一个保留窗口的录制模式。符合规则的窗口外已提交分片可以自动回收。

**保留窗口（Retention Window）**

滚动录制任务按录制时间要求保留的最近一段数据范围，随已提交数据推进。

**录制时间（Recording Time）**

任务实际开放接纳期间累计的时间，暂停、停止和应用停机期间不累计。它用于计算保留窗口。

**自动回收（Automatic Cleanup）**

按照任务明确启用的保留规则删除符合条件的已提交分片。

**覆盖报告（Coverage Report）**

对录制有效时段、已知缺口与无法判定区间的证据描述。

**采集频率（Receive Rate）**

观测窗口内数据流实际到达的记录数除以窗口时长，单位为 Hz。窗口内无记录时频率为 `0 Hz`。

**存储逻辑速率（Storage Logical Rate）**

存储器处理的原始逻辑载荷字节随时间的速率，用于与接纳的数据量比较。开启压缩时它可以显著高于物理写入速率。

**存储物理速率（Storage Physical Rate）**

分片文件或底层介质实际增长/写入的物理字节速率。它反映压缩、索引和格式开销后的 I/O，不直接代表逻辑数据是否丢失。

**提交滞后（Commit Lag）**

最新已接纳录制进度与最新已完成持久提交进度之间的差值，用于判断录制链路是否长期落后。应与最老待写记录年龄和队列高水位共同解释。

**Commit Lane**

负责 sealed Segment 的文件同步、原子发布、父目录同步和 durable fact 发布的有界执行域。它与 Writer Lane 解耦，但 backlog 的 Segment 数和物理字节严格受限。

**可重试性（Retryability）**

结构化描述失败是否以及在什么条件下允许重试，例如永不重试、条件变化后重试、退避后重试、进程重启后重试或必须人工恢复。恢复逻辑不得从错误字符串推断可重试性。

**失败域（Failure Domain）**

表示失败归属的系统范围，例如 Request、Task Control、Pipeline、Collector、Storage、Volume、Persistence State 或 Process，用于决定隔离、恢复和告警范围。

**数据影响（Data Impact）**

失败对录制数据语义的结构化影响，例如无数据影响、仅接纳前拒绝、已接纳记录确定失败、持久结果未知、产物隔离或历史产物损坏。它与操作结果、期望控制状态和 Task 执行条件分开表达。
