# RoDAQ Logging V1 设计

**Project:** RoDAQ — Robot Data Acquisition
**Version:** v0.1
**Status:** Implementation Baseline

本文定义 RoDAQ V1 Logging 的公共语义、Runtime 生命周期、bounded dispatch、Flush/Shutdown、Plugin logging boundary、spdlog/fmt backend、错误处理、指标和验证要求。

Logging 是 RoDAQ Runtime 的基础 primitive，不是独立 Observability Framework。

---

# 1. 目标与边界

## 1.1 目标

RoDAQ Logging V1 必须满足：

- Runtime / Core / Plugin 使用统一日志语义；
- Plugin 不直接拥有 spdlog logger / thread pool / sink；
- Logging 不暴露 spdlog/fmt 类型到 Public API 或 Stable Plugin ABI；
- 多线程调用安全；
- async mode 资源有界；
- Stop/Failure 下可 boundedly flush / shutdown；
- logging failure 不破坏 RoDAQ primary failure；
- 日志热路径不参与 Canonical / Admission correctness；
- logging 资源耗尽不会阻塞或破坏 Accepted Record pipeline。

## 1.2 非目标

V1 不实现：

- distributed tracing；
- OpenTelemetry SDK；
- Prometheus exporter；
- remote log shipping framework；
- arbitrary structured-event schema framework；
- hard real-time logging；
- per-Plugin logging runtime；
- Plugin-owned spdlog async pool。

## 1.3 Ownership

~~~text
Application
    ↓
RoDAQ Runtime
    owns
LoggingContext
    ↓
Logger facade
    ↓
bounded dispatch
    ↓
LoggingBackend
    ↓
SpdlogBackend
~~~

Plugin：

~~~text
Plugin
    ↓
rodaq.runtime.common.v1::log()
    ↓
Runtime LoggingContext
~~~

Plugin 不创建 spdlog thread pool，不拥有 sink，也不直接 flush/shutdown backend。

---

# 2. Public Logging Model

## 2.1 LogLevel

RoDAQ public type：

~~~cpp
namespace rodaq {

enum class LogLevel : std::uint8_t {
  kTrace,
  kDebug,
  kInfo,
  kWarn,
  kError,
  kCritical,
  kOff,
};

}
~~~

Public API 不暴露 spdlog::level::level_enum 或 fmt 类型。

## 2.2 Category

Category：

~~~text
UTF-8
non-empty
lowercase recommended
hierarchical '.' separator recommended
~~~

RoDAQ built-in examples：

~~~text
rodaq.runtime
rodaq.admission
rodaq.payload
rodaq.plugin
rodaq.storage
rodaq.acquisition
rodaq.mcap
rodaq.sqlite3
~~~

Plugin category 建议：

~~~text
plugin.<plugin-id>
plugin.<plugin-id>.<subsystem>
~~~

Category 是诊断/过滤 metadata，不属于 Canonical State。

## 2.3 LogRecord

Runtime internal LogRecord 至少包含：

~~~text
level
category
message
wall timestamp
monotonic sequence/time for local ordering
thread identifier diagnostic
optional source location
~~~

Logger input string 在调用返回后不保证继续有效。

因此 async mode 必须在 admission 成功时把 required category/message bytes materialize 到 bounded Logging-owned storage，不能保存 caller temporary pointer。

## 2.4 Logger Facade

推荐 Public C++ API：

~~~cpp
namespace rodaq {

class Logger {
 public:
  void Log(LogLevel level,
           std::string_view category,
           std::string_view message) noexcept;
};

}
~~~

具体 convenience API 可以在实现阶段微调，但必须保持：

~~~text
RoDAQ-owned types
no exceptions crossing noexcept boundary
no spdlog/fmt public dependency
~~~

## 2.5 Plugin Logging ABI

Plugin 通过 rodaq.runtime.common.v1::log() 记录日志。

V1 Stable ABI 数值域已经冻结为：

~~~text
RODAQ_LOG_TRACE     = 1
RODAQ_LOG_DEBUG     = 2
RODAQ_LOG_INFO      = 3
RODAQ_LOG_WARN      = 4
RODAQ_LOG_ERROR     = 5
RODAQ_LOG_CRITICAL  = 6
RODAQ_LOG_OFF       = 7

RODAQ_DIAGNOSTIC_INFO     = 1
RODAQ_DIAGNOSTIC_WARNING  = 2
RODAQ_DIAGNOSTIC_ERROR    = 3
RODAQ_DIAGNOSTIC_FATAL    = 4
~~~

`DiagnosticSeverity` 与 `LogLevel` 是两个独立 domain；不得按整数直接互转。未知值不得 reinterpret 成较低严重度。

Plugin call：

~~~text
thread-safe
non-throwing
best-effort
~~~

由于 ABI 为 void，logging rejection/drop 不通过 Plugin return path 传播；Runtime 通过 metrics 记录。

---

# 3. Runtime、Boundedness 与 Lifecycle

## 3.1 LoggingContext Lifecycle

~~~text
Created
    ↓
Running
    ↓
Stopping
    ↓
Stopped
~~~

正常 Runtime Stop：

~~~text
close new logging admission
        ↓
drain accepted log records boundedly
        ↓
Backend Flush
        ↓
Backend Shutdown / sink close
        ↓
Stopped
~~~

Runtime failure path：

~~~text
preserve primary Runtime error
        ↓
attempt bounded logging drain / flush
        ↓
logging failure becomes secondary diagnostic
~~~

Logging failure 不得覆盖更早的 primary Runtime / Storage / Plugin failure。

## 3.2 Sync / Async Mode

V1 internal execution strategy：

~~~text
Sync
AsyncBounded
~~~

Production default 推荐 AsyncBounded；测试/minimal environment 可以使用 Sync。Public API / Plugin ABI 不因 strategy 不同而改变。

## 3.3 Async Bounded Queue

Async mode 必须同时受：

~~~text
fixed configured record capacity
fixed configured byte capacity
~~~

约束。

Logging queue 禁止 unbounded growth，也禁止 fallback 到 unlimited heap。

Logging capacity 与 Runtime Record Admission ResourceBudget 独立，Logging 不允许占用 Accepted Record 所需 reserved resource。

## 3.4 Logging Admission

每条 log call：

~~~text
level/category filter
        ↓
bounded logging reservation
        ↓
materialize LogRecord
        ↓
publish to logging queue
~~~

资源不足时日志可以 drop，因为 Logging 是 diagnostic plane，不是 Canonical Data Plane。

必须维护：

~~~text
log_records_submitted
log_records_written
log_records_dropped
log_bytes_dropped
logging_queue_high_water
backend_failures
~~~

## 3.5 Overflow Policy

V1 默认：

~~~text
drop-new
~~~

而不是：

~~~text
block acquisition thread indefinitely
drop-old silently
grow queue
~~~

Warn/Error/Critical 可以配置 best-effort emergency sink，但不得持 Logging internal mutex 调用 emergency I/O，也不得覆盖 Runtime primary error。

## 3.6 Logger Calls During Stop

Stopping 后 new ordinary application/plugin log admission 可以被拒绝。

Runtime shutdown 自身仍可使用少量 pre-reserved control diagnostic capacity；不得为了记录“Logging 已关闭”递归进入已经关闭的 queue。

## 3.7 Reentrancy

LoggingBackend / sink 不得调用同一个 Logger 形成递归 logging。

Runtime 至少检测同线程 direct recursion；发生时 drop recursive record 并计数，不能无限递归。

---

# 4. Backend、Flush 与 Failure

## 4.1 LoggingBackend

RoDAQ-owned internal interface 示例：

~~~cpp
class LoggingBackend {
 public:
  virtual ~LoggingBackend() = default;
  virtual Result<void> Write(const LogRecord& record) = 0;
  virtual Result<void> Flush() = 0;
  virtual Result<void> Shutdown() = 0;
};
~~~

该接口 internal/private，不属于 Stable Plugin ABI。

## 4.2 SpdlogBackend

V1 production backend：

~~~text
spdlog 1.17.0
fmt 12.1.0
~~~

构建关系：

~~~text
fmt static
   ↑
spdlog static
   ↑
RoDAQ Runtime-owned backend
~~~

RoDAQ 可以使用 spdlog sinks / formatting primitives，但 RoDAQ 自己拥有：

~~~text
Logger API
queue boundedness
drop policy
lifecycle
flush/shutdown semantics
metrics
~~~

不得把 spdlog global async thread-pool lifecycle 变成 RoDAQ lifecycle contract。

## 4.3 Formatting

格式化可以使用 fmt，但 Public API 不暴露 fmt::format_string / fmt::memory_buffer。

首个实现可以优先接受已经构造好的 UTF-8 message string，减少 API surface。

## 4.4 Backend Failure

Backend Write failure：

~~~text
latch backend_failed
increment backend_failures
stop pretending record was written
~~~

后续 logging 可以 drop 或使用 emergency fallback，但默认不得仅因为 diagnostic file 写失败就把健康 RoDAQ Runtime 转为 Failed。

## 4.5 Flush

Flush(deadline) 成功至少保证：

~~~text
all LogRecords accepted before Flush linearization point
    have either
        been handed to Backend and Backend Flush completed
    or
        been explicitly accounted as dropped/backend-failed
~~~

Flush 不自动表示 fsync / media durability / remote acknowledgment。

## 4.6 Shutdown

~~~text
close normal logging admission
        ↓
drain bounded queue
        ↓
Flush
        ↓
Backend Shutdown
        ↓
join worker
        ↓
Stopped
~~~

Shutdown 受 Runtime-owned deadline 监督。

deadline 到达只表示 caller timeout，不证明 worker 已停止。禁止 unsafe thread termination；LoggingContext / worker-owned state 必须保持到真正 quiescent。

## 4.7 Destructor

Destructor 不承担无限等待 contract。

正常代码必须在 Runtime Stop 中显式 Shutdown。若析构时仍未安全 quiescent，优先 memory safety，不在 destructor 中无限 join。

---

# 5. Configuration、Metrics 与验证

## 5.1 Configuration

示例：

~~~yaml
logging:
  level: info
  mode: async
  queue_records: 8192
  queue_bytes: 8388608
  flush_timeout_ms: 2000
  shutdown_timeout_ms: 3000
  sinks:
    - type: stderr
~~~

yaml-cpp 只属于 configuration frontend；Logging implementation 不保存 YAML::Node。

## 5.2 Metrics

至少：

~~~text
logging.records_submitted
logging.records_written
logging.records_dropped
logging.bytes_dropped
logging.queue_records_current
logging.queue_records_high_water
logging.queue_bytes_current
logging.queue_bytes_high_water
logging.backend_failures
logging.reentrant_drops
logging.flush_timeouts
logging.shutdown_timeouts
~~~

## 5.3 Required Tests

API / formatting：

- level mapping；
- category validation；
- backend-independent Logger tests。

Boundedness：

- queue record capacity；
- queue byte capacity；
- no unlimited fallback；
- overflow drop accounting；
- high-water accuracy。

Concurrency：

- multi-thread producer；
- worker drain；
- Stop while producers logging；
- reentrancy detection；
- TSan clean for required tests。

Lifetime：

- async input string can die immediately after Log return；
- queued record owns required bytes；
- backend/sink outlives worker access；
- no use after LoggingContext destruction。

Failure：

- Backend Write / Flush / Shutdown failure；
- emergency sink failure；
- materialization failure；
- primary Runtime error not overwritten。

Timeout：

- Flush timeout；
- Shutdown timeout；
- stuck backend；
- timeout does not force-kill worker；
- state retained until safe quiescence。

Plugin integration：

- Plugin logs through rodaq.runtime.common.v1；
- Plugin does not require spdlog linkage；
- logging callable from acquisition callback threads；
- log overflow does not consume Canonical Record admission capacity。

## 5.4 Sanitizers

Required Logging tests：

~~~text
ASan
UBSan
TSan for async/concurrency paths
~~~

## 5.5 Freeze Boundary

V1 freezes：

~~~text
RoDAQ-owned LogLevel semantics
Plugin uses Runtime common logging service
Logging resources are bounded
default overflow does not block Data Plane indefinitely
Runtime owns Logging lifecycle
spdlog/fmt do not leak into public API/ABI
primary Runtime failure is preserved
Shutdown timeout never implies unsafe thread termination
~~~

May evolve privately：

~~~text
queue implementation
worker implementation
sink set
format layout
spdlog sink composition
emergency sink mechanics
metrics implementation
~~~

---

## 最终结论

RoDAQ Logging V1 采用：

> **RoDAQ-owned Logger facade + Runtime-owned bounded LoggingContext + private spdlog/fmt backend + Plugin Runtime-service logging + explicit bounded Flush/Shutdown semantics。**

Logging 必须服务于 RoDAQ 的可诊断性，而不能反过来成为 Data Plane correctness、resource boundedness 或 shutdown safety 的破坏源。
