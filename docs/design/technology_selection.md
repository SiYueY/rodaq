# RoDAQ 技术选型说明书

| 项目 | 内容 |
| --- | --- |
| 状态 | 选型评审稿 |
| 日期 | 2026-11-08 |
| 系统依据 | [早期设计笔记](note.md) |
| 适用对象 | V1 工业录制核心及本机应用的依赖与实现基线 |

本文保留早期选型依据。[当前架构评审稿](architecture.md)已确认单进程故障边界和采集器、存储器两类动态库插件，动态库边界采用版本化 C ABI。兼容范围按联合验证的发布矩阵声明。录制后端、执行容量、外部进程监督、持久化和应用依赖仍需按当前合同验证。

## 1. 选型原则

依赖必须能够在 C++17 和目标编译器上构建，适用于单进程录制，并有清楚的线程、内存、生命周期和许可边界。优先采用成熟且适合当前职责边界的通用组件。RoDAQ 的录制状态、数据接纳、产物提交和资源所有权由系统设计定义。

新依赖的选型分为推荐、接入验证、锁定和发布资格。候选库须在目标环境通过接入验证后才能锁定版本。发布资格以本项目的验证结果为准。核心数据路径优先用真实负载证据决定复杂度。

## 2. 总体选型

| 领域 | 推荐技术 | 归属和主要理由 |
| --- | --- | --- |
| 语言、工具链 | C++17，GCC 11.x / Clang 14.x，CMake 3.22 + CTest | 以 Ubuntu 22.04 为可复现构建基线 |
| Task 状态机 | **Boost.Ext SML** | `rodaq` 模块私有依赖，集中定义事件、Guard 和转换，支持多个独立 Task 实例 |
| 通用并发 | C++17 标准线程、互斥锁、条件变量、原子操作 | `rodaq` 模块从进程总量为链路分别预留串行写入与采集控制容量，另留停止和诊断容量 |
| 数据队列、接纳与预算 | 固定容量有界队列 + Pipeline Admission Gate + 项目维护的接纳/载荷预算 | 链路启动前同时预留记录数、逻辑字节和物理载荷内存；Gate 只建立 Pipeline-wide cut，per-Stream commit 负责 Descriptor/Sequence。队列槽位和 Payload 在 commit 前取得，Pause/Stop 关闭 Gate 后只等待有界 commit-domain in-flight |
| 日志和格式化 | spdlog + 外部 fmt | 应用拥有日志设施，统一配置日志限额和关停策略 |
| 错误与结果 | 项目 `Status`、`Result<T>`，复用 tl::expected 作为底层实现候选 | 冻结 Status Code、Retryability、Failure Domain 和 Data Impact 四个维度；操作结果、Task 执行条件和数据影响保持正交。C ABI 使用独立结果码并稳定映射到 Core 语义 |
| JSON 配置与持久状态 | nlohmann/json | RoDAQ 自有的部署配置、任务配置、Stream Sequence/Identity 状态、Task Control Record、恢复记录和分片清单统一采用严格 JSON。Deployment Generation、`config_version` 和 `effective_config_id` 使用冻结的规范化规则生成稳定身份 |
| 插件配置描述 | JSON Schema，具体方言和验证实现随插件 ABI 冻结探针一起锁定 | 插件声明自己的参数结构和版本；Web/Server 可据此生成或校验通用表单，插件仍执行协议相关语义校验。Schema 库不跨插件 C ABI |
| HTTP Server | **Drogon v1.9.14** 作为接入基线 | 根目录 `server/` 私有依赖，提供控制与查询 API 及前端静态资源 |
| Web 前端 | **原生 HTML、CSS 和 JavaScript**，通过 Fetch 调用 HTTP JSON API | 根目录 `web/`，以静态文件随服务端发布，基础构建无需 Node.js 工具链 |
| 采集 | 首个闭环使用 ROS 2 `rclcpp`，第二个采集器为 MQTT。gRPC 为扩展候选 | 协议依赖限定在各采集器插件及其验证环境 |
| 存储 | 首个闭环用 MCAP C++，第二个验证的存储器用 rosbag2 SQLite3 布局 | 两种 ROS 2 组合分别验证格式封口、独立读回和标准 ROS 2 回放 |
| 身份、任务与控制持久化 | 不可变任务配置 + Stream Sequence Identity State + Task Control Record + 独立恢复记录 | Stream 接纳序号以持久区间预留跨重启不复用；Task Control Record 保存 Run/Reset authority 和操作账本；Recovery Record 保存实例/续录/逐卷清理进度，三者职责不重叠 |
| 指标与测试 | HdrHistogram_c，GoogleTest，Google Benchmark，Clang/GCC Sanitizer | 支持接纳、写入、封口和提交尾延迟，记录 CPU/RSS/线程/FD/队列和物理载荷高水位。真实 ROS 2/MCAP、文件系统和 72h/168h 资格有独立测试入口 |
| 插件装载 | 使用独立动态库和版本化 C ABI。由平台适配层封装目标系统的装载接口 | 按发布矩阵验证兼容性。实例静止后可销毁，库句柄随进程退出回收 |

## 3. `rodaq` 模块依赖接入要求

下表列出候选依赖的职责及接入检查。接入时选择适配目标环境的版本，完成验证后将完整提交与许可证写入依赖清单。

| 依赖 | 许可证 | 接入检查 |
| --- | --- | --- |
| spdlog + fmt | MIT | 验证外部 fmt 组合的构建兼容性、日志满载行为与关停 |
| nlohmann/json | MIT | 验证严格 JSON 解析、重复键拒绝、文档资源上限、规范化序列化、强类型转换和持久状态往返 |
| tl::expected | BSL-1.0 | 验证 Result 实现、C++ SDK 安装和独立消费 |
| HdrHistogram_c | BSD-2-Clause | 验证尾延迟统计口径、采样和合并开销 |
| GoogleTest | BSD-3-Clause | 验证目标编译器和 CI 测试构建 |
| Google Benchmark | Apache-2.0 | 验证 `rodaq` 模块基准可复现，真实写盘与长时试验有独立入口 |

日志后端和格式化依赖留在实现侧。若安装的 `Result<T>` 模板间接包含 tl::expected，随 C++ SDK 交付所需的头文件。插件 C ABI 使用独立的 C 头和 ABI 类型。

插件配置描述采用标准 JSON Schema 表达，但不把任一 JSON Schema C++ 库写入公共接口。接入探针需要验证所选方言、默认值处理、未知字段策略、错误路径定位、最大深度/大小和恶意输入成本；验证通过后再锁定具体实现及提交。插件 ABI 只接收 RoDAQ 定义的只读配置视图或等价 C 兼容表示，插件不能保存调用期借用指针。

插件并发资格按架构合同冻结：同一 Collector 实例的控制调用由宿主串行化；数据回调可并发但插件必须声明最大 callback concurrency；Storage 变更调用默认只有一个逻辑 lane；只读查询只有显式声明 thread-safe 才能并发。ABI conformance test 必须覆盖 callback 与 quiesce 竞争、宿主调用迟返、Storage timeout 后的 payload-access quiescence、destroy 前静止证明和禁止未声明重入。

`rodaq` 模块的数据热路径优先采用固定容量队列和按大小分档的有界载荷池，具体块尺寸由 800 B 高频记录、80 KiB 聚合记录、编码帧和约 2.76 MiB 原始帧的混合基准决定。标准分配器仍可用于控制面和非热路径对象；如果真实资格表明载荷分配、调度或内存带宽成为瓶颈，再分别评估 `std::pmr`、oneTBB 或其它经过基准证明的实现，不能以增加无界缓存代替优化。

## 4. 状态机框架

### 4.1 候选与决定

| 候选 | 适配性 | 结论 |
| --- | --- | --- |
| **Boost.Ext SML** | C++14 基线、单头文件、无需 Boost 发行包。转换表可集中表达 State、Event、Guard 和 Action，适合以每个 Task 实例持有机器 | **首选。** 将模板实例化集中在私有实现，实际编译时间与目标工具链适配需验证 |
| 显式 `enum + switch` | 零第三方依赖，调试直接。事件和错误路径增多后，转换及合法性检查需要靠项目自行组织 | 作为选型探针的简洁度对照。极小且没有独立转换价值的状态可继续用项目类型 |
| TinyFSM | 库较小，按状态类分发事件。默认用法需进一步核对同类型多个 Task 实例的状态隔离 | 多实例和统一转换审阅是当前优先条件，保留候选 |
| HFSM2 | 支持静态定义的层级状态结构，适合确有嵌套状态和多层行为的模型 | 现有 Task 可用扁平转换表表达，层级需求有独立证据后再评估 |

Boost.Ext SML 是独立的 `boost-ext/sml` 项目，许可证为 BSL-1.0。选型探针可从已发布的 `v1.2.0`（`2d8485da6661519a7697b5f800c7165c543fa499`）开始。探针沿用该发布版默认配置，通过目标工具链和 Task 转换检查后，再将提交写入 RoDAQ 依赖清单。上游资料见 [SML 概览](https://boost-ext.github.io/sml/overview.html)、[版本发布](https://github.com/boost-ext/sml/releases/tag/v1.2.0)、[TinyFSM 用法](https://digint.ch/tinyfsm/doc/usage.html) 和 [HFSM2](https://hfsm.dev/)。

### 4.2 在 RoDAQ 中的使用边界

- **每个 Task 一个机器实例。** Task 控制执行域串行投递事件。HTTP 和插件完成回调提交项目事件。公共查询读取由 Task 发布的快照。生命周期写入权集中在转换处理，公共状态枚举从状态机和在途操作投影。
- **状态转换与异步副作用分开。** 转换只更新必要的内存事实并发布待执行工作。插件调用、文件同步、等待和资源回收在执行域完成，由明确结果事件驱动后续转换。Guard 读取当前状态与已发布事实。Task 的期望控制状态和执行条件保持正交，SML 只实现控制事件序列，不产生第二套可独立写入的 Task 健康状态。
- **异常和迟返有归属。** 执行结果关联 `operation_id` 及相关资源所有权。HTTP 等待到期返回操作引用。插件调用到期进入链路故障收尾，迟返只更新原调用的结算与资源事实。旧操作完成不得推进新的 Start 或 Resume。
- **依赖范围保持窄。** SML 只在 Task 控制实现内使用。Pipeline 只有在拥有独立且需要集中验证的转换合同后才使用同一库。采集回调、Writer 热路径、Segment 提交、HTTP 类型和插件 ABI 与 SML 类型隔离。
- **测试按转换合同验收。** 覆盖合法和非法事件、准备失败并清理、Pause 封口成功及失败、迟返完成、两个 Task 实例并行以及系统关停。优先从真实初态通过项目事件驱动测试，框架测试辅助仅用于局部分支定位。

| 示例 | 转换或效果 |
| --- | --- |
| Running + PauseRequested | 登记在途 Pause，发出关闭入口、排空、封口与释放实例的工作 |
| 在途 Pause + 同一操作收尾成功 | 已接纳数据提交、实例及预留额度释放后，发布 Paused 和成功结果 |
| 在途 Pause + 收尾失败 | 期望状态保持 Paused；操作结果为 Failed，执行条件按事实投影为 PendingCleanup 或 Failed，并保留已有产物事实 |
| 收到已结束或不同操作的完成通知 | 按所属工作结算资源及诊断，当前操作的状态仍由自己的完成事实决定 |

## 5. HTTP、配置与运维

- **HTTP 服务选用 Drogon。** 以 v1.9.14（`3aae769cc405cf5abb9d477675c6430924f02c7c`，MIT）作为验证基线，接入时同时记录 Trantor、JsonCpp 等实际依赖及许可证。Drogon 网络回调做有限的解析、校验和控制提交，使用操作标识查询进度。插件调用、封口和磁盘扫描留在各自执行域。`rodaq` 模块通过项目类型交互，Drogon 和 JsonCpp 类型留在服务实现侧。
- **前端静态资源同源部署。** `web/` 中的 HTML、CSS 和 JavaScript 按静态资源打包，服务端从同一本机回环地址提供页面 `/` 和 `/api/v1/`。前端用同源相对路径访问首版 API。
- **状态更新优先使用有界轮询。** V1 读取已发布的任务及系统快照，活动页面默认约每秒刷新一次，每类查询最多一个在途请求，失败后退避。控制提交与轮询独立，历史查询按需分页。后续有实时事件需求时，单独评估推送协议及连接、缓冲和关闭成本。
- **RoDAQ 自有边界统一使用严格 JSON。** 部署配置、任务配置及导入导出、插件参数对象、Stream Identity State、Task Control Record、Recovery Record、RoDAQ Segment Manifest 和 HTTP JSON 都采用同一严格 JSON 语法。拒绝注释、尾随逗号、NaN/Infinity 和重复对象键，并对文档大小、深度、字符串长度和成员数量设上限。第三方格式按其原生规范处理，例如 rosbag2 的 `metadata.yaml` 不转换为 JSON。[nlohmann/json](https://github.com/nlohmann/json)
- **统一表示不等于统一模型。** 每种 JSON 文档有独立的格式版本、校验规则和强类型项目对象。解析与结构校验只发生在边界层，随后转换为 `DeploymentConfig`、`TaskConfig`、控制记录、恢复记录等项目类型；`nlohmann::json` 不进入 `rodaq` 公共接口、状态机、数据热路径或插件 C ABI。
- **日志优先可控。** 使用 spdlog + fmt，按任务、链路和操作标识关联诊断。日志属于应用级资源，采用低频事件和周期汇总。若启用异步，记录数和字节数都要有预算，并验证满载策略和停机。采集回调避免逐条格式化日志。
- **监控使用项目快照。** 计数器和采样时间戳由 Task/Pipeline 更新。尾延迟测量使用 HdrHistogram_c，并验证采样率、合并开销及统计口径。HTTP 查询读快照，运维导出从同一事实生成。

## 6. 真实后端与持久化

ROS 2 采集和 MCAP 存储分别由采集器和存储器插件提供。使用真实链路验证消息对账、独立解码、标准 ROS 2 回放、Descriptor 切换、滚动和写入失败处理。ROS 2 插件验证执行器线程、QoS、序列化载荷所有权、来源/时间事实和关闭条件；其配置生成资源计划，并声明可用的载荷持有方式。MCAP 插件负责格式写入、索引与封口，报告物理产物大小、保守封口余量、批次限制和所需内部资源。RoDAQ 在开放接纳前形成链路生效约束，负责分片描述、发布和资源结算。

Writer 与 Commit 使用两个逻辑 lane：Writer 只负责有序格式写入、切段和把 sealed Segment 交给有界 commit queue；Commit lane 执行文件/目录同步、原子发布和 durable fact。`max_pending_segments`、pending physical bytes 和同卷 fsync 并发都属于硬预算，资格测试必须测量 seal/fsync 尾延迟并证明 backlog 达到稳态。压缩按实际 CPU、吞吐、磁盘数据量和尾延迟决定。[MCAP 格式规范](https://mcap.dev/spec)

RoDAQ 自有的 Deployment Configuration、Task Configuration、Stream Identity State、Task Control Record、Recovery Record 和 Segment Manifest 复用同一严格 JSON 解析/序列化基础设施，但分别映射到独立强类型、版本与权威合同。Task 配置经校验后形成稳定 `config_version`；Deployment Configuration 形成 Deployment Generation；Start/Resume/恢复时解析出的实际运行合同形成 `effective_config_id`。三类身份的规范化规则都必须有跨版本测试，避免键顺序、空白或等价数值文本造成身份漂移。Stream Sequence 区间预留、操作幂等、Boot Reconciliation 和 Reset progress 分别从其权威持久对象恢复，不能用某个 JSON 文件替代另一类事实。SQLite3 依赖及其 `metadata.yaml` 仍由 rosbag2 SQLite3 存储插件按外部格式管理。[配置与持久事实权威](architecture.md#61-配置与持久事实权威) · [独立恢复记录](architecture.md#341-独立恢复记录)

### 6.1 扩展后端的接入验证

| 后端 | 候选依赖或交付约定 | 资格验证 |
| --- | --- | --- |
| MQTT 采集器 | 第二个支持的采集器，先验收 MQTT 3.1.1、QoS 0/1 的 MQTT → MCAP JSON 样本。Eclipse Paho MQTT C++ 为候选。[VDA 5050 传输协议](https://github.com/VDA5050/VDA5050/blob/main/VDA5050_EN.md?plain=1#4-transport-protocol) | 验证 Broker 晚启动、重连建立新协议会话和重新订阅、精确 Topic、保留消息、载荷大小限制与停止静止。独立读回核对原始载荷、接收时间、记录数和保留标记 |
| gRPC 采集器 | gRPC C++ 为候选，协议由指定服务、方法与消息定义约束 | 测试流终止、取消与迟返、接收上限、重连后覆盖证据和实例收尾 |
| SQLite3 存储器 | 采用 rosbag2 的 SQLite3 布局。优先验证复用目标发行版的 rosbag2 写入接口 | 验证 `.db3` 和 `metadata.yaml` 的目录级读回、载荷与时间戳对账、标准回放、封口和 WAL 相关文件完整性 |

候选依赖在确定交付范围后锁定版本并验证。PlotJuggler 的 MQTT 插件依赖记录可作为 Paho 的参考。[PlotJuggler 官方插件依赖](https://github.com/PlotJuggler/pj-official-plugins)

### 6.2 PlotJuggler 互操作参考

PlotJuggler 官方插件将实时接入、离线文件加载和消息解析分别建模。RoDAQ 将其作为外部分析工具，验收覆盖 ROS 2 → MCAP 产物。用实际分片验证预期 Topic、字段与接收时间轴，并记录所用 Loader 和解析器版本。[PlotJuggler 官方插件清单](https://github.com/PlotJuggler/pj-official-plugins)

| 能力 | PlotJuggler 支持与 RoDAQ 验证关系 |
| --- | --- |
| 实时接入 | 有 ROS 2、MQTT 等实时插件。MQTT 的载荷解析取决于所选消息解析器。[ROS 2 Subscriber](https://github.com/PlotJuggler/pj-official-plugins/tree/main/data_stream_ros2#readme) · [MQTT Subscriber](https://github.com/PlotJuggler/pj-official-plugins/tree/main/data_stream_mqtt#readme) |
| 文件加载 | 官方清单包括 MCAP、CSV、Parquet 和 ULog 等。MCAP Loader 依据 Channel 和 Schema 绑定消息解析器。互操作验收须核对实际可读取的 Topic、字段和时间轴。[MCAP Loader](https://github.com/PlotJuggler/pj-official-plugins/tree/main/data_load_mcap#readme) |
| rosbag2 SQLite3 | ROS 插件可通过 ROS bag 的 `metadata.yaml` 与相应 rosbag2 存储插件读包。[ROS bag Loader 源码](https://github.com/PlotJuggler/plotjuggler-ros-plugins/blob/main/src/DataLoadROS2/dataload_ros2.cpp#L29-L105) |
| gRPC | 官方通用实时插件清单未列出通用 gRPC 接入。Protobuf 解析是独立能力，RoDAQ gRPC 采集器需按具体服务验证。[官方插件清单](https://github.com/PlotJuggler/pj-official-plugins) |

## 7. 依赖管理与构建边界

1. 生产第三方源码采用固定提交，记录来源、标签、完整提交、许可证和启用的构建选项。使用固定提交的 Git submodule，支持依赖准备后的离线构建。系统提供的 ROS 2 环境独立记录版本和目标平台。
2. 按构建目标管理依赖。`rodaq` 模块公共接口使用项目类型。SML 属于任务控制实现，HTTP 框架归 `server/`，前端工具归 `web/`，JSON 仅用于边界解析、序列化与持久状态实现。ROS 2 与 MCAP 各归对应插件。接入时单独构建 `rodaq` 模块，核对它可在没有 HTTP 框架和录制后端的环境中构建。[模块与构建依赖](architecture.md#27-模块与构建依赖)
3. 随包发布版本化 C ABI 头文件和最小插件示例。用 C 编译器独立检查头文件，并在源码树之外用发布头编译、装载和调用示例。检查接口版本、符号可见性及错误结果。
4. 开发工具和运行时产物分别打包。分别记录接入、验证和发布资格状态。升级第三方库后，重跑直接受影响的合同与性能检查。
5. 对新候选选择可复现的发行版本。接入时记录完整提交并执行许可证审查，避免从浮动分支或在线配置阶段隐式下载。

## 8. 接入门槛

| 验证 | 通过条件 |
| --- | --- |
| SML 探针 | GCC 11 和 Clang 14 的 C++17 Debug/Release 构建通过，警告和 ASan/UBSan 检查通过。验证两个独立 Task 的状态、非法事件、Pause 收尾与迟返 |
| Server 与 Web | 验证 Drogon 在目标工具链上的链接及打包。验证 Web 和本机自动化共用 `/api/v1/`，覆盖回环绑定、幂等冲突、配置导入导出、慢连接、请求饱和、关停及录制负载下的资源隔离 |
| `rodaq` 模块依赖 | 配置和结果头可独立使用，发布包链接边界正确。记录依赖的版本、许可和启用选项 |
| 插件合同 | 用可控插件验证配置描述、语义校验、能力、资源计划、链路生效约束和可选扩展。覆盖 Collector 控制串行、声明的 callback 并发、Storage 单 lane、非法重入、`quiesce` 成功/超时/迟返及 destroy 前静止证明；对批次成功、确定部分写入和未知后缀核对结果及载荷释放时间 |
| 接纳、身份与载荷 | 验证 Stream Sequence 持久区间预留跨重启不复用、Pipeline Admission Gate 与多 Stream 并发 commit 的唯一 cut、Descriptor 高并发切换，以及复制/所有权转移/共享持有组合的释放和资源计费；不允许 Gate pass 后因普通队列或内存不足失败 |
| 执行容量与启动事务 | 验证 Core 硬计费、Plugin 声明、共享外部资源三层模型和对象 cardinality 上限。`AllRequired` 必须在所有 Required Ready 后才 Open Admission；任一 Prepare 失败时不得已有 Required 业务记录。验证暂停释放额度后的 Resume 重新准入、插件挂起、迟返和超额输入 |
| 真实录制 | ROS 2 → MCAP 覆盖连续滚动、Descriptor 变化、Pause/Resume、写入失败及 PlotJuggler 样本验证。ROS 2 → rosbag2 SQLite3 验证目录级读回与标准回放。MQTT → MCAP 验证明确定义的 Topic、Broker 晚启动及 JSON 原始载荷对账 |
| 性能与稳定性 | 参考压力画像覆盖 10 路原始 720p30 与 100 路高频关节混合，真实 ROS 2/MCAP/文件系统测接收、接纳、Writer/Commit backlog、逻辑/物理写入、提交滞后、CPU、RSS、物理载荷、p99/p99.9 和控制周期影响；验证 pending Segment/bytes 上限，并通过 72h 工程预资格和 168h 生产画像 |

SML 的第一个任务是建立小型、可重复的选型探针。只覆盖 Task 核心转换和两个独立实例，同时比较手写枚举方案的可读性和编译代价。接入生产目标之前须记录实际结果、完整提交和运行配置。

## 9. V1 引入顺序

1. **先完成 F0 Core Contract Closure。** 冻结 Identifier Domain 与 Stream Sequence 持久区间、Boot Reconciliation、Reset 单一权威、Pipeline Admission Gate、AllRequired Prepare Barrier、Deployment Generation/Effective Configuration、三层资源与 metadata cardinality、插件并发/重入、Host Boot/SourceSequence scope、Writer/Commit lane 和统一失败模型；同时锁定 Descriptor、Payload、Storage 基础合同，并用可控组件完成并发、崩溃和资源不变量测试。
2. **实现 ROS 2 → MCAP 核心闭环。** 通过 `rodaq` 模块正式控制和查询接口验证有界接纳、分片、Pause/Resume、独立回放、Descriptor 变化及完整资源结算。同步 Batch 必须直接参加真实性能资格；如果不能满足声明画像，异步 Storage 能力在 V1 发布前补齐。
3. **完成持久 Task 与长期录制能力。** 实现 Task Control Record、恢复记录、多任务、连续与滚动录制、资源计划准入、频率/流量/积压快照、历史查询、自动恢复、回收和 Reset，覆盖磁盘保护、Unknown 写入、进程重启和持久文件提交阶段故障。
4. **完成生产性能与长期资格。** 在目标硬件使用真实 ROS 2、MCAP、文件系统和磁盘执行相机、关节与混合画像，归档 CPU、RSS、线程/FD、物理内存、逻辑/物理吞吐、p99/p99.9、提交滞后、控制周期影响、72h 和 168h 结果。
5. **再接入本机应用与替换后端。** `rodaq` 模块通过前述门槛后设计 HTTP 路由和 Web；验证 MQTT → MCAP、ROS 2 → rosbag2 SQLite3、PlotJuggler 读回及源码树外插件示例，形成发布支持矩阵。

## 参考

- [ros2_bag 的 `next` 分支技术选型 §4.3、§5.7–§5.8](https://github.com/SiYueY/ros2_bag/blob/3506ab1108ee866c322948237d82a8b7f23836fd/docs/system_technology_selection.md#L373) 和 [Server 设计](https://github.com/SiYueY/ros2_bag/blob/3506ab1108ee866c322948237d82a8b7f23836fd/docs/server.md)。
- [Drogon v1.9.14](https://github.com/drogonframework/drogon/tree/3aae769cc405cf5abb9d477675c6430924f02c7c)。
