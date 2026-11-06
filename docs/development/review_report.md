# RoDAQ V1 全量设计与实现审查报告

## 1. 审查目的

本报告对 RoDAQ V1 当前设计与 GitHub `develop` 分支实现进行系统性审查。

审查目标不是判断“代码能不能运行”，也不是仅检查当前 M2 是否通过现有测试，而是以以下标准判断整个系统是否具备继续进入真实 ROS2 / FastDDS / MCAP 后端开发的工程基础：

- 面向实际工业机器人长期运行，而非实验性或玩具系统；
- 明确的 bounded resource model；
- 可证明的生命周期、并发、ownership 与 failure semantics；
- Stable Plugin ABI；
- 高吞吐混合采集负载；
- CPU-only；
- C++17 / GCC 11；
- Ubuntu 22.04 构建基线；
- Ubuntu 22.04 / 24.04；
- amd64 / arm64；
- AppImage 发布；
- 能直接复用成熟开源实现时优先复用；
- 无法直接复用时必须基于成熟、大型、生产级开源项目进行源码级参考，而不是自行闭门设计。

本次基线以 GitHub `develop` 当前已提交状态为准，不把本地尚未提交的修订视为已经完成。

---

# 2. 总体判断

当前 RoDAQ 的问题不能归结为单纯的“实现没做完”，也不能归结为“原有架构需要推倒重来”。

准确结论为：

| 层级 | 当前判断 |
|---|---|
| Canonical / Plugin / Admission 核心架构方向 | 基本正确，可以保留 |
| 详细工程设计 | 存在若干结构性缺口与未冻结边界 |
| M0 Stable ABI | 主体完成，但调用级 ABI firewall 仍有遗漏 |
| M1 Runtime Primitives | 主体完成，但 Payload / Logging / Config 等 production foundation 未完全闭合 |
| M2 Fake E2E Runtime | 功能主体完成，但 formal closure 不成立 |
| M3 ROS2 + MCAP | 不应在当前基础上正式展开 |
| M4 FastDDS + MCAP | 尚未进入 |
| V1 Production Qualification | 未达到 |

当前更合理的里程碑状态应为：

```text
M0 Stable ABI
    substantially complete

M1 Runtime primitives
    correctness foundation substantially complete
    production foundation not fully closed

M2 Fake End-to-End Runtime
    functional implementation substantially complete
    formal closure NOT COMPLETE

M3 ROS2 Acquisition + MCAP
    NOT STARTED

M4 FastDDS Acquisition + MCAP
    NOT STARTED

M5 Production Qualification
    NOT STARTED

V1
    NOT QUALIFIED
```

现有 M2 的功能主体确实已经完成较多，但已有审查证据表明 bounded Stop、Plugin operational state、ABI input/output validation 等 Exit Gate 尚未真正闭合，因此不能维持无条件 PASS。

---

# 3. 当前需要保留的核心架构

以下设计方向目前没有足够证据要求推翻。

## 3.1 Canonical Data Model

保留：

```text
Record Identity
    = (StreamId, AcceptanceSequence)

StreamId
    = logical stream identity

StreamDescriptorId
    = immutable interpretation contract
```

Schema、PayloadEncoding、TimestampLayout、StreamName 等解释层变化产生新的 StreamDescriptor，而不改变逻辑 StreamId。

这一方向适合后续 ROS2、FastDDS 以及非 ROS2 数据源扩展。

---

## 3.2 Runtime 作为唯一 Acquisition / Storage 中介

继续保持：

```text
Native Source
    ↓
Acquisition Plugin
    ↓
RoDAQ Runtime
    ↓
Storage Plugin
    ↓
Persistent Artifact
```

Acquisition 与 Storage 不直接通信。

Runtime 继续拥有：

- Admission；
- AcceptanceSequence；
- Payload lifetime；
- ordering；
- Storage dispatch；
- completion；
- lifecycle；
- failure convergence。

---

## 3.3 Stable C Plugin ABI

以下方向继续保留：

- Stable C ABI；
- appendable structs；
- `struct_size`；
- minimum prefix；
- family-specific Acquisition / Storage protocols；
- Runtime Service Registry；
- Plugin interface query；
- DSO / instance lease；
- symbol provenance；
- C++17 authoring facade。

不应改回：

- C++ virtual ABI；
- STL crossing Plugin boundary；
- one generic plugin interface；
- direct Acquisition → Storage coupling。

---

## 3.4 Accepted no-drop

继续保持：

```text
Gate Pass
    ↓
AcceptanceSequence commit
    ↓
Accepted publication
```

Gate Pass 之后不能再因普通资源压力 silently drop。

因此 correctness-critical Dispatch transport 必须在 Gate Pass 前获得真实可发布容量。

这一要求也意味着：不能为了使用 third-party queue 而放弃 physical capacity guarantee。

---

## 3.5 Payload / Persistence 双完成轴

继续保留：

```text
Payload:
    Pending
    Consumed

Persistence:
    Pending
    Succeeded
    Failed
    Unknown
```

Payload 可以在 persistence 尚未 terminal 时提前释放，是 camera 等 multi-MB Payload 场景的重要基础。

---

# 4. 已确认的高优先级实现缺陷

## A-01 — P0 — Plugin Payload release 可以使 Runtime Stop 永久阻塞

Storage Completion Worker 在 terminalization 后最终析构 Payload。

对于 Adopt / Retain Payload：

```text
Payload::~Payload()
    ↓
PayloadRelease callback
    ↓
Plugin / native SDK code
```

该 callback 当前直接运行在 Completion Worker 上。

Runtime Stop 当前顺序为：

```text
CloseCallbacks()
    ↓
JoinCompletion()
    ↓
WaitOperationsIdle(deadline)
```

`JoinCompletion()` 自身没有 deadline。

因此：

```text
Plugin release() 卡住
    ↓
Completion Worker 不退出
    ↓
JoinCompletion() 永不返回
    ↓
后面的 Stop timeout 永远没有执行机会
```

这直接破坏：

```text
bounded Stop
bounded failure convergence
Runtime destructor convergence
```

现有 Fake release 通常只是普通内存释放，因此测试通过并不能覆盖该失败模式。该缺陷已经被当前审查明确识别。

### 修复要求

必须重新冻结 Plugin-origin Payload release contract：

- release 是否允许阻塞；
- Runtime 如何监督 potentially blocking release；
- timeout 后如何保留 Plugin instance / DSO / native owner；
- Completion Worker 是否允许直接执行不可信 Plugin callback；
- Stop deadline 是否覆盖所有 release execution。

不得仅通过增加更长 timeout 掩盖问题。

---

# 5. A-02 — P1 — ABI pointer/count 输入校验顺序不安全

当前部分 timestamp ABI 校验逻辑：

```text
pointer/count pair
    ↓
直接按照 Plugin 提供的 count 遍历
    ↓
之后才在 Runtime 更深层检查 V1 maximum
```

这违反 ABI firewall 的基本顺序。

正确顺序必须是：

```text
1. scalar/count upper bound
2. pointer/count structural validation
3. element dereference
4. element semantic validation
```

此外当前存在类似：

```cpp
timestamp_key_count + 1U > MaxSlots
```

的 32 位加法，`UINT32_MAX` 时可能 wrap。

这一问题不仅应修 timestamp，而应该扩展成一次完整 ABI `pointer + count` 审查。

需要覆盖：

```text
TimestampKey arrays
TimestampValue arrays
all variable-size ABI arrays
string pointer + size
byte pointer + size
appendable array outputs
```

当前报告已经确认 timestamp 路径存在这一问题。

---

# 6. A-03 — P1 — Acquisition Plugin operational state 未真正实现

当前 `report_plugin_state()` 对：

```text
AVAILABLE
RECOVERABLE_FAILURE
TERMINAL_FAILURE
```

没有建立完整 Runtime operational state。

实际上只有：

```text
TERMINAL_FAILURE
+
diagnostic != nullptr
```

会触发 Runtime failure。

因此存在：

```text
RECOVERABLE_FAILURE
    → normal Submit 仍可能继续

AVAILABLE
    → 没有 recovery state transition

TERMINAL_FAILURE + null diagnostic
    → Runtime 仍可能返回 OK
```

当前设计中已经区分：

```text
operational state
!=
ordinary diagnostic
```

但实现没有兑现。

### 应形成正式状态机

至少需要明确：

```text
Available
RecoverableFailure
TerminalFailure
```

如何影响：

- normal Submit；
- discovery；
- recovery；
- Binding readiness；
- Plugin-local failure domain；
- Runtime-wide failure；
- metrics / diagnostic；
- restartability。

---

# 7. A-04 — P1 — Plugin destroy 仍可无限阻塞

正常 Plugin instance 析构直接调用：

```text
destroy(instance)
```

而 Runtime 最后的 instance reset 会同步触发该调用。

因此即使：

```text
start
stop
flush
close
```

都被 deadline supervision 包围，

最终：

```text
destroy()
```

仍可以绕过整个 Stop timeout。

这是第二条无界 Runtime teardown 路径。

设计必须明确：

```text
destroy() is guaranteed bounded/non-blocking
```

或者：

```text
destroy() is supervised / quarantinable
```

不能只依赖 Plugin 实现者遵守约定。

---

# 8. A-05 — P1 — Storage slot 回收为 O(N)

当前 `ReturnIndexLocked()` 使用 free-list 线性搜索防止 duplicate free。

这导致 operation terminalization 可能具有：

```text
O(dispatch_capacity)
```

复杂度。

如果：

```text
100,000 records/s
+
10k~20k slots
```

作为 small-record stress / production profile，该实现不具扩展性。

这一问题已经在当前审查中明确识别。

### 正确方向

继续使用：

```text
preallocated slot array
+
fixed-capacity index ring
```

即可。

但 slot lifecycle 必须自己证明：

```text
Free
→ Reserved
→ Ready
→ InFlight
→ Terminalizing
→ Free
```

通过：

```text
state
+
generation
```

实现 O(1) double-return detection。

不需要为了修复这一问题引入新的并发队列库。

---

# 9. A-06 — P1 — PayloadPool 资源模型不适合目标 workload

当前：

```text
one block_size
+
one block_count
```

不能有效支持：

```text
~800 B joint record
80 KB aggregated RobotState
multi-MB camera image
```

同时存在。

如果大 Payload 决定 block size，小 Payload 会造成严重 physical capacity waste。

该问题不仅是 `payload_pool.cpp` 的实现问题，而是整个资源模型的问题。

### 应整体修改

```text
DAQConfig
    ↓
PayloadPoolConfig
    ↓
size classes
    ↓
Admission reservation
    ↓
Copy materialization
    ↓
StatsAccumulator
```

至少需要观察：

```text
logical payload bytes
physical reserved bytes
objects per class
used blocks per class
per-class HWM
allocation failure per class
```

V1 推荐结构：

```text
small
4 KiB
16 KiB
64/128 KiB
256 KiB
1 MiB
4 MiB
large path
```

具体 class 值通过 benchmark 冻结。

---

# 10. A-07 — P1 — Logging production design 没有落地

设计要求：

```text
Runtime-owned LoggingContext
    ↓
bounded queue
    ↓
LoggingBackend
    ↓
SpdlogBackend
```

并定义：

- record limit；
- byte limit；
- drop-new；
- drop metrics；
- flush；
- shutdown；
- Plugin routing；
- recursion protection；
- backend failure containment。

当前实现仍只是同步 LoggerSink facade，并且 Plugin common log / diagnostic 路径没有真正接入 LoggingContext。

因此 Logging 属于：

```text
missing Runtime production foundation
```

而不是 M5 可选优化。

---

# 11. A-08 — P1 — Runtime 没有形成可交付安装接口

当前安装包主要覆盖：

```text
foundation
public API primitives
Plugin SDK
headers
```

但没有完整导出：

```text
rodaq_runtime
runtime application API
runtime dependencies
```

因此：

> 当前安装后的 RoDAQ 可以支持 Plugin authoring，但不能形成完整 DAQ Application consumer。

该问题已被现有审查确认。

这同时暴露一个设计缺口：

## DAQ Application API 尚未真正冻结

需要明确：

```text
Runtime
DAQConfig / DAQConfig
Configure
Start
Stop
Stats
Failure
Session
Plugin topology/configuration
```

哪些属于正式 public API。

---

# 12. A-09 — P2 — Admission waiter 对 eligibility invalidation 响应不及时

当前资源等待主要响应：

```text
resource release
gate close
deadline
```

Source / Stream availability 变化以及部分 Binding invalidation 不会立即唤醒等待者。

最终 revalidation 可以防止错误 Accepted，因此当前不是主要 correctness corruption。

但它会造成：

```text
不必要的等待
reaction latency
Stop / switch latency
timeout 依赖
```

所以保持 P2 较合理。

应进一步设计：

```text
eligibility epoch / generation
+
wait interruption
```

而不是简单到处 `notify_all()`。

---

# 13. A-10 — P1 — Storage submit output ABI firewall 不完整

当前 Runtime 对 Storage submit output 的主要检查集中在 enum state。

还需要统一校验：

```text
struct_size
reserved
payload_state
persistence_state
diagnostic presence
diagnostic/state consistency
axis combination
```

并 fail closed。

这属于 M0 ABI infrastructure 与 M2 runtime call-site validation 之间尚未闭合的边界。

应扩展为完整：

```text
Runtime → Plugin input
Plugin → Runtime output
Plugin → Runtime callback
```

三向 ABI semantic firewall audit。

---

# 14. A-11 — P3 — Handle generation wraparound

Activation generation 使用更宽的 generation，而部分 Handle generation 最终依赖较窄值。

虽然实际达到 wrap 需要非常长的运行时间，但工业系统不能依赖 silent wrap。

合理策略是：

```text
generation exhaustion
    ↓
retire slot / refuse new activation
```

而不是：

```text
wrap
→ stale handle 重新合法
```

该问题不阻塞近期功能开发，但应在 V1 hardening 前解决。

---

# 15. 设计层问题 D-01 — 性能 workload 还没有完整冻结

标准场景：

```text
10 × 1280×720 × 30 FPS camera

100 joints
× 100 parameters
× 1 kHz
```

仍必须明确 Record 粒度。

Joint 至少有：

```text
Aggregated RobotState
    1,000 records/s
    ≈ 80 KB/record

Per-joint Record
    100,000 records/s
    ≈ 800 B/record
```

两者 Payload 带宽接近，但 Runtime object pressure 相差约 100 倍。

不能通过把原生 per-joint 数据擅自聚合成一个 RobotState 来“满足性能”。

正确方式是同时定义：

```text
production profile
stress profile
source-native profile
```

附件已经指出 workload 粒度是当前设计主要缺口之一。

---

# 16. D-02 — Capacity sizing 必须成为正式 contract

当前不能继续依赖：

```text
64
128
256
```

这样的经验默认值。

至少应冻结：

```text
record_capacity
    >= peak_record_rate
       × tolerated_downstream_stall
       × safety_factor

byte_capacity
    >= peak_payload_rate
       × tolerated_downstream_stall
       × safety_factor
```

Payload size-class 还要独立计算：

```text
class_objects
physical_bytes
```

这一缺口也已经在审查报告中明确提出。

---

# 17. D-03 — oneTBB 应重新定位

当前没有证据要求 oneTBB 成为 V1 Runtime Core correctness dependency。

尤其：

```text
Accepted no-drop
physical reservation
no post-Gate allocation
```

并不能由普通 generic concurrent queue 自动满足。

因此：

```text
oneTBB
    = optional concurrency / task-parallel candidate
```

比：

```text
mandatory Runtime Core dependency
```

更合理。

只有后续出现真实：

```text
parallel compression
parallel transformation
CPU worker scheduling
```

等需求再引入。

---

# 18. D-04 — mimalloc 应保持 optional / evidence-driven

Data Plane 正确设计本来就应该：

```text
preallocate
+
bounded pools
+
steady-state no general heap
```

因此 mimalloc 不是当前 100k small record / camera workload 的核心解法。

应：

```text
optional backend
benchmark only
no global override by default
```

除非真实数据证明 general heap fragmentation / allocator contention 是瓶颈。

---

# 19. D-05 — Runtime Public Application API 缺失

Plugin API 已经非常完善。

但应用侧：

```text
Application
    ↓
Runtime
    ↓
Configure
Start
Stop
Stats
Failure
```

没有形成同等清晰的 public contract。

需要冻结：

```text
public headers
source compatibility
binary compatibility expectations
configuration model
ownership
thread safety
lifecycle
install/export
```

否则 M3 做出来以后最终应用程序仍然依赖 private `src/` implementation headers。

---

# 20. D-06 — Lock hierarchy 缺失

当前至少涉及：

```text
control mutex
stream admission mutex
binding transition mutex
StoragePipeline mutex
ResourceBudget mutex
Quarantine mutex
Plugin-local locks
```

M2 已经开始处理 reentrancy / lock isolation，但还缺少正式：

```text
lock ownership table
lock order
external call under lock prohibition
callback reentry rule
```

M3 一旦引入 ROS2 / FastDDS callback，锁关系会明显更加复杂。

因此 lock hierarchy 应作为正式设计/代码审查 gate。

---

# 21. D-07 — Failure taxonomy 需要与 disposition 对齐

当前 ErrorCode 基础可以保留。

但还必须统一：

```text
Plugin protocol violation
ABI contract violation
Plugin timeout
Storage fatal failure
Artifact failure
Runtime correctness failure
Resource exhaustion
ordinary PreAdmission rejection
```

分别如何决定：

```text
primary failure
secondary error
quarantine
session termination
process restartability
next activation eligibility
```

避免所有异常路径最终都退化成：

```text
InternalError
```

---

# 22. D-08 — Reference-driven implementation 尚未形成工程 Gate

以下 RoDAQ-owned 模块本来就不适合直接交给一个 generic library：

```text
Admission
ResourceBudget
Payload lifetime
Registry/Handles
Lifecycle/Quiescence
Plugin ABI
```

但这不意味着从零自行设计。

必须建立正式 source-reference audit。

推荐主参考：

```text
ResourceBudget / Admission
    gRPC ResourceQuota
    Aeron
    Fast DDS ResourceLimits

PayloadPool
    iceoryx MemPool
    Fast DDS PayloadPool

Registry
    iceoryx FixedPositionContainer
    LLVM

Structural Arena
    Protobuf Arena
    LLVM BumpPtrAllocator

Lifecycle / Quiescence
    eCAL
    rosbag2
    gRPC
    PipeWire

Plugin
    PipeWire SPA
    PlotJuggler
    LLVM
    class_loader / pluginlib
    gz-plugin
```

每个模块必须记录：

```text
reference project
version / commit
source files reviewed
mechanism adopted
mechanism rejected
reason
RoDAQ-specific differences
tests / benchmark
```

---

# 23. 工程实现质量问题

## 23.1 `active_runtime_state.cpp` 职责过度集中

问题不是简单的“2616 行太长”。

而是该文件同时承担：

```text
ABI Runtime Service endpoints
Canonical registration
Availability
Binding
Descriptor
Projection
Clock
Plugin health
Failure
Admission-related control state
```

应先分析锁域和生命周期，再按责任拆分。

不能机械按照类名拆文件。

---

## 23.2 DAQConfig 正在演化为 giant configuration object

当前 DAQConfig 已经承担：

```text
capacity
timeout
Plugin path
Plugin config
Storage
Session
Payload configuration
```

后续还需要：

```text
Logging
size classes
MCAP
ROS2
FastDDS
resource policies
```

应控制其增长。

可合理拆分为：

```text
DAQConfig
ResourceConfig
PayloadPoolConfig
LoggingConfig
AcquisitionPluginConfig
StoragePluginConfig
```

但应坚持简单，不引入无意义配置框架。

---

# 24. Observability 尚未闭合

目前有 HdrHistogram wrapper，不代表 Observability 已完成。

至少应该定义并实现：

```text
accepted records
rejected records by reason
pre-admission drops
admission wait P50/P95/P99/P99.9

payload logical bytes
physical bytes
size-class HWM

dispatch HWM
completion HWM
storage inflight HWM

handoff latency
payload consumed latency
persistence latency

Plugin health

logging queue occupancy
logging drops

Stop phase latency
quarantine count
```

HdrHistogram_c 可以继续使用。

但 instrumentation contract 应由 Runtime 设计决定。

---

# 25. 第三方依赖最终分类建议

| Dependency | 建议 |
|---|---|
| expected-lite | 保留 |
| HdrHistogram_c | 保留 |
| spdlog | 保留并真正接入 Logging backend |
| fmt | 与 spdlog 配套保留；是否独立直接使用可再评估 |
| yaml-cpp | 保留，用于 Config frontend |
| oneTBB | 从 Core mandatory 降为 optional candidate |
| mimalloc | optional benchmark backend |
| GoogleTest | development-only |
| Google Benchmark | development-only |
| mcap-cpp | M3 应直接 pin 并复用 |
| ROS2 libraries | Plugin 内使用 |
| Fast DDS / Fast CDR | Plugin 内使用 |

对于 100k 小 Record，优先级不是 oneTBB 或 mimalloc，而是：

```text
size-class / zero-copy Payload
O(1) slot management
Storage batching
capacity sizing
real MCAP throughput
```

这一结论与现有审查报告的依赖建议一致。

---

# 26. 测试与 Qualification 缺口

现有测试覆盖了大量：

```text
ABI
lifecycle
ordering
completion
race
failure
```

这是项目当前比较强的部分。

但 V1 仍缺：

```text
blocking PayloadRelease
blocking destroy
malicious / invalid ABI count
UINT32_MAX boundary
Plugin health transition matrix
Storage output malformed matrix
multi-size PayloadPool
per-class exhaustion
large + small fairness
real MCAP
real ROS2
real FastDDS
AppImage
arm64
long-duration soak
disk full
filesystem disappear
write failure
partial Artifact
power-loss/crash recovery
```

因此：

```text
89/89 CTest
```

只能证明：

> 当前测试集合通过。

不能证明：

> M2 所有设计契约已经关闭。

更不能证明：

> V1 production ready。

---

# 27. Performance Qualification 应分四类

## Production camera

```text
10 × 1280×720 × 30 FPS
source-native encoded payload
```

必须记录：

```text
average/max bytes
codec
timestamps
Stream count
Storage batching
```

## Raw camera stress

```text
10 × RGB8 1280×720 × 30 FPS
≈ 791 MiB/s
```

## Production joint

```text
100 joints
× 100 binary64 parameters
× 1 kHz
aggregated/native production grouping
```

## Small-record stress

```text
100 joint streams
× 1 kHz
≈ 100,000 records/s
```

另外必须有：

```text
camera + joint mixed
```

而不能单独通过各自 benchmark 后假设混合一定成立。

---

# 28. Milestone 修正建议

当前 GitHub 状态不应继续解释为：

```text
M2 PASS
M3 ACTIVE
```

建议改为：

```text
M0
    PASS with follow-up ABI semantic firewall closure

M1
    CORE PRIMITIVES COMPLETE
    production foundation closure inherited by M2 convergence

M2
    ACTIVE
    V1 Runtime Foundation Convergence

M3
    NOT STARTED

M4
    NOT STARTED

M5
    NOT STARTED
```

M2 Exit Gate 应增加：

```text
bounded release/destroy
Plugin health state
complete ABI semantic firewall
multi-size PayloadPool
LoggingContext
ConfigLoader
Runtime install/export
dependency qualification
reference audit
standard workload capacity qualification
```

---

# 29. 修复原则

本轮修复不是“保持当前代码结构，逐条打补丁”。

也不是：

> 因为发现问题而重新设计整个系统。

正确原则为：

```text
保留已经证明合理的核心架构

重新打开存在证据问题的详细设计

设计修订先于代码实现

能直接复用成熟 OSS 的地方直接复用

无法直接复用的 RoDAQ-specific 语义
必须先研究成熟生产项目源码

未发布阶段不维持不必要兼容层

删除错误实现
优于在错误实现外继续加 adapter

所有修复一次完成
之后做一次独立全量复审
```

---

# 30. 推荐修复阶段

```text
Phase 0
Freeze audit findings

Phase 1
Design convergence

Phase 2
P0/P1 lifecycle + ABI correctness

Phase 3
Resource model + PayloadPool

Phase 4
Logging + Config + Runtime delivery

Phase 5
Dependency cleanup

Phase 6
Reference-driven implementation review/refactor

Phase 7
M0–M2 complete qualification

Phase 8
Independent review

Phase 9
Only then enter M3
```

---

# 31. 最终结论

RoDAQ 当前不是“设计已经完全正确、代码只差几个实现”。

也不是“架构失败，需要推倒重来”。

准确判断是：

> **Canonical、Plugin ABI、Admission、Payload ownership、Storage 双完成轴等核心架构方向基本正确；但详细 production design 尚存在若干结构性缺口，同时 M0–M2 当前实现已经确认存在 Stop 收敛、ABI firewall、Plugin operational state、Payload resource model、Logging、安装交付等实际问题。**

因此当前最佳策略不是进入 M3，也不是再做一个局部的 “M2 Closure Fix”。

应先完成一次：

# RoDAQ V1 Design & M0–M2 Implementation Convergence

其目标是：

> **一次性把设计、实现、依赖、资源模型、测试与交付重新收敛到同一个 production baseline，再重新判定 M2 PASS。**

---

## 32. Convergence Evidence Ledger

This ledger closes the review findings at implementation level. Final
qualification status is recorded in `status.md` and must not be inferred from
this table alone.

| ID | Severity | Design reference | Implementation evidence | Acceptance evidence | Status |
|---|---|---|---|---|---|
| A-01 | P0 | D-045, runtime/payload | bounded Release Executor; each worker has completion state; Stop transfers blocked workers to `PluginQuarantine` | Runtime pipeline blocked-release test; quarantine test | Implemented |
| A-02 | P1 | D-046, canonical profile | all timestamp counts bounded before dereference; pointer/size parsing checked; `rodaq_error` buffer validated | `UINT32_MAX` timestamp tests; malformed diagnostic matrix | Implemented |
| A-03 | P1 | D-047, acquisition/runtime | per-Acquisition health; routing invalidation; Available requires new Descriptor/Binding; terminal failure synthesizes error | recoverable, recovered and terminal health tests | Implemented |
| A-04 | P1 | D-045, Plugin system | explicit supervised `destroy()`; implicit production destroy removed; timed-out call retained with activation | blocked-destroy bounded Stop test | Implemented |
| A-05 | P1 | D-048, runtime | state/generation slot return; no free-list scan in normal path | storage slot benchmark; double/late completion tests | Implemented |
| A-06 | P1 | D-049, payload/admission | ordered isolated size classes; Pre-Gate block/logical/release reservation; per-class stats | PayloadPool tests; mixed camera+joint runtime profiles | Implemented |
| A-07 | P1 | D-051, logging | fixed record/byte async LoggingContext; drop-new; Plugin routing; backend exception containment | logging tests; Runtime Plugin logging routing | Implemented |
| A-08 | P1 | D-050/D-053, runtime design | installable shared Runtime; public Config API; hidden production symbols; package consumer | install smoke; `DT_NEEDED` and dynamic symbol checks | Implemented |
| A-09 | P2 | D-045/runtime | eligibility generation plus waiter notification outside structural locks | StoragePipeline eligibility-interruption test | Implemented |
| A-10 | P1 | D-046, storage | Storage submit result semantic matrix; malformed diagnostics fail closed | storage output/input matrix; malformed callback test | Implemented |
| A-11 | P3 | D-046, canonical profile | activation generation exhaustion rejected; Registry slot retirement before wraparound | generation exhaustion tests | Implemented |
| A-12 | P0 | D-045/D-048, runtime | completion/release wait no longer treats Stop as submit reconciliation; timeout quarantines instead | completion-before-return and runtime failure tests | Implemented |
| A-13 | P1 | D-045, payload | Payload move transfers Plugin lifetime guard | lifetime-guard move test | Implemented |
| Q-01 | P1 | runtime observability | admission, handoff, Payload terminal, Persistence terminal and Stop HdrHistogram summaries | Runtime latency assertions; capacity benchmark counters | Implemented |
| Q-02 | P1 | D-052 | camera, joint, mixed, slot and release-worker profiles; offered-schedule enforcement | Release capacity runs | Implemented |
| D-01 | P1 | system design | encoded/raw camera and aggregated/per-joint/source-native profiles frozen | benchmark source and capacity report | Implemented |
| D-02 | P1 | system/runtime | formula-driven record/class capacities and documented stall/safety values | benchmark capacity derivation | Implemented |
| D-03 | P2 | reference audit | oneTBB evaluated, no V1 Core use, removed from baseline | dependency inventory; runtime-only build | Implemented |
| D-04 | P2 | reference audit | mimalloc evaluated, no measured DTO bottleneck, removed from baseline | dependency inventory; runtime-only build | Implemented |
| D-05 | P1 | D-050 | `DAQConfig`, `ResourceConfig`, `AcquisitionPluginConfig`, `StoragePluginConfig`, installable Candidate | Runtime config tests; install consumer | Implemented |
| D-06 | P1 | runtime design | lock domains and callback-under-lock prohibition documented; reentry tests | projection-under-Plugin-lock and concurrency tests | Implemented |
| D-07 | P1 | decisions/D-047 | primary/secondary history, terminal health, invariant failure paths made explicit | failure history and contract-violation tests | Implemented |
| D-08 | P2 | reference audit | pinned source-level review recorded for all required mechanism families | `reference_audit.md` | Implemented |

## M2 Final Closure Fix — implementation and qualification report

**Date:** 2026-11-05
**Baseline:** `413ba651f7d9fb1611ec294d4cc8e9799d4363b4`
**Implementation:** `fe4f938adef1680021201334dae0c434f67d4011` — `fix(runtime): retain quarantine roots until worker join`
**Outcome:** FR-01 / FR-02 / QF-01 COMPLETE; M2 PASS; M3 NOT STARTED.

Implementation commits:

- `9467e0a`: automatic failure supervisor/ownership, Dispatch cutoff,
  Descriptor escalation, quarantine reaping and initial regression coverage.
- `fe4f938`: actual Join barriers for cleanup continuations and transferred
  release workers, capture destruction before completion publication, and the
  Runtime-destruction/captured-resource regressions.
- The following evidence commit updates development state and measured results;
  it changes no executable code.

The preceding historical review remains intact. The previous claim of 9/9 TSan
closure at 413ba65 is superseded: the pipeline previously disabled thread-leak
reporting. This qualification keeps thread-leak and data-race checks enabled.
No Stable C ABI layout, field, vtable, Service ID, Result v5 contract, Canonical
identity/sequence rule, or dependency pin changed.

### FR-01 — automatic failure convergence

The original failure path only recorded Failed and closed Admission, leaving
cleanup dependent on application Stop. `src/daq/failure_mailbox.hpp` now
provides a shared one-bit mailbox with no Runtime pointer or activation owner.
`DAQ::Impl::failure_loop()` owns one joinable supervisor per Runtime.
`RequestFailure()` records/cuts off/notifies and performs no Plugin I/O or Join.
Runtime destruction closes the mailbox before joining its supervisor; a late
quarantined callback cannot access a destroyed Runtime.

`LifecycleOwner` serializes Start, public Stop and automatic cleanup logically,
releasing the lifecycle mutex across Plugin I/O and worker barriers. Both Stop
and the supervisor call `stop_internal()`. Starting failures retain the existing
activation-abort path and ACTIVATION_ABORT disposition. A failure during Stop
belongs to that owner. Completed cleanup stays Failed and repeated Stop cannot
repeat quiesce/stop/close/destroy.

`StoragePipeline::BeginFailureCutoff()` stops future handoff authorization and
classifies never-handed-off queued work as PersistenceFailed. Gate-passed work
published after cutoff still becomes Accepted and receives the same independent
persistence classification. The acquire load immediately before submit is the
handoff-entry authorization point; only the failure path takes the extra queue
lock. A previously authorized call retains its original thread and Payload until
its actual return. Timeout alone proves neither failed handoff nor consumption.
HandoffAccepted completions retain their independent axes. After a successful
close proves no future access/callback, `ResolveAfterClose()` drains staged
completions and marks unconfirmed persistence Unknown, never Succeeded.
Runtime statistics expose terminal persistence counts for this qualification.

Timed-out calls retain their recorded result and cleanup continuation under the
existing activation root budget. The same cleanup implementation resumes after
progress is proven, without reissuing a timed-out destructive call. Failed
quiescence or permanently blocked code remains retained with native resources
and DSO leases; a safe late return allows Join and root-capacity recovery.

### FR-02 — dynamic Descriptor FAILED escalation

`ActivationState::ReconcileDescriptor()` reports readiness errors through
RequestFailure after releasing structural locks, regardless of startup-critical
status. The original Storage diagnostic is preserved. FAILED, timeout and
malformed responses fail closed during Starting and Running; UNSUPPORTED stays
local, and existing Projection-dependent retry remains covered. A Descriptor
worker never performs its own cleanup or joins itself.

### QF-01 — eventual quarantine Join

Completed entries are claimed once under the manager mutex, then Joined and
released outside it. Capacity includes claimed entries until Join finishes.
Concurrent reapers cannot double-join; a worker reentering Reap skips itself.
One process maintenance thread reaps late Logging/control/release workers and
safe activation continuations without requiring another application API call.
That maintenance thread is itself joined, with a bounded final completion grace
period during process teardown.

Cleanup continuations and transferred Pipeline release workers now wait for a
separate actual Joined signal before root recovery. Supervised operations dispose
of their captured resources before publishing completion, so captured Plugin
destructors remain within the supervised deadline.

The Logging timeout test now releases its backend through RAII, uses deadlines,
reaps to its baseline count and proves final worker ownership is gone. Fake
Acquisition's injected quiescence-error path joins its own native fixture workers
before returning an operational error; Runtime still quarantines on that error.
No expectation was weakened and no sanitizer exception was added.

### Added regression coverage

The integration executable now has 68 tests (previously 60), including:

- FatalStorageFailureAutomaticallyConvergesWithoutStop (submit and background fatal).
- FailureCutoffDoesNotDispatchQueuedRecords (Accepted history, Failed counts,
  Retain release and zero final record/byte occupancy).
- HandoffWatchdogAutomaticallyStartsFailureConvergence (late successful handoff,
  queued cutoff, automatic cleanup and activation-root recovery without Stop).
- ConcurrentFailureAndStopHaveSingleCleanupOwner (exact lifecycle call counts).
- FailureDuringStartingUsesActivationAbortOnly (no duplicate cleanup).
- DescriptorWorkerFailureDoesNotSelfJoin (dynamic FAILED and preserved diagnostic).
- DynamicUnsupportedDescriptorRemainsStreamLocal.
- QuarantinedReleaseOutlivesRuntimeAndJoinsBeforeRootRecovery (no application
  observer retains the Pipeline; late release must Join before root reclamation).

Quarantine tests additionally cover concurrent Join/state destruction outside
manager locks, completion-publication reentry without self-Join, and a blocking
capture destructor that must finish before completion/Joined publication. Existing
Admission, Payload, Binding, Projection, Clock, ABI, install, DSO, failure-history
and completion-violation tests were retained.

### Qualification on implementation fe4f938

| Environment / configuration | Result |
|---|---|
| Ubuntu 22.04, GCC 11.4 Debug, warnings as errors | 93/93 CTest PASS |
| Ubuntu 22.04, GCC 11.4 ASan + LSan | 93/93 PASS; detect_leaks=1, halt_on_error=1 |
| Ubuntu 22.04, GCC 11.4 UBSan | 93/93 PASS; halt_on_error=1 |
| Ubuntu 22.04, GCC 11.4 Release, warnings as errors | 93/93 CTest PASS |
| Clang 14 TSan, existing required nine-target set | 9/9 PASS; thread-leak and race checks enabled |
| Ubuntu 24.04.4 offline container, GCC 13.3 | 93/93 CTest PASS, tests and benchmarks built |
| Release capacity, all existing Pipeline/Joint/Camera/Mixed/Slot/Release profiles | PASS; three repetitions for each baseline/current case |
| Runtime-only Release, tests/benchmarks and development dependency directories absent | Configure + full Runtime build PASS |
| clang-format 14 dry-run, git diff --check, Stable C ABI diff | PASS / PASS / unchanged |
| Native arm64 | DEFERRED under D-034 |

ASan/LSan and TSan used `setarch x86_64 -R`. LSan needed execution outside the
sandbox because its thread inspection is restricted there. The local Clang 14
installation selected an incomplete GCC installation; explicit GCC 11 standard
include/library paths were supplied only in the local CMake cache. No compiler
checks or sanitizer detections were disabled. The mandated TSan subset matches
`.github/workflows/m0-qualification.yml`; this is not a claim that all unrelated
Clang fixture targets were qualified.

Capacity CPU/latency/RSS/high-water comparisons and scheduling limits are in
`docs/development/capacity_qualification.md`. This is Fake Pipeline qualification;
MCAP/filesystem production throughput and long soak remain M3–M5 work.

No remote push or hosted workflow was performed, so this task has no new CI Run
URL. These results are actual local builds/tests on the stated environments,
not a claim about remote CI state. The subsequent documentation commit records
this implementation's evidence without changing executable code.

**Final assessment:** M0 PASS; M1 PASS; M2 PASS; M3 NOT STARTED. M2 Final Closure
Fix COMPLETE. No known remaining P0/P1 finding from this review; local required
qualification PASS. Native arm64 remains explicitly deferred.

Final closure counters: **Known P0 = 0; Known P1 = 0; Qualification = PASS
(local required matrix); M2 Final Closure Fix = COMPLETE**.


## M2 Ownership Closure and Quarantine publication hardening — 2026-11-05

### Baseline and previous hosted evidence

Local qualification was performed on the working tree based on
`develop@3adefb28cee14014d19fccf1fb98f1afc737cd85` before commit/push.
Qualification below describes that implementation; new hosted qualification
remains pending until its associated workflow completes and must not be
attributed to the previous hosted run.

The prior local implementation qualification on `fe4f938` remains historical
PASS. The subsequent [RoDAQ Qualification run 41968952350](https://github.com/SiYueY/rodaq/actions/runs/41968952350)
was independently checked through GitHub: push event, branch develop, HEAD
`3adefb28`, completed/success. Its seven non-arm64 jobs succeeded; native arm64
was skipped under D-034. Earlier statements that no hosted run was triggered
refer to the earlier implementation task, before that push.

### Closed findings

Source entries now retain immutable owner identity. Same-owner SourceId
registration is idempotent; a different Plugin claiming identical Canonical
bytes receives Conflict. Runtime does not rewrite SourceId bytes. ABI report
wrappers preserve the caller Plugin identity; Source/Stream mutations reject
unknown handles with NotFound and foreign handles with InvalidState before
mutation. Optional Source Submit ownership is checked before Payload acquisition,
returning PreAdmissionRejected/STALE_HANDLE with the existing independent
Payload disposition. Recoverable and terminal Plugin failure mark only that
Plugin's Sources Unavailable; health recovery requires explicit Source recovery.
Descriptor transport keys reject foreign and Runtime-owned ingress Clocks with
InvalidState; unknown Clocks remain NotFound, and the reserved ingress role
remains InvalidArgument.

Quarantine call and worker vectors were replaced with fixed 64-slot arrays and
the original shared 64-entry budget; activation roots remain bounded at 32.
Run reserves its budget and prepares state before thread creation, then moves
into a slot on timeout without allocation. RetainWorker checks capacity before
taking the caller's joinable thread and uses non-throwing moves. Completed entries
are still claimed under the mutex and Joined/destroyed outside it, with capacity
retained through destruction. This closes the retention allocation/temporary
joinable-thread destruction risk without claiming arbitrary global OOM recovery.

Stable C ABI and public/installable headers are unchanged. Failure supervisor,
Descriptor FAILED escalation, independent Storage completion axes and actual
Joined-before-root-reclamation contracts remain intact. No new dependency or
real backend implementation was introduced.

### Regression coverage

The integration executable contains 71 tests (previously 68). New double-Plugin
coverage checks Source registration idempotence/collision, foreign Source/Stream
reports, foreign/unknown/ingress Clock publication, and foreign Source Submit
under Copy, Adopt and Retain. Successful own-Source submissions verify both
Plugins remain usable. The recovery test synchronizes the healthy Plugin's
submission after the other Plugin's failure/rebinding, verifies Source remains
Unavailable after health recovery, then explicitly restores that Source.
Existing rollback and mixed-Payload tests now use distinct SourceIds. Mixed
capacity fixtures likewise declare distinct camera/joint SourceIds.

Quarantine coverage fills the shared budget with 32 calls and 32 workers,
checks rejected retention preserves the caller's thread, drains and reuses
both slot kinds, and retains existing concurrent/reentrant/exactly-once Join
coverage. An isolated new executable uses thread-local allocation failure
injection with linker wrapping, calibrated before the test; timeout publication
and worker retention perform zero guarded allocations. Sanitizer new/delete
interceptors remain active. The new executable is included in hosted TSan's
build target list and existing test selection.

### Working-tree qualification

| Environment / configuration | Final result |
|---|---|
| Ubuntu 22.04 / GCC 11.4 Debug / warnings as errors | 94/94 CTest PASS |
| GCC 11.4 ASan + LSan | 94/94 PASS; detect_leaks=1, halt_on_error=1 |
| GCC 11.4 UBSan | 94/94 PASS; no recovery from undefined behavior |
| GCC 11.4 Release / warnings as errors | 94/94 PASS |
| Ubuntu 24.04.4 offline container / GCC 13.3 | 94/94 PASS; tests and benchmarks built |
| Clang 14 TSan required subset, including new allocation test | 10/10 PASS; race and thread-leak checks enabled |
| Runtime-only Release / development dependencies absent | Full Runtime build PASS |
| All eight existing Release benchmark executables, three repetitions each | PASS; see capacity_qualification.md |
| clang-format 14 dry-run / git diff --check / public-header and Stable ABI diff | PASS / PASS / unchanged |

ASan/LSan and TSan used `setarch x86_64 -R`; no new suppressions or disabled
checks were introduced. UBSan builds retain `-fno-sanitize-recover=undefined`.
The local Clang cache uses explicit GCC 11 include/library paths as in the
previous qualification. Concurrent local matrix processes use isolated temporary
directories: separate PID namespaces can otherwise collide on PID-based fixture
filenames in the shared `/tmp`. The final UBSan/Release runs used separate
`TMPDIR` directories, with all original assertions intact.

Final local execution logs are `/tmp/rodaq-ownership-debug-tests.log`,
`/tmp/rodaq-ownership-asan.log`, `/tmp/rodaq-ownership-ubsan-tests.log`,
`/tmp/rodaq-ownership-closure-release-tests.log`,
`/tmp/rodaq-ownership-tsan.log`, `build/ownership-ubuntu24-tests.log`, and
`/tmp/rodaq-ownership-runtime-only.log`. These are transient host artifacts;
the results above are the durable qualification record.

**Assessment:** ownership/isolation findings closed; retention publication
hardening closed; Known P0 = 0 and Known P1 = 0 from this closure. M2 generic
Runtime locally qualified/frozen; M3 NOT STARTED.
At local qualification the new hosted result was pending; the subsequent
`be448c8` hosted qualification completed successfully at [RoDAQ Qualification #41975605291](https://github.com/SiYueY/rodaq/actions/runs/41975605291).
Native arm64: **DEFERRED under D-034**.


## C++ coding standards review and snake_case migration — 2026-11-05

### Scope and compatibility

The working tree is based on `develop@be448c81da075a64a4282d2123e321e77c66e13b`.
The prior commit's hosted qualification completed successfully at [RoDAQ Qualification #41975605291](https://github.com/SiYueY/rodaq/actions/runs/41975605291).
This review's code is locally qualified; no new hosted run has yet qualified it.

Project-owned free functions, static/member functions, DAQ Application
Candidate, SDK authoring interfaces, trampolines, tests, benchmarks and installed
consumers use snake_case. Representative migrations: `DAQ::start()` to
`DAQ::start()`, `AcquisitionPlugin::Bind()` to `bind()`,
`ConfigTree::Double()` to `float64()` (avoids the C++ `double` keyword), and
Registry/Projection `Register()` to `register_entry()` (avoids `register`).
No PascalCase source compatibility aliases were added.

D-056 supersedes D-033 only for C++ SDK source names. Existing C++ Plugin and
Application source must migrate; existing Runtime C++ binaries must be relinked.
The Stable C Plugin ABI headers, frozen V1 fixtures, C entry points and table
layouts are unchanged. C and C++ ABI/header-isolation, symbol, V1 fixture and
installed SDK/Application consumer tests passed. `nm` confirmed unchanged
`rodaq_get_acquisition_plugin`, `rodaq_get_storage_plugin`, and
`rodaq_plugin_abi_version` exports; Runtime C++ exports now use snake_case.

### Audit and handling

Clang-format 14 checked all 182 project-owned C/C++ files. Clang-tidy 14 checked
110 build translation units, 26 installable C++ headers, all eight C ABI headers
individually in C and C++ modes, and five installed consumer sources. Two
intentionally non-compiling negative fixtures are excluded from standalone lint;
their expected-failure compiler tests passed. Generated code and pinned
third-party sources are not linted as project code.

The CI style job enforces function/member/virtual-method naming, braces,
narrowing conversions, use-after-move, override, nullptr and C-style casts.
C++ constructors/destructors/operators, third-party overrides, GoogleTest,
generated symbols, frozen C ABI and GNU ld `__real__`/`__wrap__` hooks retain
their required names. Six narrow `bugprone-use-after-move` suppressions in tests
explain deliberate moved-from-state checks and the retention contract under
which a failed RetainWorker leaves its rvalue-reference thread with the caller.

The broader advisory scan also identified 14 moves of trivially-copyable values
and four potential copies. The moves express relinquishment and are not
correctness defects. Two copies explicitly test copy semantics; two own YAML
scalar/tag strings during conversion. No unprofiled optimization was made. Manual
review sampled Admission cancellation/locking, Quarantine retention/Join,
Payload release, Runtime lifecycle ownership, Plugin lease/unload, exception
boundaries and casts against their existing contracts. No new P0/P1 issue was
confirmed. Static checks and sanitizer tests cannot prove all concurrency or
lifetime properties; existing dedicated fault/contract tests remain necessary.

### Local qualification

| Configuration | Result |
|---|---|
| GCC 11 Debug / warnings as errors | 94/94 CTest PASS |
| GCC 11 ASan + LSan, detect_leaks=1 | 94/94 PASS |
| GCC 11 UBSan, no recovery | 94/94 PASS |
| GCC 11 Release | 94/94 PASS |
| Ubuntu 24.04 offline, GCC 13 | 94/94 PASS |
| Clang 14 TSan, race and thread-leak checks enabled | 10/10 PASS |
| Runtime-only Release without development submodules | build PASS |
| Release capacity, eight executables, three repetitions | PASS; details in capacity_qualification.md |
| clang-format 14 / seven clang-tidy checks / git diff --check | PASS |
| Stable C ABI headers, V1 fixtures and C entry points | unchanged / compatibility PASS |
| Native arm64 | D-034 DEFERRED |

Local ASan/LSan and TSan used `setarch x86_64 -R`; no new sanitizer suppression
was introduced. Local Clang 14 uses GCC 11 headers when compiler autodiscovery
is incomplete. Concurrent local matrices used separate `TMPDIR` directories to
avoid PID-based fixture collisions. Lint is reproducible with the `cpp-style` CI
job or `python3 tools/check_cpp_style.py --build-dir <build>`, after configuring
tests/benchmarks with `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, building and running
`rodaq.abi.install_smoke` once for generated SDK metadata.

The last post-qualification edits to test C++ source add only clang-tidy
suppression comments for those six intentional checks; executable statements and
behavior are unchanged. New hosted qualification is **PENDING** until these changes
are committed/pushed and the workflow finishes. M2 is locally qualified; M3 is
NOT STARTED.


## 2026-11-05 Declaration / implementation separation follow-up

Moved SDK non-template service wrappers, configuration and validation routines
into the existing static foundation library, and moved selected internal Runtime
methods into `.cpp` files. Header-only templates, `constexpr`, Plugin-exported
trampolines and public Runtime inline accessors required by current symbol
visibility are retained. The installed SDK links via `rodaq::sdk`; Stable C ABI
headers and Plugin C exports are unchanged. Consumers must link the SDK target
when using these moved methods.

After the move: Normal, Release, ASan/LSan, UBSan and Ubuntu 24 each 94/94;
Clang 14 TSan CI subset 10/10 with thread-leak detection; Runtime-only Release
build PASS; formatting and clang-tidy PASS (189 C/C++ files, 117 translation
units, 26 C++ headers, 8 C ABI headers in C/C++ modes, 5 install consumers).
All eight Release capacity benchmarks passed three repetitions; comparative
medians are recorded in `capacity_qualification.md`. A full Clang TSan build was separately attempted and encounters an existing
`-Wunused-const-variable` error for the null-descriptor fixture; it is outside
the defined TSan subset. New hosted qualification remains PENDING.


## 2026-11-05 C++ StatusCode convergence

`StatusCode` replaces the C++ `ErrorCode` taxonomy and includes `Ok`, with
explicit Stable C ABI status mappings. `Error::status_code()` replaces `code()`;
constructing Error with Ok or an unknown enum normalizes it to InternalError
without discarding diagnostics. Tests cover mappings for all ABI statuses, Ok,
unknown values and malformed / mismatched diagnostics. C++ applications and
Plugin sources must migrate and rebuild. C ABI headers and layouts remain
unchanged. The Runtime-only build and installed consumer smoke passed.

Post-change full CTest: Debug, Release, ASan/LSan, UBSan 94/94 each; Clang 14
TSan CI subset 10/10 with thread-leak detection. Formatting and clang-tidy pass
for 189 C/C++ files, 117 build translation units, public headers and installed
consumers. Hosted qualification is PENDING; historical results above do not
qualify this change. Native arm64 remains D-034 DEFERRED.


## 2026-11-05 Status + Result full migration — COMPLETE / local PASS

This section supersedes the earlier Error-based and direct expected-alias
recommendations for the current revision. Status now represents success or
failure; Result<T> privately uses pinned expected-lite and admits only non-OK
failure states. OK-as-failure normalizes to UnknownError. No Error class,
make_error helper, legacy result accessor or compatibility alias remains.
Result<void> and Result<Status> are covered by negative compile tests; primary
failure observation returns Status directly; OK means no primary failure. SDK, Runtime, benchmarks and all
consumer/tests migrated. C status-code and diagnostic types are rodaq_status_code
and rodaq_status, with status_code / error_code fields. Independent current ABI
fixtures replace legacy compatibility sources. Existing Admission, ownership,
Payload/Persistence and quarantine convergence tests continue to pass.

Final local checks: Debug, Release, ASan/LSan, UBSan and Ubuntu 24/GCC 13 each
96/96 CTest; Clang 14 TSan CI subset 10/10 with thread-leak detection; Runtime-only
Release build PASS; eight capacity executables each three repetitions PASS.
Formatting and clang-tidy PASS for 191 files, 117 build translation units,
26 C++ headers, eight C ABI headers in both languages and five install consumers.
Root clang-tidy configuration is supplied explicitly so dependency-local config
files cannot change the approved checks. Plugin exports remain the two required
C entry points per family; the foundation archive has no old Error symbols.
Final namespace/identifier scans and git diff whitespace review PASS.

New hosted qualification remains PENDING, M3 NOT STARTED, native arm64 D-034
DEFERRED. These are local results for the uncommitted working tree; historical
hosted successes above do not qualify the new interfaces.

## 2026-11-06 Direct Status failure storage review

Removed all project optional<Status> instances. primary_failure() and its stored
history use OK to mean no failure; Descriptor reconciliation, deferred admission
failures, Storage completion effects and Quarantine diagnostics use the same
representation. Async completion remains guarded by the existing completion,
Join and mutex protocols, independently of Status. Updated all callers and the
installed DAQ consumer without aliases. Reviewed the final diff and verified
no optional<Status> or old optional primary-failure access remains.

Debug warnings-as-errors, ASan/LSan and UBSan: 96/96 each; Clang 14 TSan with
thread-leak checks: 10/10. Full clang-format/clang-tidy checks and the separately
rerun installed consumer pass. Regression assertions verify OK throughout a
healthy lifecycle and exact failed-call diagnostic preservation. LSan could not
operate in the initial ptrace-constrained sandbox; that run was stopped and the
full outside-sandbox rerun passed with leak detection enabled.

Release capacity, Runtime-only and Ubuntu 24 were not rerun for this follow-up.
Earlier results are historical; new hosted qualification remains PENDING.

## 2026-11-06 Result<void> operation migration — COMPLETE / local PASS

All no-value fallible C++ operations now use Result<void>, backed privately by
nonstd::expected<void, Status>. Default construction is success; non-OK Status
constructs failure, and OK supplied as a failure becomes UnknownError. Status
continues to carry diagnostics and primary/deferred failure observations.
Non-void Result defaults and the Result<Status> prohibition remain unchanged.
The specialization's non-template methods live in src/result.cpp.

Reviewed Runtime, SDK virtuals/overrides/trampolines, loader/services, logging,
Quarantine callbacks, tests, benchmarks and installed consumers. Explicit failure
propagation prevents implicit Result<void>-to-Result<bool> conversion from
turning Binding failures into successful values. Existing Binding failure/stall
regressions pass. Success paths at ABI boundaries return a successful void
Result rather than constructing it from OK Status. C ABI headers, values,
layouts, exported Plugin entry points and domain correctness contracts are
unchanged; no compatibility layer is retained.

Debug warnings-as-errors, Release, ASan/LSan, UBSan and offline Ubuntu 24:
95/95 each. Clang 14 TSan with thread-leak detection: 10/10. Runtime-only
Release with production dependencies only and all eight Release benchmarks with
three repetitions pass. Full clang-format/clang-tidy 14 checks pass: 191 files,
118 TUs, 26 C++ headers, eight C ABI headers in both languages, five install
consumers. Source residue, C exports and final staged/unstaged diff checks pass.
CTest count is 95 after deleting the obsolete void compile-fail fixture; three
positive void unit tests plus compile/install checks cover the replacement.

New hosted qualification remains PENDING. Earlier validation is historical.
M3 is NOT STARTED and D-034 remains DEFERRED. Existing staged work was preserved.
