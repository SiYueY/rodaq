# RoDAQ Plugin 通用插件系统设计

🧩

**文档说明**

本文件是 RoDAQ 通用插件系统的唯一权威详细设计，覆盖稳定 C ABI、C++17 Plugin SDK、插件运行时、插件包/插件目录/加载器、生命周期、异常保留、常驻插件库生命周期、第三方插件交付和资格验证。

本文档以“可直接指导实现”为目标。除明确标注为待冻结的名称或数值外，开发人员不应自行改变影响 ABI/API、所有权、并发、生命周期或错误语义的设计。

</aside>

| 文档属性 | 内容 |
| --- | --- |
| 文档名称 | RoDAQ Plugin Framework 详细设计 |
| 文档标识 | `plugin.md` |
| 文档版本 | **0.2.3** |
| 状态 | **实现就绪详细设计候选（Implementation-Ready Detailed Design Candidate）** |
| 语言基线 | C ABI + C++17 |
| 平台基线 | Linux ELF；Ubuntu 22.04 / 24.04；amd64 / arm64 |
| 设计范围 | C ABI、C++ SDK、插件运行时、Plugin Package、Plugin Catalog、Loader、生命周期、异常保留、常驻插件库生命周期、交付与资格验证 |
| 非设计范围 | 具体业务模型、具体协议/Backend 语义、跨进程插件、插件沙箱、自动 Plugin 依赖图、透明热升级 |

# 1. 文档概述与设计基线

本章建立 RoDAQ Plugin Framework 的 V1 设计边界。V1 的目标不是构造一个通用插件平台，而是为 RoDAQ 的 Acquisition、Storage 等可扩展能力提供稳定、可维护、能够长期运行的插件机制。

## 1.1 设计目标

V1 必须同时满足：

- Linux ELF / `dlopen()` 插件加载；
- Ubuntu 22.04 / 24.04，amd64 / arm64；
- 稳定 C ABI，C++17 SDK；
- 一个 Plugin 可以提供多个 Operation Interface；
- RoDAQ Application 可以向 Plugin 提供 Service Interface；
- Plugin DSO 成功加载后保持常驻，直到进程退出；
- Plugin Instance 可以安全创建、配置、调用和关闭；
- Plugin 异常或关闭迟返不得导致未定义行为；
- 普通插件开发者不需要理解 C ABI trampoline、Loader、调用保护和内部状态管理；
- Framework 的内部复杂度不能反向扩散到 Application 或 Plugin API。

V1 明确不以支持运行期 DSO 卸载、热更新、多 Runtime Domain、插件沙箱、跨进程插件和自动 Plugin 依赖图为目标。

## 1.2 规范性用语

本文使用：

- **必须**：实现必须满足；
- **不得**：实现明确禁止；
- **应该**：推荐实现；偏离时应有明确理由；
- **可以**：允许的实现选择。

文档根据具体上下文分别使用“规范、规则、约定、语义、约束”等表述。

## 1.3 三类开发者

整套设计只区分三类开发角色。

### 应用开发者

应用开发者负责 RoDAQ 上层 Application / Core / Server，主要使用：

- `PluginManager`；
- `PluginLibrary`；
- `PluginInstance`；
- `ServiceRegistry`；
- Acquisition / Storage 等业务 Interface SDK；
- Plugin 配置与诊断接口。

普通应用开发者不需要理解 C ABI、Loader 内部结构、调用计数、Plugin DSO 句柄或异步关闭执行器。

### 插件开发者

插件开发者实现 ROS 2 Acquisition、MQTT Acquisition、MCAP Storage、SQLite3 Storage 等 Plugin，主要使用：

- `PluginBase`；
- `PluginSetup`；
- 标准 Interface SDK；
- `RODAQ_DEFINE_PLUGIN`；
- `rodaq_register_plugin()`。

普通插件开发者不直接编写 C ABI、vtable、Adapter、trampoline、Manifest 或 Loader 代码。

### 框架开发者

框架开发者负责：

- Stable C ABI；
- C++17 Plugin SDK；
- Interface SDK；
- PluginManager / Loader；
- Plugin 生命周期与并发安全；
- Package / Manifest / PluginCatalog；
- ABI、SDK、Runtime 与集成资格验证。

Interface SDK 的设计和维护属于框架开发工作，不再单独定义第四类开发角色。

## 1.4 核心设计原则

1. **稳定 C ABI 是唯一跨 DSO 二进制规范。**
2. **C++17 SDK 只是 C ABI 的类型安全封装，不形成第二套跨 DSO 协议。**
3. **Framework 使用 Interface-first 模型，不定义 Acquisition/Storage 等 Plugin Type enum。**
4. **Operation 由 Plugin 提供，RoDAQ Application 查询并调用。**
5. **Service 由 RoDAQ Application 提供，Plugin 在配置阶段查询并使用。**
6. **Plugin Instance 只有完成初始化后才发布给 Application；Application 不接触任何半初始化对象。**
7. **Plugin 生命周期关闭使用 `shutdown()`，它表示“收敛到可以安全 destroy 的状态”，不等于 Acquisition Stop 或 Storage Finalize。**
8. **同步关闭使用 `close()`，异步关闭使用 `async_close()`；Framework 不暴露 request-close/ticket/completion 等内部实现概念。**
9. **成功加载的 Plugin DSO 在进程退出前保持常驻；V1 不实现运行期物理卸载。**
10. **所有仍可能执行 Plugin ABI 的调用都必须受到统一调用准入保护。**
11. **Plugin shutdown 未能完成时不得强制 destroy；实例进入 Unresolved，进程需要受控重启。**
12. **Framework 只保留解决 V1 实际问题所必需的对象和状态，不为未来潜在扩展提前建立通用机制。**

## 1.5 版本模型

V1 只有三个需要在设计层明确理解的版本。

| 版本 | 维护者 | 用途 |
| --- | --- | --- |
| Plugin Version | 插件开发者 / Plugin Project | 表示一个 Plugin 发行版本；使用 Semantic Versioning |
| Interface Version | 框架 / Interface SDK | 表示 Operation / Service Interface 的兼容版本 |
| Plugin ABI Version | Plugin Framework | 表示跨 DSO Foundation ABI 的兼容版本 |

### Plugin Version

Plugin 开发者只维护自己的 Plugin Version，例如：

```
plugin_id      = com.rodaq.acquisition.ros2
plugin_version = 1.2.0
```

Plugin Version 使用完整 Semantic Versioning，可以包含 prerelease 和 build metadata，例如：

```
1.2.3
1.2.3-rc.1
1.2.3+build.7
```

构建系统只有一个最终 Plugin Version 权威：`rodaq_register_plugin()` 先解析可选 `PLUGIN_VERSION`，未提供时才回退到 CMake `PROJECT_VERSION`。生成的 Descriptor 与 Manifest 必须使用同一个最终版本值。

V1 明确区分：

- **Plugin Version Identity**：完整 SemVer 字符串，包括 prerelease 和 build metadata；
- **Version Precedence**：按 Semantic Versioning 比较版本优先级；build metadata 不参与 precedence。

例如 `1.2.3+build.7` 与 `1.2.3+build.8` 是两个不同的 Plugin Version Identity，但具有相同的 SemVer precedence。

Plugin Package 是不可变发行物。同一个完整 Plugin Version Identity 对应固定的 DSO 与 Manifest。Plugin 私有配置格式发生不兼容变化时，应发布新的 Plugin Version。

### Interface Version

Interface Version 由 Interface SDK 维护。Backend Plugin 只依赖对应 Interface SDK，不手工填写 Interface 版本。

### Plugin ABI Version

Plugin ABI Version 由 Plugin Framework 管理。普通 Plugin 开发者不手工填写。

Manifest 文件格式版本、RoDAQ SDK/Package 发行版本仍然可以存在于 Tooling 和构建系统中，但它们不是 Plugin Runtime 的独立兼容版本域，不在本节作为 Plugin 开发者需要理解的版本模型。

## 1.6 设计依据

| 设计问题 | 主要参考 | RoDAQ 采用 |
| --- | --- | --- |
| 稳定 C ABI | PlotJuggler 4、PipeWire SPA | opaque context、typed vtable、struct size |
| C++ Plugin 开发体验 | PlotJuggler 4 | PluginBase、导出辅助、Adapter/trampoline 自动生成 |
| 多 Interface | PipeWire SPA | 一个 Plugin Instance 可提供多个 typed Interface |
| Interface 查询 | PipeWire SPA、OpenSSL Provider | stable string ID + version query |
| Service 注入 | PlotJuggler、PipeWire support | typed immutable ServiceRegistry |
| ABI 演进 | PlotJuggler、PipeWire SPA | frozen prefix + append-only tail |
| DSO 生命周期 | Linux loader semantics | 成功加载后常驻到进程退出 |
| 异常恢复 | RoDAQ-specific | Faulted Library / Unresolved Instance + restart_required |

参考项目用于证明机制可行性，不意味着复制其全部对象模型。

## 1.7 关键架构决策

| ID | 决策 |
| --- | --- |
| PF-01 | C ABI 是唯一跨 DSO 二进制规范。 |
| PF-02 | Framework 使用 Interface-first 模型，不使用业务 Plugin Type。 |
| PF-03 | 一个 Plugin Instance 可以提供多个 Operation Interface。 |
| PF-04 | Operation 与 Service 都使用稳定字符串 ID、独立 Interface Version 和 typed vtable。 |
| PF-05 | Application Service 通过 immutable ServiceRegistry 提供给 Plugin。 |
| PF-06 | Plugin 初始化成功后 Operation 集合冻结；运行期间不动态增加/删除 Operation。 |
| PF-07 | Plugin Instance 只在初始化完成后发布给 Application。 |
| PF-08 | `PluginBase` 只暴露 `configure()` 与 `shutdown()` 两个生命周期入口。 |
| PF-09 | Application 使用 `close()` / `async_close()` 关闭 Plugin Instance。 |
| PF-10 | Plugin ABI 使用 Major/Minor + struct size + required prefix 演进。 |
| PF-11 | Query 与 Operation 调用使用统一的 Runtime call admission 和 active-call accounting。 |
| PF-12 | Deadline 只限制等待时间，不代表 Framework 可以强制终止 Plugin/native code。 |
| PF-13 | Plugin shutdown 成功且所有受管调用返回后才允许 destroy。 |
| PF-14 | PluginManagerState 持有所有活动、关闭中和 Unresolved Instance 的生命周期所有权。 |
| PF-15 | Plugin Package / Artifact identity 用于加载验证和诊断，不作为普通 Task 配置的主要身份。 |
| PF-16 | PluginCatalog 是内部发现索引；Application 通过 PluginInfo 枚举插件。 |
| PF-17 | 一个 Package 对应一个主 Plugin DSO / Plugin Definition。 |
| PF-18 | Public PluginLibrary handle 释放不触发 DSO unload。 |
| PF-19 | Plugin destructor / destroy 必须 no-throw。 |
| PF-20 | 同一个实际 Plugin Artifact 在一个 PluginManager 中只发布一个 PluginLibraryState。 |
| PF-21 | 异步关闭执行资源必须有界；关闭卡死时进入 Degraded，而不是无界创建线程或强制取消。 |
| PF-22 | Plugin create ABI 违约时将 PluginLibraryState 标记 Faulted、禁止后续 create，并要求进程重启。 |
| PF-23 | Plugin shutdown 无法完成时 PluginInstanceState 进入 Unresolved，并由 PluginManagerState 保留到进程退出。 |
| PF-24 | PluginManager 提供显式 `shutdown()`，内部关闭新的 refresh/load/create 准入并收敛 Framework 资源。 |
| PF-25 | Plugin Configuration 属于 Plugin 私有语义；Framework 只做有界传递，不解析或校验 Plugin-specific 配置结构。 |
| PF-26 | Plugin 版本选择由 PluginManager 内部的 Catalog / Resolver 完成；Public API 只提供按 ID 或 ID + exact version 加载。 |
| PF-27 | Service 查询使用单一 `service&lt;T&gt;()`；是否把 NotFound 视为错误由 Plugin 自己决定。 |
| PF-28 | Service Interface C++ 调用对象使用 `Reference`，Operation 使用 `Handle`。 |
| PF-29 | V1 每个进程只有一个 process-lifetime PluginManager authority；第一次 create 成功后 authority 持续到进程退出，即使进入 Stopped 也不在同进程重新创建第二套 Manager。 |
| PF-30 | V1 不提供 Plugin DSO 主动物理卸载、热更新或可独立卸载的 Service Provider。 |

## 1.8 对外集成接口与内部二进制依赖

公开 Framework CMake target 仍只有：

```
rodaq::plugin
```

Application：

```
find_package(RoDAQ REQUIRED)
target_link_libraries(app PRIVATE rodaq::plugin)
```

Backend Plugin：

```
find_package(RoDAQ REQUIRED)

add_library(my_plugin SHARED
    src/my_plugin.cpp
)

target_link_libraries(my_plugin
    PRIVATE
        my_private_dependencies
)

rodaq_register_plugin(my_plugin
    PLUGIN_ID com.example.my_plugin
    PLUGIN_NAME "Example Plugin"
)
```

`add_library()` 负责创建和配置 CMake target；`rodaq_register_plugin()` 只把已有 shared-library target 注册为 RoDAQ Plugin，并自动注入内部 authoring support、导出符号、Descriptor/Manifest、Package 与安装规则。普通 Application / Plugin 不链接 Framework 内部实现 target。

## 1.9 建议阅读路径

| 读者 | 建议阅读 |
| --- | --- |
| 应用开发者 | 第 2 章 → 第 5 章 → 第 7 章 |
| 插件开发者 | 第 2 章 → 第 4 章 → 第 7.3 节 |
| 框架开发者 | 第 3～8 章 |

第一次阅读时，先理解第 2 章的 Application / Plugin / Framework 关系和完整使用流程，再进入 ABI 与 Runtime 细节。

## 1.10 非目标

V1 不设计：

- Plugin DSO runtime unload；
- hot reload / state migration；
- 多个独立 Plugin Runtime Domain；
- 自动 Plugin 依赖图；
- 跨进程 Plugin；
- Plugin sandbox；
- hostile/untrusted DSO 的进程内安全隔离；
- 通用动态 Service Provider code lifetime；
- 通用 Plugin Configuration Schema / validator；
- Foundation 级 Acquisition / Storage 业务状态机。

# 2. 总体架构与对象模型

本章从 Application 和 Plugin 的实际使用关系出发描述 V1。内部实现只保留三个主要共享状态对象：`PluginManagerState`、`PluginLibraryState` 和 `PluginInstanceState`。

## 2.1 Application、Plugin 与 Framework

RoDAQ Plugin Framework 只连接两侧：

```
RoDAQ Application
        │
        │ load / create / query / close
        ▼
   Plugin Framework
        │
        ▼
      Plugin
```

其中：

- **RoDAQ Application**：使用插件的上层应用，包括 Core / Server；
- **Plugin**：被加载的插件实现，例如 ROS 2 Acquisition、MQTT Acquisition、MCAP Storage；
- **Plugin Framework**：负责发现、加载、ABI 适配、实例生命周期和调用安全。

## 2.2 Operation 与 Service

Plugin 与 Application 的扩展方向只有两种：

```
                     Operation
RoDAQ Application ─────────────────► Plugin
                  查询并调用 Plugin 能力

                      Service
RoDAQ Application ◄───────────────── Plugin
                  Plugin 调用 Application 能力
```

因此：

- **Operation 由 Plugin 提供，Application 查询并调用**；
- **Service 由 Application 提供，Plugin 查询并调用**。

Operation 在 C++ Interface SDK 中使用 `Handle`；Service 使用 borrowed `Reference`。

两者都使用：

- 稳定字符串 `interface_id`；
- Interface Version；
- typed vtable；
- opaque context；
- 明确的线程、阻塞、错误与生命周期规则。

## 2.3 Application 的完整使用流程

V1 的普通路径只有：

```
PluginManager
    │
    ├── refresh()
    ├── plugins()
    └── load(plugin_id [, plugin_version])
              │
              ▼
         PluginLibrary
              │
              └── create_instance(services, config)
                          │
                          ▼
                    PluginInstance
                       │
                       ├── query<Interface>()
                       ├── close()
                       └── async_close()
```

一次完整流程：

```
发现插件
→ 加载 Plugin Library
→ 创建并初始化 Plugin Instance
→ 查询 Operation
→ 调用 Operation
→ 业务层停止/Finalize
→ 关闭 Plugin Instance
→ PluginManager shutdown
```

Application 不接触半初始化的 Plugin Instance。`create_instance()` 只有在 create + configure + Operation publish 全部成功后才返回 `PluginInstance`。

## 2.4 Plugin 开发流程

插件开发者只需要：

```
PluginBase
├── configure(PluginSetup&)
└── shutdown(Deadline)

PluginSetup
├── config()
├── service<T>()
└── provide<T>()
```

其中：

- `configure()` 读取配置、查询 Application Service、创建必要资源并发布 Operation；
- `shutdown()` 停止 Plugin-owned worker/callback/async work，使实例进入可以安全 destroy 的状态；
- `shutdown()` 不是 Acquisition Stop，也不是 Storage Finalize；业务停止/完成仍由对应 Interface 定义。

## 2.5 对外对象职责

| 对象 | 作用 |
| --- | --- |
| PluginManager | 发现、枚举、加载 Plugin，并在进程退出前统一 shutdown Framework |
| PluginInfo | Application 可见的插件静态摘要：ID、名称、版本 |
| PluginArtifactInfo | Session 审计使用的实际插件 ID、版本与主 Artifact SHA-256 |
| PluginLibrary | 一个已经验证并加载的 Plugin Definition，可以创建多个 Instance |
| PluginInstance | 一个已经完成初始化、可供 Application 调用的 Plugin 实例 |
| CloseFuture | `async_close()` 返回的异步关闭结果对象 |
| Operation Handle | Application 对 Plugin Operation 的类型安全受管调用句柄 |
| Service Reference | Plugin 对 Application Service 的类型安全借用引用 |
| ServiceRegistry | Application 提供给 Plugin 的 Service 集合 |

Application 不需要理解 Loader、Catalog、调用计数、close worker 或 DSO handle。

## 2.6 Framework 内部状态与 Public Handle

V1 内部只保留三个主要共享状态对象。

命名统一遵循：

```
State  = 长生命周期共享对象，持有资源、同步原语和完整内部状态
Phase  = 生命周期状态机当前阶段的枚举值
Status = 一次操作的成功/失败结果
```

因此 `PluginManagerState / PluginLibraryState / PluginInstanceState` 都是内部 C++ class/object，不是枚举；对应生命周期值分别使用 `PluginManagerPhase / PluginLibraryPhase / PluginInstancePhase`。

### PluginManagerState

表示整个进程中唯一 PluginManager 的运行状态，负责：

```
PluginManagerState
├── PluginCatalog
├── loaded_plugins
├── instances
├── AsyncCloseExecutor
├── Capacity
├── Diagnostics
├── active_refresh_count
├── phase
└── restart_required
```

它是 Plugin Framework 的进程级所有权根。

### PluginLibrary

`PluginLibrary` 是只引用已发布 `PluginLibraryState` 的 move-only Public Handle。其完整 Public 声明以 §5.1 为唯一来源；析构或覆盖 Public Handle 不触发 DSO unload。成功发布的 DSO 仍按 Resident DSO 规则常驻到进程退出。

### PluginInstanceState

表示一个 Plugin Instance 的共享状态：

```
PluginInstanceState
├── Instance ID
├── phase
├── raw Plugin Instance
├── ServiceRegistry
├── active_call_count
├── close_result
├── strong PluginLibraryState
└── diagnostics
```

生命周期状态收敛为：

```
Initializing
    │ success
    ▼
   Open
    │ close / destructor / manager shutdown
    ▼
 Closing
   ├── shutdown + drain + destroy success → Closed
   └── shutdown cannot be proven safe      → Unresolved
```

`PluginInstanceState` 是内部长期共享对象，内部字段 `phase` 使用 `PluginInstancePhase`。`Initializing` 不发布给 Application，因此有效 Public `PluginInstance::phase()` 不会观察到 Initializing。

`Unresolved` 表示 Framework 无法证明实例可以安全 destroy。此时实例状态继续由 `PluginManagerState` 持有，`restart_required = true`。

## 2.7 所有权关系

正常运行：

```
PluginManagerState
├── strong PluginLibraryState ─owns→ resident DSO
└── strong PluginInstanceState
       ▲            ▲
       │            │
 PluginInstance   Operation Handle
```

异步关闭：

```
PluginManagerState ─strong→ PluginInstanceState
CloseFuture       ─strong→ PluginInstanceState
AsyncCloseExecutor ─temporary strong→ PluginInstanceState
```

关闭成功后：

- raw Plugin Instance 已 destroy；
- Instance-owned ServiceRegistry 和 backing 已释放；
- `PluginInstanceState::phase = Closed`；
- close result 固定；
- PluginManagerState 从活动实例集合中移除该 State；
- 如果仍有 `CloseFuture`，只会保留一个已经关闭的小型 State，不会保留 ROS 2 Subscription、MCAP writer 等业务资源。

关闭结果直接保存在已经关闭的 `PluginInstanceState` 中。

## 2.8 异常处理模型

V1 的异常状态只保留两类：Faulted Plugin Library 与 Unresolved Plugin Instance。

### Faulted Plugin Library

如果 Plugin `create()` 违反 ABI，例如：

- 返回 OK 但 Instance/vtable 不完整；
- 返回 non-OK 但仍输出非默认 raw Instance；

Framework：

```
mark PluginLibraryState.phase = Faulted
→ record critical diagnostic
→ reject future create_instance()
→ PluginManagerState.restart_required = true
```

Framework 不调用未经验证的 destroy/function pointer。由于成功加载的 DSO 本身保持常驻，因此无需额外对象保活代码映射。

### Unresolved Plugin Instance

必须区分“Native shutdown 仍在执行”和“已经形成关闭终态失败”：

```
shutdown() remains in-flight beyond policy deadline
→ PluginInstancePhase remains Closing
→ PluginManagerPhase = Degraded
→ restart_required = true

shutdown() actually returns non-OK
or Framework obtains equivalent terminal proof
that safe destroy cannot be established
→ PluginInstancePhase = Unresolved
→ retain PluginInstanceState until process exit
→ PluginManagerPhase = Degraded
→ restart_required = true
```

Framework 不强制线程取消，也不执行不安全 destroy。只要 Native shutdown 仍未返回，就不得把仍在执行的 Instance 提前发布成 terminal Unresolved。

## 2.9 标识与名称

`ID` 表示 Identifier（标识符），不是数字序号。

例如：

```
plugin_id      = com.rodaq.acquisition.ros2
plugin_name    = ROS 2 Acquisition
plugin_version = 1.2.0
```

规则：

| 名称 | 含义 |
| --- | --- |
| plugin_id | 稳定机器标识符；跨版本保持稳定 |
| plugin_name | 面向开发者和 UI 的插件名称 |
| plugin_version | Plugin 发行版本 |
| interface_id | 稳定 Interface 标识符 |
| PluginInstanceId | 当前进程内用于诊断的实例 ID |

`*_id` 可以是字符串；`*_name` 才表示面向人的名称。

Artifact digest、canonical path、actual file identity 等只用于 Loader 验证、去重和诊断，不作为普通 Application API 中的 Plugin 名称。

## 2.10 Service 依赖模型

PluginSetup 只提供一个 Service 查询入口：

```cpp
template <typename ServiceInterface>
Result<typename ServiceInterface::Reference> service();
```

缺失 Service 返回 `NotFound`。Framework 只报告“Service 是否存在以及是否版本兼容”，不替 Plugin 决定该 Service 是 Required 还是 Optional。

必需 Service：

```cpp
auto logging = setup.service<LoggingInterface>();
if (!logging) {
    return logging.status();
}
```

可选 Service：

```cpp
auto tracing = setup.service<TracingInterface>();
if (!tracing &&
    tracing.status().code() != StatusCode::NotFound) {
    return tracing.status();
}

if (tracing) {
    // use optional service
}
```

ServiceRegistry 在 Plugin Instance 初始化期间已经冻结，Plugin 不能修改它。

## 2.11 Plugin Instance 创建与关闭

### 创建

```
PluginLibrary::create_instance(services, config)
→ reserve Instance State / capacity
→ publish Manager ownership
→ supervised Plugin ABI create()
→ validate raw Instance
→ attach immutable ServiceRegistry
→ supervised Plugin configure()
→ freeze Operation Registry
→ publish PluginInstanceState = Open
→ return PluginInstance
```

任一步失败都不向 Application 发布半初始化 Instance。

如果 create/configure 已产生部分资源，Framework 调用 Plugin `shutdown()` 收敛部分初始化状态；只有 shutdown 成功后才 destroy。

### 关闭

```
PluginInstance::close()
or async_close()
→ phase Open → Closing
→ reject new Query / Operation calls
→ invoke Plugin shutdown()
→ wait active_call_count == 0
→ destroy exactly once
→ release ServiceRegistry / Instance backing
→ phase = Closed
```

`close()` 是同步等待接口；`async_close()` 返回 `CloseFuture`。

如果调用者等待超时：

- `close()` / `CloseFuture::wait()` 返回 `DeadlineExceeded`；
- 实际关闭继续在 Framework 的有界异步关闭执行器中推进；
- timeout 不代表 Plugin 已停止，也不允许 unsafe destroy。

## 2.12 PluginManager shutdown

Application 正常退出顺序：

```
停止新的业务 Task
→ 停止/Finalize 已有业务 Operation
→ 关闭 Task 持有的 PluginInstance
→ PluginManager::shutdown(deadline)
→ process exit
```

`PluginManager::shutdown()` 是幂等观察接口：

- Running：开始 shutdown 并等待同一流程；
- ShuttingDown：继续观察同一流程；
- Stopped：返回 Ok；
- Degraded：返回已记录的终态失败。

关闭流程：

- 原子关闭新的 `load()` / `create_instance()` 准入；
- 原子关闭新的 `refresh()` 准入；
- **立即**对当前已经 Open 的 Instance 发起关闭；
- **并行**监督已经进入的 refresh/load/create/configure 与 Pending/Running close work；
- 已经进入的 refresh 只能完成内部扫描/解析；Manager 已非 Running 时必须丢弃新 Catalog generation，不得发布；
- late initialization 在 Manager 已非 Running 时不得发布新的 Open Instance，而是进入 Manager-owned cleanup；
- hung refresh/load/create/configure 达到 lifecycle operational threshold 后进入 Degraded / `restart_required`，但不得阻止其他健康 Instance 继续关闭；
- 只有满足 Normal Shutdown Success Predicate（无 Faulted/Unresolved、无 active refresh/initialization、无 pending/running refresh/initialization/close work、`restart_required == false`）时，才停止内部执行资源并进入 `Stopped`；
- 如果只是 observer deadline 到期，返回 DeadlineExceeded，但保持 `ShuttingDown` 并继续内部收敛；
- 如果存在 Faulted/Unresolved、refresh/initialization supervision failure、Native Shutdown policy timeout 或已确认 lifecycle/close-executor liveness failure，则进入 `Degraded` 并返回对应失败。

Manager Shutdown 不采用“先等 active refresh/load/create 全部返回，再关闭已有 Instance”的串行顺序。

Application 不需要自行维护另一套 Plugin refresh/load/create 准入状态，也不需要拼接 Framework 内部 counter 来证明退出安全。

# 3. 稳定 C ABI 详细设计

本章定义 RoDAQ Application 与 Plugin 跨 DSO 边界时唯一稳定的二进制规范。C 类型名和函数名保持英文；正文使用中文说明语义、所有权和失败行为。

## 3.1 ABI 基本规则

允许跨 ABI：

- fixed-width integer；
- C POD struct；
- pointer + length；
- domain-typed opaque context pointer；
- typed / opaque vtable pointer；
- function pointer；
- caller-owned output buffer。

禁止跨 ABI：

- C++ class / virtual interface；
- STL；
- exception；
- RTTI；
- compiler-specific C++ layout；
- ownership 未定义的跨 DSO heap object；
- Application / Plugin 互相 `free/delete` 对方对象。

所有 ABI function 必须：

- 不让异常越过 C ABI；
- 在进入业务逻辑前初始化 output；
- 明确 nullability 和 ownership；
- non-OK 返回不得要求调用方解析 message 决定控制流。

### Public C ABI 命名层级

- RoDAQ-wide 基础类型使用 `rodaq_*`：Status/Error、String/Bytes/Buffer、Deadline、Struct Header；
- Plugin Framework 特有对象使用 `rodaq_plugin_*`；
- 通用 Interface binding/version/query 使用 `rodaq_interface_*`；
- Acquisition / Storage / 第三方 Interface 使用各自前缀。

Public C ABI 不直接使用裸 `void*` 表达领域 context：

```c
typedef struct rodaq_interface_context
    rodaq_interface_context_t;

typedef struct rodaq_interface_vtable
    rodaq_interface_vtable_t;

typedef struct rodaq_service_registry_context
    rodaq_service_registry_context_t;

typedef struct rodaq_plugin_context
    rodaq_plugin_context_t;
```

这些 opaque type 提供静态类型隔离；实际生命周期仍由 Framework 和 Interface 规则保证。

### Public C 函数参数约定

参数名称只表达领域对象，不使用 `in_`/`out_` 前缀。Public Header 通过 Doxygen 说明：

- 输入 / 输出方向；
- 是否允许 `nullptr`；
- borrowed / owned；
- 有效期；
- non-OK 后 output 状态；
- blocking / deadline；
- concurrency / reentrancy；
- 所有带单位的数值字段/参数的单位。

Public 字段和参数名不编码物理单位，例如不使用 `_ns`、`_ms`、`_bytes` 后缀；单位由 Doxygen 明确规定。类型本身表达领域语义的名称（例如 `rodaq_bytes_t`）不属于参数单位后缀。

## 3.2 状态码与基础类型

```c
typedef uint32_t rodaq_status_code_t;

enum {
    RODAQ_STATUS_OK                 = 0u,
    RODAQ_STATUS_INVALID_ARGUMENT   = 1u,
    RODAQ_STATUS_INVALID_STATE      = 2u,
    RODAQ_STATUS_NOT_FOUND          = 3u,
    RODAQ_STATUS_CONFLICT           = 4u,
    RODAQ_STATUS_RESOURCE_EXHAUSTED = 5u,
    RODAQ_STATUS_DEADLINE_EXCEEDED  = 6u,
    RODAQ_STATUS_UNAVAILABLE        = 7u,
    RODAQ_STATUS_PERMISSION_DENIED  = 8u,
    RODAQ_STATUS_DATA_LOSS          = 9u,
    RODAQ_STATUS_INTERNAL           = 10u
};

typedef struct rodaq_string {
    const char* data;
    uint32_t size;
} rodaq_string_t;

typedef struct rodaq_bytes {
    const uint8_t* data;
    uint32_t size;
} rodaq_bytes_t;

typedef struct rodaq_buffer {
    uint8_t* data;
    uint32_t capacity;
    uint32_t size;
} rodaq_buffer_t;

typedef struct rodaq_deadline {
    /**
     * Absolute CLOCK_MONOTONIC deadline.
     *
     * Unit: nanoseconds.
     */
    int64_t monotonic_deadline;
} rodaq_deadline_t;

#define RODAQ_DEADLINE_INFINITE INT64_MAX

typedef struct rodaq_struct_header {
    uint32_t struct_size;
} rodaq_struct_header_t;

typedef struct rodaq_error {
    rodaq_buffer_t message;
} rodaq_error_t;
```

`rodaq_status_code_t` / `rodaq_error_t` 是 RoDAQ-wide 公共类型，与 C++ `StatusCode` 一一映射。

`rodaq_struct_header_t` 只保存 `struct_size`。Plugin ABI Major/Minor 和 Interface Major/Minor 已有独立版本机制，不在每个 appendable struct 中重复保存版本字段。

### 字符串与字节序列

`rodaq_string_t` / `rodaq_bytes_t` 都是 non-owning pointer + length：

- `{nullptr, 0}` 表示 empty；
- `nullptr + nonzero size` 非法；
- `rodaq_string_t` 必须是 UTF-8；
- 类型本身不负责 free/realloc；
- 同步输入默认只在当前 ABI call 内有效；
- 需要长期保存必须复制。

### 错误信息缓冲区

Caller 提供 `data` 与 `capacity`；Callee 只写 buffer 内容和 `size`。

- `size &lt;= capacity`；
- message 可以截断，但必须保持 UTF-8 code-point 完整；
- 不要求 NUL 终止；
- `error == nullptr` 合法；
- StatusCode 是唯一机器控制流依据，message 只用于诊断。

Runtime 在 Plugin ABI 调用返回后必须验证 Plugin 写入的状态和 `rodaq_error_t` 输出：

- `rodaq_status_code_t` 不属于 §3.2 已发布枚举值是 ABI contract violation；不得静默转换成 C++ `StatusCode` 或解释为成功；
- `{data = nullptr, capacity = 0, size = 0}` 是唯一 empty Error Buffer；`data == nullptr && capacity != 0`、`size > capacity` 或 `size != 0 && data == nullptr` 均非法；
- 非空 message 必须满足 §3.2 Public String 的 UTF-8 规则；
- 合法 non-OK StatusCode 是主结果；即使 Error Buffer 结构非法，也不得覆盖该非 OK 状态，只记录 bounded contract diagnostic；
- StatusCode == OK 却产生结构非法或非空 Error Buffer 时，当前操作返回 Internal，output 按对应 ABI 的 default-invalid/丢弃规则处理；
- unknown status 出现在 create/configure 时按初始化 ABI 违约处理，Library → Faulted、禁止后续 create 并设置 `restart_required`；
- unknown status 出现在 query/typed Operation 时，当前调用返回 Internal、不得发布 partial output，并记录 critical diagnostic；
- unknown status 出现在 shutdown 时，当前关闭结果按 Internal 处理，且不得 destroy，实例进入既有 Unresolved 收敛路径。

### Deadline

`monotonic_deadline` 表示 Linux `CLOCK_MONOTONIC` 的绝对截止时间。单位由 Public Header 的 Doxygen 明确规定为 nanoseconds，不在字段名中编码单位。

- `INT64_MAX`：无限等待；
- 其他负值：非法；
- finite value ≤ now：已过期；
- Deadline 只限制等待，不表示取消执行；
- 从 C++ duration 计算 absolute deadline 时必须使用检查/饱和算术，禁止 signed overflow；有限 duration 如果超过可表示范围，饱和到 `INT64_MAX - 1`（最晚有限 deadline），不得把有限等待隐式转换成 `RODAQ_DEADLINE_INFINITE`。

## 3.3 Interface 版本与查询

```c
typedef struct rodaq_interface_version {
    uint16_t major;
    uint16_t minor;
} rodaq_interface_version_t;

typedef struct rodaq_interface_query {
    rodaq_string_t interface_id;
    uint16_t major;
    uint16_t min_minor;
    uint16_t max_minor;
} rodaq_interface_query_t;
```

规则：

- Interface ID 长度 1～255 bytes；
- canonical ID 使用 lowercase ASCII `[a-z0-9._-]`；
- 推荐 reverse-domain namespace，例如 `com.example.counter`；
- Major 精确匹配；
- Minor 选择双方共同支持的最高版本；
- ID 不存在 → `NOT_FOUND`；
- ID 存在但版本无交集 → `UNAVAILABLE`；
- Query 非法 → `INVALID_ARGUMENT`。

## 3.4 InterfaceReference 与函数表

```c
typedef struct rodaq_interface_context
    rodaq_interface_context_t;

typedef struct rodaq_interface_vtable
    rodaq_interface_vtable_t;

typedef struct rodaq_interface_reference {
    rodaq_interface_context_t* context;
    const rodaq_interface_vtable_t* vtable;
    uint32_t vtable_size;
    rodaq_interface_version_t negotiated_version;
} rodaq_interface_reference_t;
```

Raw `InterfaceReference` 是 borrowed。具体 Interface SDK 在验证 Interface ID/version/vtable size 后，把 opaque vtable 恢复为对应 typed vtable。

`vtable_size` 的单位固定为 **bytes**，表示从 typed vtable 对象起始地址开始，Provider 承诺可安全读取的字节数。它不是 slot count。

具体 Interface SDK 必须按以下顺序验证 typed vtable，不能先解引用 Header 再检查长度：

```
1. reference.context != nullptr
2. reference.vtable != nullptr
3. reference.vtable_size >= sizeof(rodaq_struct_header_t)

4. only now read typed_vtable.header.struct_size

5. typed_vtable.header.struct_size >= sizeof(rodaq_struct_header_t)

6. readable_size =
       min(
           reference.vtable_size,
           typed_vtable.header.struct_size,
           sizeof(consumer_known_vtable)
       )

7. readable_size >= required_prefix_size
8. validate each required/optional slot by byte range
```

这只能防止“合法地址 + 错误长度/布局”造成的越界读取；V1 不提供不可信 DSO 的进程内内存隔离，因此不能防止恶意 Plugin 返回任意无效地址。

Consumer 只有在以下条件全部满足时才能访问某个 slot：

```
slot introduced minor <= negotiated minor
AND required_prefix_size <= readable_size
AND offsetof(TypedVTable, slot) + sizeof(slot) <= readable_size
AND function pointer != nullptr
```

因此 ABI 兼容检查使用 byte range，而不是比较 Provider/Consumer 的完整 `sizeof`。新增 Minor slot 只能尾部追加。

Query 调用前 output 必须初始化为唯一 default-invalid 表示：

```c
{
    .context = NULL,
    .vtable = NULL,
    .vtable_size = 0,
    .negotiated_version = {0, 0}
}
```

Query 返回 non-OK 后 output 必须保持该 default-invalid 状态。

## 3.5 配置 ABI

Plugin Configuration 属于 Plugin 私有语义。Framework 只负责把有界的 UTF-8 JSON 文本传给 Plugin，不解析配置字段，也不执行 Plugin-specific Schema validation。

```c
typedef struct rodaq_config {
    rodaq_struct_header_t header;
    rodaq_string_t json;
} rodaq_config_t;
```

规则：

- `json = {nullptr, 0}` 表示未提供 Plugin-owned 配置；
- 非空内容按 Public String 规则必须是 UTF-8；
- Framework 只执行通用大小限制和基础指针/长度合法性检查；
- `json.size` 必须同时满足 `json.size &lt;= UINT32_MAX` 与 Runtime 的 `max_config_size`；
- JSON syntax、字段结构、跨字段关系、环境依赖和业务语义由 Plugin `configure()` 自己解析与校验；
- 配置无效时 Plugin 返回 `InvalidArgument`，message 用于给出插件自己的诊断；
- `rodaq_config_t` 和 JSON backing 只在当前 `configure()` 调用内有效；
- Foundation 不执行隐式配置迁移或配置格式转换。

Framework V1 不定义通用的结构化配置错误、配置 Schema validator 或配置表单元数据模型。需要字段级结构化错误或 UI 表单描述时，由具体 Plugin / 业务 Interface / Application 配置层定义，而不是进入通用 Plugin Foundation。

## 3.6 Service Registry ABI（Application 服务注册表）

```c
typedef struct rodaq_service_registry_context
    rodaq_service_registry_context_t;

typedef struct rodaq_service_registry_vtable {
    rodaq_struct_header_t header;

    rodaq_status_code_t (*query)(
        rodaq_service_registry_context_t* context,
        const rodaq_interface_query_t* query,
        rodaq_interface_reference_t* interface,
        rodaq_error_t* error);

    rodaq_status_code_t (*enter_call)(
        rodaq_service_registry_context_t* context,
        rodaq_error_t* error);

    void (*leave_call)(rodaq_service_registry_context_t* context);
} rodaq_service_registry_vtable_t;

typedef struct rodaq_service_registry_reference {
    rodaq_service_registry_context_t* context;
    const rodaq_service_registry_vtable_t* vtable;
} rodaq_service_registry_reference_t;
```

`enter_call/leave_call` 是 append-only C ABI extension。Plugin 必须通过
`header.struct_size` 判断 slot 是否可读；新 slot 用于把 Plugin-owned
asynchronous callback 发起的 Application Service 调用纳入 managed-call
self-wait 检测。旧长度 vtable 仍可被安全读取，但无法获得该检测能力。

ServiceRegistry 在传给 Plugin 前已经冻结：

- concurrent read query 必须 thread-safe；
- 返回的 raw Service Interface reference 是 borrowed；
- Plugin 不得修改 Registry；
- Provider object/context 必须覆盖 Plugin 合法调用期；
- Provider executable code 在 V1 中必须 process-resident。

`query()`：

| 项目 | 规则 |
| --- | --- |
| context | 必须属于有效 immutable Registry |
| query | 满足 §3.3 Interface ID/version 规则 |
| interface | 调用前 default-invalid；non-OK 后保持 invalid |
| error | 可以为空，仅用于诊断 |
| ID 不存在 | NotFound |
| 版本无交集 | Unavailable |
| blocking | 有界内存查找，不执行外部 I/O |

## 3.7 Plugin Descriptor

```c
typedef struct rodaq_plugin_descriptor {
    rodaq_struct_header_t header;

    rodaq_string_t plugin_id;
    rodaq_string_t plugin_name;
    rodaq_string_t plugin_version;
} rodaq_plugin_descriptor_t;
```

Descriptor：

- static / Plugin Library lifetime；
- immutable；
- 不包含可调用 Operation；
- 不包含 Plugin-private configuration metadata；
- `plugin_id` 是稳定机器标识；
- `plugin_name` 是面向人的名称；
- `plugin_version` 是 Plugin 发行版本；
- Descriptor 不重复保存 Plugin ABI Minor requirement；
- Manifest 与 Descriptor 交叉验证 Plugin ID / Name / Version；
- Plugin ABI 兼容性以 `rodaq_plugin_abi_version()` 为唯一运行时事实源。

Loader 在调用任何 Plugin-provided lifecycle function 前必须完成 Descriptor ingress validation：

| 字段 | V1 验证规则 |
| --- | --- |
| `plugin_id` | 1～255 bytes；lowercase ASCII `[a-z0-9._-]`；推荐 reverse-domain；不得包含 embedded NUL |
| `plugin_name` | 1～255 bytes；有效 UTF-8；不得包含 embedded NUL |
| `plugin_version` | 1～128 bytes；ASCII；必须是完整、严格合法的 Semantic Versioning 2.0.0 字符串；不得包含 embedded NUL |

任何字段违反通用 String contract、长度、字符集或 SemVer 格式时，Library 在 publish 前被拒绝加载。Descriptor 与 Manifest 的 ID / Name / Version 必须字节级一致。

## 3.8 Foundation Instance ABI

Foundation 只保留四个动作：

```
configure
query
shutdown
destroy
```

ServiceRegistry 与 Plugin config 在 `configure()` 时一起传入。

Foundation ABI 的 nullability 固定如下：

| Function | 参数 | Nullability / 默认值 |
| --- | --- | --- |
| configure | context | non-null |
| configure | services | non-null；没有 Service 时传入 empty frozen Registry reference |
| configure | config | non-null；没有 Plugin config 时使用 `config->json = {nullptr, 0}` |
| configure | error | nullable |
| query | context / query / interface | non-null |
| query | error | nullable |
| shutdown | context | non-null |
| shutdown | error | nullable |
| destroy | context | non-null |

`rodaq_plugin_instance_t` 的唯一 default-invalid 表示为 `{nullptr, nullptr}`。`create()` 调用前由 Runtime 写入该值；任何 non-OK 正常失败都必须保持该值。

```c
typedef struct rodaq_plugin_context
    rodaq_plugin_context_t;

typedef struct rodaq_plugin_instance {
    rodaq_plugin_context_t* context;
    const struct rodaq_plugin_instance_vtable* vtable;
} rodaq_plugin_instance_t;

typedef struct rodaq_plugin_instance_vtable {
    rodaq_struct_header_t header;

    rodaq_status_code_t (*configure)(
        rodaq_plugin_context_t* context,
        const rodaq_service_registry_reference_t* services,
        const rodaq_config_t* config,
        rodaq_error_t* error);

    rodaq_status_code_t (*query)(
        rodaq_plugin_context_t* context,
        const rodaq_interface_query_t* query,
        rodaq_interface_reference_t* interface,
        rodaq_error_t* error);

    rodaq_status_code_t (*shutdown)(
        rodaq_plugin_context_t* context,
        rodaq_deadline_t deadline,
        rodaq_error_t* error);

    void (*destroy)(
        rodaq_plugin_context_t* context);
} rodaq_plugin_instance_vtable_t;
```

### configure

`configure()` 同时完成：

- 接收已经冻结的 Application ServiceRegistry；
- 接收 Plugin-owned JSON configuration；
- 调用 Backend `PluginBase::configure(PluginSetup&)`；
- 构造并冻结 Plugin-side Operation Registry。

成功后，Runtime 才把 Plugin Instance 发布为 Open。

如果 configure 失败：

- 不发布半初始化 Operation Registry；
- Runtime 调用 `shutdown()` 清理可能已经建立的部分资源；
- shutdown 成功后调用 destroy；
- Application 只收到原始 configure failure，不得到半初始化 PluginInstance。

### query

`query()` 只允许对已经成功配置的 Instance 调用。

- output 进入调用前 default-invalid；
- ID 不存在 → NotFound；
- Query 非法 → InvalidArgument；
- 版本无交集 → Unavailable；
- 必须是有界只读 lookup，不执行长期 I/O 或业务资源创建。

Raw InterfaceReference 是 borrowed；C++ `OperationHandle` 可以跨 query 调用保存，但每次真正 Operation 调用仍重新进入 Runtime call admission。

### shutdown

`shutdown()` 的含义是：

> Plugin 停止产生新的 Instance-dependent 后台工作，停止并收敛已有 worker/callback/task，使该 Instance 可以安全 destroy。
>

它必须对任意已经成功 create、但可能只完成部分 configure 的实例安全。

`shutdown() == Ok` 必须证明：

- Plugin-owned thread/task/executor/callback/source 已终止，或已经完成与 Instance 无关的安全 ownership handoff；
- 之后不会再产生新的 Instance-dependent asynchronous work；
- borrowed Service Reference 不再被 Plugin-owned work 使用，除非对应 Service Interface 明确定义独立 ownership handoff。

`shutdown()` 不表示 Acquisition Stop 成功，也不表示 Storage Finalize 成功。

### destroy

`destroy()`：

- 只在 `shutdown() == Ok` 且 Runtime 的 active ABI call 已全部返回后调用；
- 最多一次；
- no-status；
- 不得与其他 Instance ABI call 并发；
- Plugin destructor 必须 no-throw；
- 返回后 raw context 不再可访问。

### Foundation 函数规则

| Function | 前置条件 | 失败/输出 | 阻塞与并发 |
| --- | --- | --- | --- |
| configure | context、services、config 有效；Instance 尚未发布 | failure 不发布部分 Operation Registry | control-plane；允许执行 Backend 初始化 |
| query | Instance 已 Open；Runtime 已取得统一 CallGuard | output default-invalid；按 ID/version 返回结果 | bounded read-only lookup |
| shutdown | Instance 正在关闭或初始化失败清理 | non-OK 表示安全停止未证明 | 可以阻塞；Runtime 不强制取消 |
| destroy | shutdown 成功且 active_call_count == 0 | no-status；最多一次 | 不与其他 ABI call 并发 |

## 3.9 Plugin ABI 引导与入口

Bootstrap 不拥有独立版本域。

```c
#define RODAQ_PLUGIN_ABI_ENCODE(major, minor)     ((((uint32_t)(major)) << 16u) | ((uint32_t)(minor) & 0xffffu))

#define RODAQ_PLUGIN_ABI_MAJOR(value)     ((uint16_t)(((uint32_t)(value)) >> 16u))

#define RODAQ_PLUGIN_ABI_MINOR(value)     ((uint16_t)((uint32_t)(value) & 0xffffu))

RODAQ_PLUGIN_EXPORT_SYMBOL uint32_t rodaq_plugin_abi_version(void);

RODAQ_PLUGIN_EXPORT_SYMBOL
const rodaq_plugin_entry_t* rodaq_plugin_get_entry(void);
```

兼容判定：

```
runtime.major == plugin_abi_version.major
AND runtime.minor >= plugin_abi_version.minor
```

其中：

- `rodaq_plugin_abi_version()` 返回 Plugin 构建并依赖的 Plugin ABI Major/Minor；
- Plugin ABI Minor 同时表示该 Plugin 所需的最低 Runtime Minor；
- Manifest 镜像同一个 Plugin ABI Major/Minor，只用于 `dlopen()` 前预筛选；
- DSO 加载后必须将 Manifest ABI 与 `rodaq_plugin_abi_version()` 交叉校验；
- Plugin ABI Minor 通过 append-only struct/vtable prefix 演进。

Entry：

```c
typedef struct rodaq_plugin_entry {
    rodaq_struct_header_t header;

    const rodaq_plugin_descriptor_t* (*descriptor)(void);

    rodaq_status_code_t (*create)(
        rodaq_plugin_instance_t* instance,
        rodaq_error_t* error);
} rodaq_plugin_entry_t;
```

加载验证：

```
dlopen(RTLD_NOW | RTLD_LOCAL)
→ resolve Plugin ABI version
→ validate ABI Major
→ resolve Entry
→ validate Entry required prefix
→ validate Descriptor
→ Manifest / Descriptor cross-check
→ publish PluginLibraryState
```

Create：

- 成功后 Instance context/vtable 必须完整有效；
- 失败后 output 必须保持 default-invalid；
- create 不读取 Service/config，不启动长期业务 worker。

如果 create 返回异常结果：

| 返回结果 | Runtime 处理 |
| --- | --- |
| OK + valid Instance | 进入后续 configure |
| non-OK + default-invalid | 正常失败 |
| OK + malformed | Plugin ABI violation → PluginLibraryState.phase = Faulted |
| non-OK + non-default | Plugin ABI violation → PluginLibraryState.phase = Faulted |

ABI violation 时：

- 不调用未经验证的 function pointer；
- 不尝试猜测 destroy；
- 禁止该 Library 后续 create；
- 记录 critical diagnostic；
- `PluginManagerState.restart_required = true`；
- 已加载 DSO 继续常驻到进程退出。

## 3.10 ABI 兼容演进规则

### 完全冻结类型

适用于 small value、array element、固定视图。发布后 layout 不变。所有 `struct_size` / `vtable_size` 字段的单位统一为 bytes。

### 可尾部扩展类型

适用于 vtable、Descriptor、request/response struct。

规则：

- 只允许尾部追加；
- required prefix 明确；
- 旧字段顺序、类型和语义不变；
- 同一 Plugin ABI Major 下，既有函数的成功、失败、ownership、blocking、threading、shutdown 语义必须保持向后兼容；
- 不使用 `struct_size == sizeof(current)` 判断兼容；
- array element 不通过尾部追加改变 stride。

Provider-returned const struct：

```
readable = min(provider.struct_size, consumer_known_size)
```

Caller-provided output struct：

- Caller 先写自己的 `struct_size`；
- Provider 最多写双方都知道的 size；
- 不得按新版本 `sizeof` 覆盖旧 Caller buffer。

## 3.11 异常与 Destroy 边界

除 destructor `noexcept` 违约外，Plugin SDK trampoline 必须阻止 C++ exception 越过 C ABI。

Plugin destructor 必须 no-throw。若 `noexcept` 析构链仍抛异常，语言语义可能触发 `std::terminate()`；Framework 不承诺进程内恢复。

该风险通过：

- 编码规范；
- 静态检查；
- 隔离子进程故障测试；

降低，而不是通过更复杂的 Runtime recovery 对象掩盖。

## 3.12 Callback 与延迟工作所有权

任何 callback/deleter 如果可能在当前 ABI Call 返回后继续执行，都必须满足：

```
Callable Safe
⇒ Code Alive
∧ Context Alive
∧ Backing Alive
```

V1 Plugin DSO 成功加载后保持常驻，因此 Plugin code mapping 在进程退出前有效；这不自动保证 callback context 或 backing 有效。

具体 Interface 必须定义：

- callback/deleter descriptor；
- callback 线程与并发语义；
- drain / stop；
- context/backing ownership；
- 如果工作需要在 Plugin Instance destroy 后继续，如何在 `shutdown()` 返回 Ok 前完成与 Instance 无关的 ownership handoff。

普通 Service `Reference` 是 borrowed；默认不得被 shutdown 之后仍继续运行的 Plugin work 使用。只有对应 Service Interface 明确定义独立 ownership handoff 时才允许例外。

Framework 不在 Stable C ABI 中暴露 `shared_ptr`、通用 retain/release RPC 或动态代码生命周期 token。

## 3.13 Interface 规范模板

每一个 Operation / Service Interface 必须说明：

| 类别 | 必须定义 |
| --- | --- |
| Identity | Interface ID、Major/Minor |
| Vtable | required prefix、可选 tail、Provider version |
| Input / Output | nullability、size、borrowed/owned、失败后 output 状态 |
| Threading | concurrent / reentrant / affinity |
| Blocking | 是否阻塞、deadline、迟返 |
| Lifecycle | 哪些 Plugin Instance 状态允许 query/call |
| Callback | thread、drain、ownership handoff |
| Ownership | context、buffer、backing、deleter |
| Error | C status / C++ Result、partial result、retry |
| Readiness | absent 与 temporarily unavailable 的区别 |
| Performance | allocation、lock、scheduling 约束 |

## 3.14 Plugin ABI 验证矩阵

Runtime 按阶段验证：

| 对象 | 兼容域 | 必须验证 | 失败处理 |
| --- | --- | --- | --- |
| Bootstrap symbols | Plugin ABI | version symbol + Entry getter | 拒绝加载 |
| Plugin Entry | Plugin ABI | struct size、descriptor、create | 拒绝加载 |
| Plugin Descriptor | Plugin metadata | ID、Name、Plugin Version | 拒绝加载 |
| Foundation VTable | Plugin ABI | configure、query、shutdown、destroy | Instance 不发布 |
| Typed Operation VTable | Interface Version | Interface-specific required prefix | Query 失败 |
| Service Registry VTable | Plugin ABI / Interface | query slot | configure 失败 |

Plugin ABI metadata cross-check：

```
manifest.plugin.abi.major == plugin_abi_version().major
manifest.plugin.abi.minor == plugin_abi_version().minor
runtime.plugin_abi.major == plugin_abi_version().major
runtime.plugin_abi.minor >= plugin_abi_version().minor
```

### create() 后验证

`create()` 返回 OK 后，在 configure 之前必须验证：

- instance context 非空；
- Foundation vtable 非空；
- `struct_size` 覆盖 required prefix；
- configure / query / shutdown / destroy required slot 非空。

create 结果异常时：

```
do not call unvalidated function pointers
→ mark PluginLibraryState Faulted
→ reject further create_instance()
→ emit critical diagnostic
→ PluginManagerState.restart_required = true
```

未知 raw context 不解引用、不尝试 destroy；Library 进入 Faulted，最终由进程退出回收地址空间。

# 4. C++17 Plugin SDK 详细设计

本章定义插件开发者实际使用的 C++17 SDK。SDK 只封装第 3 章的稳定 C ABI，不增加第二套生命周期模型。

## 4.1 对外 SDK 接口与内部链接模型

公开构建入口只有：

| 使用者 | 稳定入口 |
| --- | --- |
| 插件开发者 | `rodaq_register_plugin()` |
| 应用开发者 | `rodaq::plugin` |
| 纯 C Plugin / Interface | installed Stable C ABI headers |
| 业务 Interface 使用者 | 对应 Interface SDK，例如 `rodaq::acquisition` |

普通插件项目不选择或链接 Framework 内部 authoring/runtime target。

```
add_library(my_plugin SHARED ...)
        ↓
rodaq_register_plugin(my_plugin ...)
        ↓
mark existing target as RoDAQ Plugin
→ inject public authoring support
→ generate Descriptor / Manifest / ABI entry
→ apply visibility / install / package rules
```

`rodaq_register_plugin()` 只注册已经存在的 CMake target，不替代 `add_library()`，也不接受 `SOURCES`。这样 Target 创建、Target 自身编译配置与 RoDAQ Plugin 注册保持职责分离。

Framework 内部 target 名称和拆分属于实现细节，可以在不影响第三方工程的情况下改变。

## 4.2 PluginBase 与 PluginSetup

插件开发者只实现两个生命周期方法：

```cpp
namespace rodaq::plugin {

class PluginBase {
public:
    virtual ~PluginBase() noexcept = default;

    PluginBase(const PluginBase&) = delete;
    PluginBase& operator=(const PluginBase&) = delete;

    virtual Result<void> configure(PluginSetup& setup) = 0;
    virtual Result<void> shutdown(Deadline deadline) = 0;

protected:
    PluginBase() = default;
};

}  // namespace rodaq::plugin
```

`PluginSetup` 只在当前 `configure()` 调用期间有效：

```cpp
class PluginSetup {
public:
    PluginSetup(const PluginSetup&) = delete;
    PluginSetup& operator=(const PluginSetup&) = delete;
    PluginSetup(PluginSetup&&) = delete;
    PluginSetup& operator=(PluginSetup&&) = delete;

    std::string_view config() const noexcept;

    template <typename ServiceInterface>
    Result<typename ServiceInterface::Reference> service();

    template <typename OperationInterface, typename Implementation>
    Result<void> provide(Implementation& implementation);
};
```

插件开发者通过它完成三件事：

1. 读取 Application 提供的 Plugin Configuration；
2. 查询 Application 提供的 Service；
3. 发布当前 Plugin Instance 提供的 Operation。

规则：

- `PluginSetup&` 不得保存到 configure 调用之外；
- `service&lt;T&gt;()` 缺失时返回 `NotFound`；Plugin 自己决定该 Service 是必需还是可选；
- Service `Reference` 是 borrowed，不延长 Registry/Provider lifetime；
- `provide()` 只能在 configure 期间调用；configure 成功后 Operation 集合冻结；
- `provide()` 不取得 Implementation ownership；Implementation 必须由 Plugin Instance-owned object 持有；
- `configure()` 每个 Instance 只调用一次；
- `shutdown()` 必须能够清理 configure 已经建立的全部或部分资源；
- destructor 必须 `noexcept`。

## 4.3 SDK 生命周期桥接

SDK 每个 Instance 使用内部 `InstanceContext`：

```
InstanceContext
├── Concrete Plugin Object
├── validated ServiceRegistry binding
└── Frozen Operation Registry
```

Foundation `configure()`：

```
validate services / config
→ create OperationRegistry builder
→ construct PluginSetup(config, services, builder)
→ PluginBase::configure(setup)
→ validate / freeze Operation Registry
→ publish Frozen Operation Registry
→ return
```

Foundation `shutdown()` 直接桥接到：

```cpp
PluginBase::shutdown(deadline)
```

Foundation `query()` 只查询已经冻结的 Operation Registry。

Plugin 开发者不接触：

- Service bind 阶段；
- OperationRegistry；
- Foundation vtable；
- query trampoline；
- destroy；
- PluginInstanceState 的 Open/Closing/Closed 状态转换。

## 4.4 配置访问

```cpp
std::string_view PluginSetup::config() const noexcept;
```

语义：

- empty view 表示 Application 没有提供 Plugin-owned config；
- non-empty 内容是 UTF-8 JSON 文本；
- Framework 只保证通用 size / string contract，不理解 Plugin 配置结构；
- Plugin 在 `configure()` 中负责 JSON parse、字段校验、跨字段约束、环境检查和默认值处理；
- 配置无效时返回 `InvalidArgument`，并由 Plugin 提供可读诊断；
- view 只在当前 configure 调用内有效；
- Plugin 需要长期使用时必须复制或解析到 Instance-owned state。

这使配置所有权保持清晰：Framework 负责传递，Plugin 负责解释。

## 4.5 Interface SDK 与 Provider 模板

Interface SDK 负责：

1. Interface Definition；
2. Operation 的 typed `Handle` 或 Service 的 typed `Reference`；
3. 可复用 Provider Template；
4. C ABI trampoline 与异常转换。

Operation 示例：

```cpp
struct ExampleInterface {
    using Vtable = rodaq_example_operation_vtable_t;
    using Handle = ExampleHandle;

    static constexpr std::string_view kInterfaceId =
        "com.example.operation";
    static constexpr std::uint16_t kMajor = 1;
    static constexpr std::uint16_t kMinor = 0;

    template <typename Implementation>
    using Provider = ExampleProvider<Implementation>;
};
```

Service 示例：

```cpp
struct LoggingInterface {
    using Vtable = rodaq_logging_service_vtable_t;
    using Reference = LoggingReference;

    static constexpr std::string_view kInterfaceId =
        "com.rodaq.service.logging";

    template <typename Implementation>
    using Provider = LoggingProvider<Implementation>;
};
```

Provider Template 负责：

- 编译期检查 Implementation 方法；
- 生成 static typed vtable；
- C ABI trampoline；
- context → Implementation 转换；
- `Result&lt;T&gt;` / Status 转换；
- 阻止 exception 越过 C ABI。

Operation `Handle` 是受管调用能力；Service `Reference` 是 borrowed Application Service 引用。

## 4.6 OperationRegistry（SDK 内部）

`OperationRegistry` 只存在于 SDK configure bridge 内部：

```cpp
class OperationRegistry {
public:
    template <typename Interface, typename Implementation>
    Result<void> add(Implementation& implementation);

private:
    Result<void> freeze();
};
```

规则：

- 普通插件开发者只调用 `PluginSetup::provide()`；
- `OperationRegistry::add()` 在 SDK 内部完成 Provider binding 的校验和保存；
- raw context/vtable insertion 不形成独立 Public/Design API；
- duplicate Interface ID/Major → Conflict；
- invalid vtable/prefix/version → InvalidArgument；
- configure 失败时整个 Registry 构建结果丢弃，不发布部分 Operation；
- configure 成功后形成 immutable registry。

## 4.7 PluginSetup 的 Application Service 访问

统一入口：

```cpp
template <typename ServiceInterface>
Result<typename ServiceInterface::Reference>
PluginSetup::service();
```

行为：

- ID 不存在 → NotFound；
- ID 存在但版本不兼容 → Unavailable；
- Registry/ABI 非法 → 对应错误；
- 返回 typed borrowed `Reference`；
- Framework 不区分 Required/Optional Service；
- Plugin 根据自身业务决定 NotFound 是否导致 configure 失败。

普通 Service Reference 默认只能在当前 Plugin Instance 有效期间使用。所有使用 Service Reference 的 Plugin-owned background work 必须在 `shutdown() == Ok` 前结束，除非该 Service Interface 明确定义独立 ownership handoff。

## 4.8 导出辅助与自动生成元数据

插件开发者只写：

```cpp
RODAQ_DEFINE_PLUGIN(MyPlugin)
```

`rodaq_register_plugin()` 与 `RODAQ_DEFINE_PLUGIN` 自动生成：

- Plugin ABI version symbol；
- Entry getter；
- static Descriptor；
- create/configure/query/shutdown/destroy trampoline；
- Foundation vtable；
- visibility/export declarations；
- exception barrier；
- Manifest metadata。

V1 的 Concrete Plugin type 必须满足：

- default-constructible；
- destructor `noexcept`；
- constructor 不执行可能长期阻塞的文件/网络/设备 I/O；
- constructor 不启动长期 worker、callback source 或后台 executor；
- 真实 Backend 初始化、Service 查询和 Operation publish 全部发生在 `configure()`。

`RODAQ_DEFINE_PLUGIN` / authoring support 必须对可静态验证的条件执行 `static_assert`。

Create：

```
default-invalid output
→ allocate SDK InstanceContext
→ construct Concrete Plugin Object
→ publish context + static Foundation vtable
```

所有 SDK trampoline 都必须阻止 C++ exception 越过 C ABI，映射规则统一为：

| Trampoline | `std::bad_alloc` | 其他 `std::exception` / unknown exception | Runtime 后续处理 |
| --- | --- | --- | --- |
| create | ResourceExhausted | Internal + bounded diagnostic | 不发布半初始化 Instance |
| configure | ResourceExhausted | Internal + bounded diagnostic | 保留原错误并进入初始化失败 cleanup |
| query / typed Provider | ResourceExhausted | Internal + bounded diagnostic | output 保持 default-invalid / 不发布 partial result |
| shutdown | ResourceExhausted | Internal + bounded diagnostic | shutdown 未证明成功；进入关闭失败/Unresolved 收敛 |
| destroy | 不允许异常 | 不允许异常 | destructor/noexcept 违约按 C++ `std::terminate` 语义处理 |

Destroy：

- 只在 Runtime 已证明 `shutdown() == Ok` 且 active ABI call 已归零后调用；
- 销毁 Frozen Operation Registry 和 Concrete Plugin Object；
- destructor 必须 no-throw；
- 只执行一次；
- 不重试。

## 4.9 第三方 Counter 完整规范性示例

本节用于证明：Interface SDK 可以封装全部 C ABI 适配，而两个独立 Backend Plugin 只实现业务 C++ 类。

### Interface SDK：C ABI

```c
#define RODAQ_COUNTER_INTERFACE_ID "com.example.counter"

typedef struct rodaq_counter_vtable {
    rodaq_struct_header_t header;

    rodaq_status_code_t (*increment)(
        rodaq_interface_context_t* context,
        int64_t delta,
        int64_t* value,
        rodaq_error_t* error);

    rodaq_status_code_t (*current)(
        rodaq_interface_context_t* context,
        int64_t* value,
        rodaq_error_t* error);
} rodaq_counter_vtable_t;
```

### Interface SDK：定义、Handle 与 Provider 模板

```cpp
class CounterHandle;

template <typename Implementation>
struct CounterProvider;

struct CounterInterface {
    using Vtable = rodaq_counter_vtable_t;
    using Handle = CounterHandle;

    static constexpr std::string_view kInterfaceId =
        RODAQ_COUNTER_INTERFACE_ID;
    static constexpr std::uint16_t kMajor = 1;
    static constexpr std::uint16_t kMinor = 0;

    static constexpr std::size_t required_prefix_size() noexcept;

    template <typename Implementation>
    using Provider = CounterProvider<Implementation>;
};

class CounterHandle {
public:
    Result<std::int64_t> increment(std::int64_t delta);
    Result<std::int64_t> current() const;

private:
    friend struct CounterInterface;
    OperationHandle<CounterInterface> operation_;
};
```

`CounterProvider&lt;Implementation&gt;` 由 Counter Interface SDK 实现，包含 compile-time method checks、typed vtable 与 C ABI trampoline。Backend Plugin 不实现 Adapter。

### Backend A

```cpp
class AtomicCounter {
public:
    Result<std::int64_t> increment(std::int64_t delta);
    Result<std::int64_t> current() const;

private:
    std::atomic<std::int64_t> value_{0};
};

class AtomicCounterPlugin final : public PluginBase {
public:
    Result<void> configure(PluginSetup& setup) override {
        return setup.provide<CounterInterface>(counter_);
    }

    Result<void> shutdown(Deadline) override {
        return {};
    }

private:
    AtomicCounter counter_;
};

RODAQ_DEFINE_PLUGIN(AtomicCounterPlugin)
```

### Backend B

```cpp
class SharedMemoryCounter {
public:
    explicit SharedMemoryCounter(SharedCounterState& state);

    Result<std::int64_t> increment(std::int64_t delta);
    Result<std::int64_t> current() const;

private:
    SharedCounterState& state_;
};

class SharedMemoryCounterPlugin final : public PluginBase {
public:
    Result<void> configure(PluginSetup& setup) override {
        return setup.provide<CounterInterface>(counter_);
    }

    Result<void> shutdown(Deadline) override {
        return {};
    }

private:
    SharedCounterState state_;
    SharedMemoryCounter counter_{state_};
};

RODAQ_DEFINE_PLUGIN(SharedMemoryCounterPlugin)
```

两个 Backend 不包含 Adapter、vtable、trampoline、Descriptor、Manifest 或 Plugin ABI metadata。

### 调用方（Consumer）调用路径

`PluginInstance::query&lt;CounterInterface&gt;()` 返回 `Result&lt;CounterHandle&gt;`。

```
CounterHandle
→ OperationHandle<CounterInterface>
→ acquire Runtime CallGuard
→ validate negotiated version / vtable_size / slot
→ call rodaq_counter_vtable_t
→ translate status_code to Result<T>
→ release Runtime CallGuard
```

资格验证必须证明两个 Backend 可以只依赖同一个 Counter Interface SDK 独立构建。

# 5. 插件运行时内部实现设计

本章说明 Application 看到的 `PluginManager / PluginLibrary / PluginInstance` 如何映射到内部状态。V1 只保留三个主要共享状态对象：`PluginManagerState`、`PluginLibraryState`、`PluginInstanceState`。

## 5.1 对外 API

### PluginManagerOptions

V1 只公开真正需要由 Application 配置的参数：

```cpp
struct PluginManagerOptions {
    std::vector<std::filesystem::path> search_paths;

    std::size_t max_loaded_plugins;
    std::size_t max_instances;

    /**
     * Maximum Plugin configuration size.
     *
     * Unit: bytes.
     */
    std::size_t max_config_size;

    DeadlineDuration shutdown_timeout;
};
```

`PluginManager::create()` 必须验证 `max_config_size &lt;= UINT32_MAX`，因为 Stable C ABI 的 `rodaq_string_t::size` 是 `uint32_t`；超出范围返回 InvalidArgument。配置长度超过 `max_config_size` 时，在进入 Plugin ABI 前统一返回 ResourceExhausted；该行为不依赖 message 判断。

close worker 数、内部 queue capacity、load single-flight bookkeeping 等使用 Framework 内部经过资格验证的默认值，不提前冻结为 Public API。Plugin 配置解析不属于 Framework。

### PluginInfo

```cpp
struct PluginInfo {
    std::string plugin_id;
    std::string plugin_name;
    std::string plugin_version;
};

struct PluginArtifactInfo {
    std::string plugin_id;
    std::string plugin_version;
    std::string sha256;
};
```

`PluginInfo` 是面向发现、选择和 UI 的稳定静态摘要；`PluginArtifactInfo` 是面向运行审计的只读实际 Artifact 身份，只暴露 Session metadata 所需事实，不暴露 canonical path、inode、native handle 等 Loader 实现细节。

### PluginManager

```cpp
class PluginManager {
public:
    static Result<PluginManager> create(PluginManagerOptions options);

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    PluginManager(PluginManager&&) noexcept;
    PluginManager& operator=(PluginManager&&) noexcept;
    ~PluginManager() noexcept;

    Result<void> refresh();

    Result<std::vector<PluginInfo>> plugins() const;

    // 选择最高兼容的已安装版本。
    Result<PluginLibrary> load(std::string_view plugin_id);

    // 选择精确 Plugin Version。
    Result<PluginLibrary> load(
        std::string_view plugin_id,
        std::string_view plugin_version);

    Result<PluginDiagnostics> diagnostics() const;

    Result<void> shutdown(Deadline deadline);
};
```

V1 一个进程只允许一个逻辑 PluginManager authority。第一次 `create()` 成功后，该 authority 持续到进程退出；即使 Manager 后续进入 Stopped，也不支持在同一进程重新创建第二套 PluginManager。之后再次 `create()` 返回 Conflict。

`PluginManager` 是 move-only Public Handle。move 后源对象只允许析构或再次 move-assign。析构函数 `noexcept` 且不得阻塞等待 Plugin/native shutdown：

- 正常退出路径必须显式调用 `shutdown()` 并检查结果；
- 如果最后一个 Public Manager Handle 析构时 Framework 仍处于 Running/ShuttingDown，Framework 只触发或继续 best-effort 非阻塞收敛并记录 critical diagnostic，不把析构视为成功 shutdown；
- process-level `PluginManagerState` 继续持有未 Closed/Unresolved Instance，直到内部收敛完成或进程退出。

`shutdown()` 是幂等观察接口：

- Running：开始一次 Manager shutdown 并等待该同一流程；
- ShuttingDown：继续等待同一流程；
- Stopped：返回 Ok；
- Degraded：返回已记录的终态失败；Framework 仍可以在后台继续完成能够安全完成的迟返清理，但不会重新恢复为正常 authority。

`shutdown()` 开始后：

- 新 `refresh()` 返回 InvalidState；
- 新 `load()` 返回 InvalidState；
- 新 `create_instance()` 返回 InvalidState；
- 已经进入的 refresh/load/create 允许正常返回；late refresh 不得发布新 Catalog generation；
- Framework 开始收敛尚未关闭的 Plugin Instance。

### PluginLibrary

```cpp
class PluginLibrary {
public:
    PluginLibrary() = delete;
    PluginLibrary(const PluginLibrary&) = delete;
    PluginLibrary& operator=(const PluginLibrary&) = delete;
    PluginLibrary(PluginLibrary&&) noexcept;
    PluginLibrary& operator=(PluginLibrary&&) noexcept;
    ~PluginLibrary() noexcept;

    explicit operator bool() const noexcept;

    const PluginInfo& info() const noexcept;
    const PluginArtifactInfo& artifact() const noexcept;

    Result<PluginInstance> create_instance(
        const ServiceRegistry& services,
        std::string_view config) const;
};
```

### PluginInstance

```cpp
enum class PluginInstancePhase {
    Initializing,
    Open,
    Closing,
    Closed,
    Unresolved
};

class CloseFuture;

class PluginInstance {
public:
    PluginInstance() = delete;
    PluginInstance(const PluginInstance&) = delete;
    PluginInstance& operator=(const PluginInstance&) = delete;
    PluginInstance(PluginInstance&&) noexcept;
    PluginInstance& operator=(PluginInstance&&) = delete;
    ~PluginInstance() noexcept;

    explicit operator bool() const noexcept;

    template <typename Interface>
    Result<typename Interface::Handle> query();

    Result<void> close(Deadline deadline);
    CloseFuture async_close() noexcept;

    PluginInstancePhase phase() const noexcept;
    PluginInstanceId id() const noexcept;
};
```

Application 只能获得已经 Open 的 `PluginInstance`。

`~PluginInstance()` 不阻塞等待 Plugin/native shutdown：如果当前 Handle 仍引用 Open Instance，它只发起或继续同一个异步关闭流程，然后释放 Public Handle。正常业务路径仍必须显式 `close()` 或 `async_close()` 并检查真实关闭结果。

删除 move-assignment 是刻意的 API 约束：禁止通过 `target = std::move(source)` 隐式覆盖一个仍可能拥有实时业务生命周期的 Instance。需要替换 Handle 时，调用方必须先显式关闭旧 Instance，再构造或移动新的 Handle。

### Public Handle 有效性、移动与线程安全

`PluginLibrary` 是 move-only handle；`PluginInstance` 是 move-constructible、不可复制且不可 move-assign。move 后源对象进入有效但空的 moved-from 状态：

- 可以析构；
- `operator bool() == false`；
- 除赋予新值所必需的构造/销毁操作外，不得调用要求有效 backing 的成员函数。

对返回引用或 `noexcept` 值的访问器，例如 `PluginLibrary::info()`、`PluginLibrary::artifact()`、`PluginInstance::phase()`、`PluginInstance::id()`，前置条件统一为 `operator bool() == true`；Framework 不因此把这些轻量访问器改成 `Result<T>`。

Public wrapper 对象本身遵循普通 C++ 对象规则：不得在一个线程 move/destroy 同一个 wrapper 的同时，由另一个线程通过该 wrapper 调用成员函数。下面的并发保证针对多个独立 Public Handle 或同一稳定 backing 上的并发调用。

| Public API | 并发语义 |
| --- | --- |
| `PluginManager::refresh()` | 可与 `plugins()/load()` 并发；并发 refresh 使用 single-flight 并观察同一流程；仅 Running 状态可准入和发布 |
| `PluginManager::plugins()` | 返回一个一致 Catalog snapshot |
| `PluginManager::load()` | 观察某个完整 Catalog generation；同一 Artifact 使用 single-flight |
| `PluginManager::shutdown()` | 多线程调用观察同一个 shutdown 流程；幂等 |
| `PluginLibrary::info()/artifact()` | 可并发只读 |
| `PluginLibrary::create_instance()` | 同一 Library 可并发调用，受 Manager/Library capacity 与 Faulted 状态约束 |
| `PluginInstance::query()` | Open 阶段可并发；每次 query 进入统一 CallGuard |
| `PluginInstance::async_close()` | 可并发且幂等；复用同一个 close shared state |
| `PluginInstance::close()` | 多调用方只是观察同一个关闭流程，不创建第二次 shutdown/destroy |
| `PluginInstance::phase()/id()` | 有效 Handle 上可并发只读 |

`refresh()` 的 publication point 是新 Catalog generation 的原子替换点。已经开始的 `load()` 可以继续使用其捕获的旧 generation 完成解析；之后的新 `load()` 使用新 generation。

Manager 进入 ShuttingDown/Degraded/Stopped 后，新的 `refresh()` 返回 InvalidState。已经进入的 refresh 可以完成内部扫描和解析，但其 Catalog generation 只能在 publication point 观察到 Manager == Running 时发布；否则必须丢弃并返回 InvalidState。并发 refresh 不创建第二个 Catalog build，后到调用等待并观察同一 refresh 结果。refresh 的调度、扫描/解析持续时间和发布阶段都进入 Lifecycle Supervision。

`CloseFuture` 是可复制、可移动的 shared-state observer。复制 Future 不创建新的关闭流程，也不延长 Plugin 业务资源生命周期；多个 Future 只观察同一个 `PluginInstanceState` terminal publication。

### CloseFuture

```cpp
enum class CloseWaitStatus {
    Completed,
    TimedOut
};

class CloseFuture {
public:
    CloseFuture() = delete;
    CloseFuture(const CloseFuture&) noexcept = default;
    CloseFuture& operator=(const CloseFuture&) noexcept = default;
    CloseFuture(CloseFuture&&) noexcept;
    CloseFuture& operator=(CloseFuture&&) noexcept;

    explicit operator bool() const noexcept;
    bool ready() const noexcept;

    // 只表示本次等待是否观察到 terminal close。
    CloseWaitStatus wait(Deadline deadline) const noexcept;

    // 返回真实 close terminal result。
    // Future 尚未 ready 时返回 InvalidState。
    Result<void> result() const;
};
```

`close()` 是普通同步接口，其语义等价于：

```
future = async_close()
status = future.wait(observer_deadline)

TimedOut  → return DeadlineExceeded
Completed → return future.result()
```

因此：

- `close().ok()` 只表示真实关闭已经成功完成；
- 同步 `close()` 是 convenience API，`DeadlineExceeded` 可能来自两种来源：Observer Wait 超时，或真实 close terminal result 本身为 `DeadlineExceeded`；
- 同步 `Result&lt;void&gt;` 不携带“DeadlineExceeded 来源”这一额外维度，调用方不得仅凭该 StatusCode 推断来源；
- 需要区分来源的调用方必须使用 `async_close()`：先检查 `CloseFuture::wait()` 是否 TimedOut，再在 Completed 后读取 `CloseFuture::result()`；
- Observer Wait 超时不会取消实际关闭；真实 terminal `DeadlineExceeded` 则表示 Plugin/Framework 已经形成一个完成态失败结果。

`async_close()` 多次调用只复用同一个关闭流程，不创建第二次 shutdown/destroy。

`ready()`、`wait()` 和 `result()` 必须观察同一个 terminal publication point。`result()` 在 Future 尚未 ready 时返回 InvalidState，不依赖调用者违反前置条件后产生未定义行为。

Moved-from `CloseFuture` 是有效但空的 handle：可以析构或重新赋值，`operator bool() == false`；`ready()`、`wait()` 和 `result()` 要求 Future 有效，调用方应先检查 `operator bool()`。

### ServiceRegistry

V1 不再公开独立 Builder 类型：

```cpp
class ServiceRegistry {
public:
    ServiceRegistry();

    ServiceRegistry(const ServiceRegistry&) = delete;
    ServiceRegistry& operator=(const ServiceRegistry&) = delete;
    ServiceRegistry(ServiceRegistry&&) noexcept;
    ServiceRegistry& operator=(ServiceRegistry&&) noexcept;
    ~ServiceRegistry() noexcept;

    template <typename ServiceInterface, typename Implementation>
    Result<void> add_shared(std::shared_ptr<Implementation> implementation);

    template <typename ServiceInterface, typename Implementation>
    Result<void> add_static(Implementation& implementation);

    Result<void> freeze();
    bool frozen() const noexcept;
};
```

规则：

- Public `ServiceRegistry` wrapper 是 move-only；
- Registry 内部使用 immutable shared backing；`freeze()` 后该 backing 不再修改；
- `add_shared()`：Registry backing 强持有 Provider object；
- `add_static()`：只用于进程级静态 Provider；Registry 不取得 Provider object ownership；
- `create_instance()` 只接受 frozen ServiceRegistry，并把同一个 immutable backing 的强引用捕获到 `PluginInstanceState`；
- 因此调用 `create_instance()` 成功后，外部 `ServiceRegistry` wrapper 可以析构，而 Plugin 的 borrowed Service `Reference` 在 Instance 合法调用期仍保持有效；
- 最后一个 backing owner 释放 `add_shared()` Provider 时，Provider destructor/custom deleter 不保证特定线程，但必须在 Framework 控制锁之外执行；
- 如果某个 Provider 要求特定线程销毁，由 Provider 自身实现线程转交，不把 thread-affinity 变成通用 ServiceRegistry 机制；
- Provider executable code 在 V1 中必须 process-resident。

## 5.2 PluginManagerState

`PluginManagerState` 是进程级插件管理状态：

```
PluginManagerState
├── phase
├── PluginCatalog
├── loaded_plugins
├── instances
├── AsyncCloseExecutor
├── active_refresh_count
├── active_load_count
├── active_create_count
├── capacity
├── diagnostics
└── restart_required
```

`PluginManagerState` 是内部长期共享对象；生命周期值使用 Public `PluginManagerPhase`。

Phase 转换：

```
Running
→ ShuttingDown
→ Stopped

Running / ShuttingDown
→ Degraded
```

含义：

- `Running`：允许 refresh/load/create；
- `ShuttingDown`：拒绝新的 refresh/load/create，正在收敛实例；
- `Stopped`：Framework 已满足 Normal Shutdown Success Predicate；
- `Degraded`：存在 Faulted Library、Unresolved Instance、refresh/load/create/configure 生命周期监督超时或卡死、Native Shutdown/close pipeline 超时或 liveness failure，Framework 已无法证明正常收敛，需要进程重启。

Normal Shutdown Success Predicate：

```
restart_required == false
AND no Faulted Library requiring process recovery
AND no Unresolved Instance
AND active_refresh_count == 0
AND active_load_count == 0
AND active_create_count == 0
AND no pending/running refresh work
AND no pending close work
AND no running close work
AND all normal instances == Closed
```

只有满足该谓词，`PluginManager::shutdown()` 才能进入 `Stopped` 并返回 Ok。

`PluginManagerState` 强持有所有 Loaded Library 和所有尚未 Closed 的 Instance，因此不需要额外 Retention Registry。

## 5.3 PluginLibraryState

```
PluginLibraryState
├── NativeLibraryHandle
├── PluginInfo
├── PluginDescriptor
├── PluginEntry
├── ArtifactIdentity

├── instance_count
├── phase
└── diagnostics
```

`PluginLibraryState` 是内部长期共享对象；其阶段使用内部枚举：

```cpp
enum class PluginLibraryPhase {
    Loaded,
    Faulted
};
```

### Loaded

正常可创建 Instance。

### Faulted

如果 create 返回违反 ABI 规范的 output：

```
Loaded
→ Faulted
→ reject future create_instance()
→ manager.restart_required = true
```

Framework 不调用未经验证的 destroy；异常信息直接记录在 Faulted Library 和 Manager diagnostics 中。

如果 Faulted Library 已经存在其他正常 Instance，这些 Instance 仍按正常 close/shutdown 路径关闭；Application 应根据 critical diagnostic 受控重启进程。

成功发布的 Native DSO 在进程退出前保持常驻。

## 5.4 Plugin Instance 创建与初始化监督

`create_instance(services, config)` 是单一初始化入口。V1 把 create/configure 视为受监督生命周期调用，而不是普通无界同步函数。

正常流程：

```
require manager == Running
require library == Loaded
require services.frozen()
validate config size / Public String contract
reserve max_instances slot
allocate PluginInstanceState = Initializing
publish strong Manager ownership
active_create_count++

supervised Plugin ABI create()
→ validate Foundation Instance
→ attach ServiceRegistry
→ supervised Foundation configure(services, config)
→ freeze Operation Registry
→ enter Open Commit Point
```

Open Commit Point：

```
under PluginManagerState lock:

if manager == Running
    phase = Open
    publish PluginInstance
    active_create_count--
    return PluginInstance

if manager != Running
    do not publish Open Instance
    phase = Closing
    publish Manager-owned Pending Close Work
    active_create_count--
    return InvalidState
```

Application 永远不会获得 Initializing Instance。

`create_instance()` 返回 Ok 的线性化点就是 Open Commit Point：

```
create_instance() returns Ok
⇒ returned Instance has already been committed as Open
```

### 初始化生命周期监督

create/configure 必须通过 Framework 的有界 Lifecycle Supervision 执行。监督机制满足：

- native call 不在 Manager structural lock 内执行；
- 调用使用有界 lifecycle execution capacity，不允许每次超时创建新的永久线程；
- 独立 health monitor 可以在执行 worker 被阻塞时继续观测 stage 与持续时间；
- operational threshold 由 Framework qualification / deployment policy 管理，不为每个 Public API 增加独立 Deadline 参数；
- threshold 只限制 Framework 的等待与健康判定，不代表可以取消 native code；
- threshold 耗尽后，等待方可以得到 `DeadlineExceeded`，Manager 进入 Degraded，`restart_required = true`；
- 仍在执行的 native call 及其所需 backing 必须继续被 Manager 强持有，直到真实迟返或进程退出；
- 已公开的 Degraded / restart-required 结论不因迟返自动恢复。

初始化监督阶段至少区分：

```
Create Scheduling
Native Create
Foundation Validation
Native Configure
Operation Freeze
Open Commit
Initialization Cleanup
```

这些是内部 health stage，不新增 Public lifecycle enum。

### Catalog refresh 生命周期监督

Catalog refresh 与 load/create/configure 使用同一套有界 Lifecycle Supervision，但保持独立 stage：

```
Refresh Scheduling
Catalog Scan
Manifest Read / Parse
Catalog Build
Catalog Publish
```

规则：

- 只有 Manager == Running 时才能增加 `active_refresh_count` 并进入 refresh；
- 并发 refresh 使用 internal single-flight，最多一个 Catalog generation build；后到调用等待并观察同一结果；
- native filesystem scan / manifest parse 不在 Manager structural lock 内执行；
- 独立 health monitor 必须能在 refresh worker 被阻塞时观测 stage 和持续时间；
- operational threshold 耗尽后，等待方返回 `DeadlineExceeded`，Manager 进入 Degraded，`restart_required = true`；
- late refresh completion 仍然执行 output validation 和资源清理，但 Manager 已非 Running 时必须丢弃 generation，不替换 Catalog，也不发布新的 Library/Instance；
- refresh 不创建 Public Ticket/Future，也不增加每次调用的 Deadline 参数。

该规则使 Manager shutdown 可以并行关闭现有 Instance，同时等待或监督已经进入的 refresh，而不形成“refresh 卡死则健康实例无法关闭”的顺序。

### Manager shutdown 与初始化并发

如果 Manager shutdown 在 create/configure 完成之前已经从 Running 切换为 ShuttingDown，则 shutdown 获胜：

- 初始化调用可以继续迟返；
- 不发布新的 Open Instance；
- 已得到有效 raw Instance 时，在 native create/configure 返回并且不再并发执行后转入 Closing；
- Framework 发布 Manager-owned Pending Close Work，再执行 shutdown/drain/destroy；
- native create/configure 尚未返回时，不得并发调用同一 Instance 的 shutdown/destroy；
- 如果初始化超过 operational threshold，Manager 进入 Degraded，但其他已经 Open 的健康 Instance 仍必须继续关闭。

### create 失败

如果 create 在建立 Plugin-owned resource 之前正常返回 failure：

```
rollback InstanceState
release capacity
active_create_count--
return original Status
```

如果 create 违反 ABI：

```
mark PluginLibraryState Faulted
manager.phase = Degraded
manager.restart_required = true
release ordinary Instance capacity bookkeeping
active_create_count--
return Internal
```

Framework 不解引用未知 raw context，不调用未经验证的 function pointer，也不猜测 destroy。

如果 create 在 operational threshold 后迟返：

- 先执行正常 ABI output validation；
- Manager 已非 Running 时不得 publish Library/Instance；
- valid Instance 进入初始化失败/Manager-shutdown cleanup；
- malformed output 按 Faulted Library 处理；
- `restart_required` 保持 true。

### configure 失败

configure 失败可能已经创建部分 worker/connection/backing，因此异步清理的所有权必须在原始错误返回前完成提交：

```
Manager already owns PluginInstanceState
→ configure failure
→ preserve original configure Status
→ phase = Closing
→ publish Pending Close Work
→ only then return original configure failure to Application
```

cleanup：

```
Plugin shutdown()
→ if Ok and active_call_count == 0
     destroy
     release ServiceRegistry/backing
     phase = Closed
     release capacity
  else
     phase = Unresolved
     manager = Degraded
     restart_required = true
```

需要特别区分：

```
create_instance() returns failure
≠ cleanup completed
≠ max_instances capacity already returned
```

如果 cleanup 仍在 Closing/Pending Close 中，Application 立即重试可能暂时得到 `ResourceExhausted`。这是安全语义，不引入 FailedInstanceHandle/CleanupFuture。

## 5.5 统一调用准入

Query 与普通 Operation 都属于对 Plugin Instance 的受管 ABI 调用，使用同一套：

```
CallGuard
active_call_count
```

### Query

```
lock PluginInstanceState
require phase == Open
active_call_count++
unlock

call query()
validate InterfaceReference
attach strong PluginInstanceState ownership
construct complete Operation Handle

lock
active_call_count--
unlock
```

CallGuard 在完整 Managed Handle 构造完成后才释放。Handle 构造完成点之后，不得再访问 Provider metadata、raw vtable backing 或其他可能随 Instance close 被销毁的资源。Close 可以使这个 Handle 的后续调用返回 InvalidState，但不能在 Handle 构造过程中销毁其依赖 backing。

### Operation

```
Operation Handle
→ lock PluginInstanceState
→ require phase == Open
→ active_call_count++
→ unlock
→ call typed C ABI vtable
→ active_call_count--
```

“检查 phase == Open”与“增加 active_call_count”必须在同一个线性化边界完成。

Hot path：

- 不取得 PluginManagerState 全局锁；
- 不执行 heap allocation；
- 不跨线程调度；
- 不访问 PluginCatalog。

Query 与 Operation 共用同一个 CallGuard 和 `active_call_count`。

### 锁顺序与锁外执行

如果实现确实需要同时持有 Manager 与 Instance 控制锁，统一顺序为：

```
PluginManagerState lock
→ PluginInstanceState lock
```

不得反向取得。

以下操作不得在 PluginManagerState / PluginInstanceState 控制锁内执行：

- Plugin ABI function；
- Application / Plugin callback；
- Provider destructor；
- `shared_ptr` custom deleter；
- 可能重入 Plugin Framework 的任意第三方代码；
- 可能阻塞的外部 I/O。

需要释放 ServiceRegistry/provider/backing 时，应先在锁内完成 ownership detach 与状态/accounting 提交，再在锁外执行 destructor/deleter。

## 5.6 Plugin Instance 关闭

### async_close()

```
lock PluginInstanceState

Closed:
    return CloseFuture(state)

Unresolved:
    return CloseFuture(state)

Closing:
    return CloseFuture(state)

Open:
    phase = Closing
    atomically publish Pending Close Work

unlock

AsyncCloseExecutor schedules/observes pending work
return CloseFuture(state)
```

`PluginManagerState` 持有所有未 Closed Instance，异步关闭不会因为 Public wrapper 释放而失去所有者。

`Closing` 同时意味着该 Instance 必须存在于 Manager 可发现的 Pending Close Work Set 中。关闭责任不能只存在于一次 queue insertion：

- Open → Closing 与 Pending Work 发布必须形成确定的提交点；
- queue 满或一次调度失败不得丢失 close work；
- worker 完成、queue capacity 恢复、Manager shutdown 等事件必须重新扫描/推进 Pending Work；
- 同一 Instance 同时最多一个 close task 进入真实 Plugin shutdown；
- Pending Work 必须具有有界公平性；
- `async_close() noexcept` 不得因为临时 heap allocation failure 丢失关闭责任。

实现可以使用预留 slot、intrusive node、固定容量索引或等价无丢失机制，但不增加新的 Public 对象。

### close()

```
future = async_close()
wait = future.wait(observer_deadline)

TimedOut  → return DeadlineExceeded
Completed → return future.result()
```

`close().ok()` 只表示 Plugin Instance 已经安全 Closed，并且关闭成功发布点之前要求的 backing release、Manager accounting 和 instance capacity 归还已经完成。

Observer DeadlineExceeded 只表示调用者停止等待，异步关闭继续。

### 阻塞式 close 的调用限制

如果当前线程正在执行同一个 Instance 的 Operation/callback，而该调用本身必须先返回才能完成关闭，则同步 `close()` 不得阻塞等待自己。

规则统一定义为：

> 任何等待条件要求当前 managed Plugin invocation 先退出才能完成的同步生命周期等待，都不得在该 invocation 调用链内阻塞。
>

因此：

- callback/Operation 中允许 `async_close()` 本身；
- callback/Operation 中不得同步等待同一 Instance 的 `close()`；
- callback/Operation 中不得调用 `CloseFuture::wait()` 等待同一 Instance 完成关闭；
- 如果 Manager shutdown 的完成条件包含当前 invocation 返回，则不得在该调用链内同步等待 `PluginManager::shutdown()`；
- Debug/qualification 实现可以使用轻量 TLS 检测明确的 same-thread self-wait 并返回 InvalidState；
- Framework 不承诺检测任意跨线程应用级死锁。

TLS 检测属于实现辅助，不形成新的 Public Runtime 对象。

## 5.7 AsyncCloseExecutor

Plugin `shutdown()` 可能长时间阻塞，因此 destructor 和 `async_close()` 不能直接在调用线程同步执行 shutdown。

Framework 使用内部有界 AsyncCloseExecutor：

- worker 数使用 Framework 固定/内部配置，不进入 Public API；
- 最大 pending close 数自然受 `max_instances` 限制；
- PluginManagerState 持有所有 Closing Instance，因此 queue 满或调度迟延不会产生无主 Instance；
- 不创建无界线程；
- 不使用线程强制取消。

关闭 worker：

```
PluginInstanceState.phase = Closing
→ Plugin shutdown(deadline)
→ if shutdown != Ok
     phase = Unresolved
     manager = Degraded
     restart_required = true
     stop
→ wait active_call_count == 0
→ destroy exactly once
→ detach ServiceRegistry / Instance backing under control lock
→ release detached backing outside control locks
→ commit Manager instance removal / accounting
→ release max_instances capacity
→ close_result = Ok
→ phase = Closed
→ notify CloseFuture observers
```

### Observer Deadline 与 Native Shutdown Deadline

V1 明确区分：

| Deadline | 作用 |
| --- | --- |
| `PluginInstance::close(observer_deadline)` | Application 最多同步等待多久 |
| `CloseFuture::wait(observer_deadline)` | Application 最多等待 Future 多久 |
| `PluginManager::shutdown(observer_deadline)` | Application 最多等待 Manager 收敛多久 |
| `PluginManagerOptions::shutdown_timeout` | Framework 对单个 Plugin Native Shutdown 的策略上限 |

Framework 根据 `shutdown_timeout` 为每次 Foundation / PluginBase `shutdown()` 计算 absolute native shutdown deadline。Observer Deadline 不直接传给 Backend，也不会因为 Application 只愿意等待很短时间而缩短 Plugin 的安全关闭时间。

### 全关闭链路健康监督

Health Monitor 不能只观察 Plugin `shutdown()`。每个 Closing Instance 的完整关闭链路都必须具有内部可观察阶段和开始时间：

| 内部阶段 | 需要监督的内容 |
| --- | --- |
| Pending Scheduling | pending age、公平性、是否持续无法获得 worker |
| Native Shutdown | 调用开始时间、native deadline、迟返 |
| Call Drain | `active_call_count` 与 drain 持续时间 |
| Destroy | Foundation `destroy()` 调用持续时间 |
| Backing Release | ServiceRegistry/provider/custom deleter 释放持续时间 |
| Terminal Publication | close result、capacity/accounting、Future 通知是否完成 |

这些阶段只用于内部监督、Logging/Metrics 和故障诊断，不新增 Public 生命周期枚举或新的长期共享对象。

如果任一阶段超过其经过 Qualification 的 operational threshold：

- 独立于被阻塞 close worker 的 health monitor 必须能够发现；
- Manager 进入 Degraded，设置 `restart_required = true` 并发出包含 plugin/instance/close-stage/duration 的 critical diagnostic；
- 不强制取消 Plugin 线程，不在未知状态下提前 destroy，也不跳过必须的 ownership release；
- 如果阻塞调用后来迟返，Framework 继续从真实阶段向后推进，并保持 exactly-once destroy/terminal publication；
- 已经公开的 Degraded/restart-required 结论不因迟返自动恢复。

Native `shutdown()` 的 `shutdown_timeout` 是其中唯一传入 Plugin ABI 的 deadline；Call Drain、Destroy、Backing Release 的监督阈值属于 Framework 内部 operational policy，不进入 Public API。

因此即使只有一个 close worker 卡在 Call Drain 或 Destroy、其他 worker 仍可工作，Framework 也必须能够独立识别该 Instance 的异常关闭持续时间，而不能只依赖“所有 worker 饱和”才发现问题。

close executor 的 queue/worker/supervision 细节全部属于内部实现与 Metrics，不形成额外 Public 生命周期状态。

## 5.8 Plugin shutdown 的 Plugin 侧要求

`shutdown()` 必须：

- 对部分初始化 Instance 安全；
- 停止产生新的 Instance-dependent background work；
- 唤醒/停止 Plugin-owned worker、executor、timer、callback source；
- 等待或完成对这些工作的安全 ownership handoff；
- 不销毁仍可能被已准入 Operation 使用的 backing；
- 返回 Ok 后，任何随后继续执行的已准入 Operation 都不得重新创建新的 Instance-dependent background work。

Framework 不规定 Plugin 必须实现一个名叫 “Async Work Admission” 的对象。Plugin 可以根据自身实现使用 atomic flag、mutex/state、executor stop token 等方式满足上述行为。

## 5.9 ServiceRegistry 生命周期

Application 构建并冻结 ServiceRegistry，随后可以复用于多个 Plugin Instance。

`PluginInstanceState` 在 Instance 生命周期内强持有 immutable ServiceRegistry backing，因此 Plugin 的 borrowed Service `Reference` 在合法调用期有效。成功 `create_instance()` 后，Application 可以销毁或移动原始 Public `ServiceRegistry` wrapper，不影响已经创建的 Instance。

关闭成功后：

```
Plugin shutdown() == Ok
→ active_call_count == 0
→ destroy
→ release ServiceRegistry backing
```

Service `Reference` 本身不延长 Registry/Provider lifetime。最后一个 backing owner 释放 owned Provider 时，Provider destructor/custom deleter 必须在 Manager/Instance 控制锁之外执行；Framework 不保证具体销毁线程。

## 5.10 并发加载、初始化监督与常驻 Library

内部 PluginCatalog 和 Resolver 根据：

- plugin_id；
- 可选 exact plugin_version；
- platform/architecture；
- Plugin ABI compatibility；

选择唯一 Artifact。

并发加载同一实际 Artifact 使用 internal single-flight，避免重复 `dlopen()`。

```
resolve catalog entry
→ validate path / digest / file identity
→ loaded_plugins lookup
   ├── already loaded → reuse PluginLibraryState
   └── absent
        → reserve load capacity
        → supervised dlopen(RTLD_NOW | RTLD_LOCAL)
        → validate symbol provenance
        → validate ABI / Entry / Descriptor
        → publish PluginLibraryState only if Manager is still Running
```

`dlopen()` 及其 static initializer 也属于 Lifecycle Supervision：

- 不在 Manager structural lock 内执行；
- operational threshold 耗尽时 Manager 进入 Degraded，设置 `restart_required`；
- Framework 不尝试取消正在执行的 loader/native code；
- shutdown 不得因为一个 hung load 而推迟对已有 Open Instance 的关闭；
- 如果迟返时 Manager 已不是 Running，则不得新发布 PluginLibraryState；可以安全回滚的 transient unpublished handle 按 Loader rollback 规则释放；
- 如果迟返结果表明已执行无法证明安全回滚的 Plugin/loader side effect，保持 Degraded 并要求进程恢复。

成功 publish 后不主动 `dlclose()`。

## 5.11 诊断

Public diagnostics 只冻结稳定、对 Application 有价值的健康信息：

```cpp
enum class PluginManagerPhase {
    Running,
    ShuttingDown,
    Degraded,
    Stopped
};

struct PluginDiagnostics {
    PluginManagerPhase phase;
    bool restart_required;

    std::size_t loaded_plugin_count;
    std::size_t live_instance_count;
    std::size_t closing_instance_count;
    std::size_t unresolved_instance_count;
    std::size_t faulted_plugin_count;

    std::vector<DiagnosticRecord> recent_errors;
};
```

`recent_errors` 是有界 snapshot，不是无界历史容器。Framework 内部使用固定容量 ring / bounded store；容量、单条 message 最大字节数和总诊断字节上限属于 Framework qualification limit，不冻结为 Public API。容量耗尽时按明确的 oldest-first eviction 或等价确定性策略覆盖，并记录 dropped/overwritten counter 到 Metrics。

内部 worker count、queue depth、active call count、refresh/load/create/configure duration 等仍通过 Logging/Metrics 观测，不冻结成 Plugin Framework Public API。

生命周期 Health Monitor 至少记录：

- Refresh Scheduling / Catalog Scan / Manifest Parse duration；
- Load Scheduling / Native Load duration；
- Create Scheduling / Native Create duration；
- Configure duration；
- Pending Close Age；
- 当前 close stage；
- active-call drain duration；
- Destroy duration；
- Backing Release duration；
- busy lifecycle/close workers；
- terminal publication latency。

每条 DiagnosticRecord 至少能够关联：

- plugin_id；
- plugin_version；
- instance_id（适用时）；
- status_code；
- reason；
- bounded message；
- monotonic timestamp。

## 5.12 PluginManager shutdown 与进程退出

Application 正常退出顺序：

```
停止新的业务 Task
→ 停止/Finalize 已有业务 Operation
→ 关闭 Task 持有的 PluginInstance
→ PluginManager::shutdown(deadline)
→ process exit
```

`PluginManager::shutdown()` 是幂等观察接口：

```
Running
→ atomically phase = ShuttingDown
→ reject new refresh/load/create

→ immediately trigger close for all currently Open instances
│
├── concurrently drain Pending / Running close work
│
└── concurrently supervise already-entered refresh/load/create/configure
       ├── late refresh must not publish a Catalog generation
       └── late successful initialization must not publish Open Instance

→ evaluate Normal Shutdown Success Predicate
```

这里的关键约束是：**Manager 不得先串行等待 refresh/load/create/configure 返回，再开始关闭已有健康 Instance。** Existing Open Instance closure 与 in-flight refresh/initialization convergence 必须并行推进。

已经进入的 refresh：

- 允许已经开始的 native filesystem scan / parse 返回；
- Manager 非 Running 时不得替换 Catalog generation；
- hung refresh 达到 operational threshold 后，Manager 进入 Degraded，设置 `restart_required = true`；
- hung refresh 不阻止其他健康 Instance 完成 shutdown/drain/destroy。

已经进入的 load/create/configure：

- 允许真实 native call 继续迟返；
- Manager 不强制取消；
- Manager 已非 Running 时不得 publish 新 Library/Open Instance；
- valid late-created Instance 在 native initialization 返回后进入 Manager-owned Closing/Pending Close；
- hung initialization 达到 operational threshold 后，Manager 进入 Degraded，设置 `restart_required = true`；
- hung initialization 不阻止其他健康 Instance 完成 shutdown/drain/destroy。

Normal Shutdown Success Predicate 仍要求：

```
restart_required == false
AND no Faulted Library requiring process recovery
AND no Unresolved Instance
AND active_refresh_count == 0
AND active_load_count == 0
AND active_create_count == 0
AND no pending/running refresh work
AND no pending/running initialization work
AND no pending/running close work
AND all normal instances == Closed
```

结果语义：

- predicate 成立：停止内部 lifecycle/close execution resources，进入 Stopped，返回 Ok；
- 仅 observer deadline 到期，且尚未形成实际 Degraded 条件：保持 ShuttingDown，返回 DeadlineExceeded，内部收敛继续；
- Faulted / Unresolved / lifecycle operational threshold / close-executor liveness failure：进入 Degraded，`restart_required = true`，返回对应 terminal failure；
- Degraded 后迟返 cleanup 可以继续 exactly-once 收敛，但 authority 不恢复到 Running。

Application 不需要自行维护另一套 refresh/load/create admission，也不需要拼接 Framework 内部 counter 判断是否可以退出。

### 同步生命周期 API 的自等待限制

任何会等待当前 managed Plugin invocation 返回的同步生命周期操作，都不得从该 invocation 的调用链中同步等待自身完成。至少包括：

- 当前 Instance 的 `PluginInstance::close()`；
- 会等待当前 Instance/Operation 收敛的 `PluginManager::shutdown()`。

Operation/callback 只发送非阻塞控制请求或调用 `async_close()`，然后尽快返回；由独立 Application control thread 执行 `CloseFuture::wait()`、同步 `close()` 或 `PluginManager::shutdown()`。

如果 Manager 进入 Degraded，Application/Supervisor 完成必要日志与 Artifact 状态处理后执行受控进程重启。Framework 不在内部直接调用 `std::_Exit()`。

# 6. 插件包、插件目录与加载器设计

本章说明磁盘上的 Plugin Package 如何被发现、选择、验证并加载。PluginCatalog、Resolver 和 LoadKey 都是 Framework 内部实现，不作为 Public API 对象暴露。

## 6.1 Plugin Package 结构

标准布局：

```
my_plugin/
├── manifest.json
└── lib/
    ├── libmy_plugin.so
    └── optional_private_dependency.so
```

规则：

- `manifest.json` 位于 Package root；
- 一个 Package 只有一个主 Plugin DSO；
- private dependency 可以放 package-local `lib/`；
- Manifest 中的相对路径 canonicalize 后必须仍位于 Package root；
- symlink / canonical path 解析不得逃逸 Package root；
- 已发布 Package 不允许原地覆盖正在使用的版本；
- Plugin 可以自行携带说明文档、示例配置或其他私有资源，但这些文件不属于 Plugin Framework 的通用运行时规范，Framework 不解释其内容。

V1 的 Manifest SHA-256 完整性承诺只覆盖 **主 Plugin Artifact**。Package-local private dependency 和其他 private resource 不逐文件写入 Framework Manifest；它们的不可变性由部署/包管理保证的 immutable release directory 负责。Framework 不把 Plugin Package 扩展成通用软件包管理系统。

## 6.2 Manifest

Manifest 由 `rodaq_register_plugin()` 自动生成，插件开发者不手写 Framework metadata。

示例：

```json
{
  "format_version": 1,
  "plugin": {
    "id": "com.example.atomic-counter",
    "name": "Atomic Counter",
    "version": "1.2.3",
    "abi": {
      "major": 1,
      "minor": 0
    }
  },
  "platform": {
    "os": "linux",
    "arch": "amd64"
  },
  "artifact": {
    "path": "lib/libatomic_counter_plugin.so",
    "sha256": "..."
  }
}
```

普通插件开发者只显式维护：

- Plugin ID；
- Plugin Name；
- Plugin Version。

Plugin ABI、platform/arch、artifact path、digest、Manifest format version 由 Tooling 生成。

Loader 必须把 Manifest 视为外部不可信文件输入，并在产生任何 Catalog entry 前执行以下 ingress validation：

| 项目 | V1 规则 |
| --- | --- |
| JSON | UTF-8 严格 JSON；拒绝注释、尾随逗号、NaN/Infinity、重复对象键和超过 `max_manifest_size` 的输入 |
| format version | `format_version` 必须是整数 `1`；缺失、类型错误或其他值 fail closed |
| 字段集合 | V1 只允许示例中的 required 字段和嵌套字段；未知字段拒绝，未来格式通过新 `format_version` 定义 |
| plugin metadata | `plugin.id/name/version` 遵循 §3.7 Descriptor ingress 规则；`abi.major/minor` 是无符号 16-bit 整数 |
| platform | `os` 必须是 `linux`；`arch` 必须是 `amd64` 或 `arm64` |
| artifact path | 非空相对路径，不得为绝对路径、包含 `..` 逃逸、指向 remote URL，或 canonicalize 后离开 Package root |
| artifact digest | `sha256` 必须是 64 个 lowercase hexadecimal 字符，并与最终 DSO 内容匹配 |

Manifest 解析、字段验证、Descriptor cross-check 任一步失败都必须拒绝该 Package，不能通过未知字段、默认值或宽松类型转换猜测未来格式。

Manifest 中的 Plugin ABI Major/Minor 与 `rodaq_plugin_abi_version()` 表示同一个版本事实：

- Manifest 用于 `dlopen()` 前快速预筛选；
- DSO 加载后必须与 symbol 返回值完全一致；
- Runtime 兼容条件为 Major 相同且 Runtime Minor 不低于 Plugin Minor。

Manifest format version 是 Tooling 文件格式版本，不属于第 1.5 节的 Plugin Runtime 版本模型。

## 6.3 Plugin Configuration 所有权

Plugin Configuration 是 Plugin 私有输入，不属于 Plugin Framework 的可解释元数据。

职责划分：

```
Application / Task
    ↓ 提供 UTF-8 JSON configuration
Plugin Framework
    ↓ 只做 bounded transport
Plugin::configure()
    ↓ parse / validate / apply
Plugin-owned state
```

Framework 只负责：

- 限制配置文本最大长度；
- 验证通用 pointer/length/String contract；
- 在 `configure()` 调用期间提供稳定的 configuration view；
- 不修改、不迁移、不补默认值。

Plugin 负责：

- JSON syntax parse；
- 字段类型和必填项；
- 默认值；
- 跨字段约束；
- ROS 2 QoS、MQTT endpoint、MCAP/SQLite3 等 Plugin-specific 语义；
- 文件、网络、设备等运行环境检查；
- 无效配置时返回 `InvalidArgument` 和 Plugin-owned diagnostic message。

因此 V1 的 Plugin Framework 不提供配置 Schema、通用配置 validator、字段级结构化配置错误类型或对应 CMake/Manifest 元数据。

如果未来 Web UI 需要自动配置表单，应在具体业务配置层或独立可选的 Plugin Metadata Interface 中设计，不应把 UI Schema 校验职责耦合进 Plugin Foundation。

## 6.4 PluginCatalog 与资源边界

`PluginCatalog` 是 Framework 内部的已发现插件索引，不是 Public API 对象。

Catalog entry 保存：

```
plugin_id
plugin_name
plugin_version
Plugin ABI requirement
platform / architecture
artifact path / sha256
package root
```

Discovery：

```
configured search paths
→ bounded directory traversal
→ enumerate manifest.json
→ bounded manifest read / parse
→ path containment + symlink escape rejection
→ platform / ABI prefilter
→ artifact digest verification
→ build new Catalog generation
→ atomic publish
```

所谓 `bounded` 必须由可执行的 Framework hard limit / qualification limit 定义，而不能只作为文字要求。至少覆盖：

```
max_plugin_roots
max_package_entries
max_manifest_size
max_package_files
max_artifact_size
max_scan_bytes
max_path_length
max_catalog_diagnostics
max_catalog_diagnostic_bytes
```

这些限制属于 Loader/Catalog 的防御性实现边界，V1 不全部暴露到 `PluginManagerOptions`。只有确实需要 Application 策略控制的 `max_loaded_plugins`、`max_instances`、`max_config_size` 保持 Public option；其余值由 Framework qualification 和部署基线冻结。

达到扫描/解析资源上限时：

- 不继续无界扫描或分配；
- 对受影响 Package/refresh 返回 ResourceExhausted 或记录 bounded diagnostic，具体失败粒度由测试冻结；
- 不污染已经发布的旧 Catalog generation；
- `recent_errors` / Catalog diagnostics 都必须有界。

单个无效 Package 记录诊断，不默认使整个 `refresh()` 失败；但是全局资源上限耗尽、Catalog 无法形成一致 generation 或 internal invariant failure 可以使 `refresh()` 整体失败。

Application 通过：

```cpp
PluginManager::plugins()
```

获得一个一致 `PluginInfo` snapshot，不直接访问 CatalogEntry。

通用 Plugin Framework 不提供 Acquisition/Storage 等业务分类元数据。业务 UI 需要分类时，由 RoDAQ Application 的业务配置或对应 Interface 设计提供。

## 6.5 内部版本选择

Resolver 仍可以作为内部模块存在，但不进入 Public API。

`load(plugin_id)`：

1. exact plugin_id；
2. platform/arch compatible；
3. Plugin ABI compatible；
4. 排除所有 SemVer prerelease candidate；
5. 在剩余 Stable Release 中按 SemVer precedence 选择最高 Plugin Version；
6. 如果最高 precedence 上存在多个不同完整 Version Identity（例如 `1.2.3+build.7` 与 `1.2.3+build.8`），返回 Conflict，不按搜索路径、字典序或发现顺序任意选择；
7. 如果 Plugin ID 存在但只有 prerelease candidate，则返回 Unavailable，并要求调用方使用 exact-version overload 显式选择。

`load(plugin_id, plugin_version)`：

1. exact plugin_id；
2. 对完整 SemVer identity 做 exact string match；
3. platform/arch compatible；
4. Plugin ABI compatible。

同一个 Plugin ID + 完整 Version Identity 出现多个不同 Artifact 时返回 Conflict。Artifact digest 不参与 SemVer precedence，只用于安装内容验证、去重和审计。

Public 加载接口只保留 `load(plugin_id)` 与 `load(plugin_id, plugin_version)` 两条路径；Catalog entry 和 Resolver selection 都是 Framework 内部实现。正式生产 Task 已要求 exact Plugin Version，因此 prerelease 只通过 exact-version overload 显式启用，不影响可复现性。

## 6.6 Task 可复现性与 Artifact 审计

正式 Task 配置只需要固定：

```
plugin_id
plugin_version
plugin_config
```

其中 `plugin_version` 必须是 exact Plugin Version。

Artifact SHA-256 属于部署验证和运行审计信息，不要求普通 Task 配置复制一份。

正式数据采集任务的运行记录 / Session metadata 必须自动记录实际使用的 Plugin Artifact：

```
plugin_id
plugin_version
artifact_sha256
```

Application/Task 作者不手工填写 digest。Core 只能通过公开只读接口取得实际 Artifact 身份：

```cpp
const PluginArtifactInfo& PluginLibrary::artifact() const noexcept;
```

或者由持有该 `PluginLibrary` backing 的 Framework 集成层把同一 `PluginArtifactInfo` 传入 Session metadata writer。Core 不得访问内部 `PluginLibraryState`。

该信息用于长期数据审计、故障复现以及确认“同一 Plugin ID/Version 实际运行的是哪个主 Plugin Artifact”。

这样区分：

- **Task 配置**：声明希望运行哪个 Plugin 版本；
- **部署/Loader**：证明主 Plugin Artifact 未被替换，并依赖 immutable release directory 保护 package-local dependency；
- **Session metadata**：记录实际运行的 Plugin ID / Version / 主 Artifact SHA-256。

## 6.7 Loader Pipeline 与符号来源验证

```
internal CatalogEntry
→ canonicalize Package / Artifact path
→ reject path/symlink escape
→ validate main Artifact SHA-256
→ capture pre-load file identity
→ internal single-flight
→ loaded_plugins lookup
   ├── existing same artifact
   │    → reuse PluginLibraryState
   └── absent
        → reserve max_loaded_plugins
        → supervised dlopen(RTLD_NOW | RTLD_LOCAL)
        → resolve rodaq_plugin_abi_version
        → resolve rodaq_plugin_get_entry
        → validate symbol provenance
        → validate ABI / Entry / Descriptor
        → Manifest / Descriptor cross-check
        → verify loaded-object identity
        → atomically publish PluginLibraryState only while Manager == Running
```

Bootstrap symbol provenance 是 V1 Loader contract 的一部分。对：

```
rodaq_plugin_abi_version
rodaq_plugin_get_entry
```

解析出的地址必须使用平台可用的 ELF provenance 能力（例如 `dladdr()` 或等价机制）确认其 defining object 就是已经验证的主 Plugin Artifact，而不是 dependency 中的同名 symbol。无法证明 provenance 时 fail closed。

加载后的 loaded-object canonical identity / file identity 必须与 pre-load 验证的 Artifact 一致。V1 不声称单靠 path-based `dlopen()` 在 hostile writable directory 中消除所有 TOCTOU；其安全前提是 §6.8 的 immutable release directory。

失败回滚：

| 阶段 | 处理 |
| --- | --- |
| pre-dlopen | 释放内部 load reservation |
| dlopen failure | 返回 loader error |
| provenance / symbol failure | close transient unpublished handle |
| ABI mismatch | close transient unpublished handle |
| Descriptor mismatch | close transient unpublished handle |
| Manager 已进入 ShuttingDown/Degraded | 不发布新 Library；安全释放 transient unpublished handle |

Publish race lost：复用 winner，并释放 loser 的 transient native handle/reference。

成功 publish 后：

- Native DSO 不再由 Framework 主动 `dlclose()`；
- Public `PluginLibrary` 析构不改变 DSO 常驻状态；
- 不存在 UnloadRequested / Unloading / Mapping Generation；
- 不支持 in-place hot replacement。

## 6.8 部署、不可变目录与依赖边界

V1 生产部署要求：

- Plugin search path 显式配置；
- Package release directory 在整个进程运行期间对普通运行用户不可写并保持内容不可变；
- 已验证主 Artifact 与其 package-local dependency 不允许原地替换；
- 升级使用新的 Plugin Version / Release Directory，并通过进程重启生效；
- 主 Artifact SHA-256 用于内容完整性检查，不表示发布者授权；
- Package-local private dependency 的完整性由 immutable release directory / 外部包管理保证，而不是由 Framework Manifest 逐文件签名；
- Publisher trust 由部署/包管理流程保证；
- 不可信 DSO 的进程内沙箱不属于 V1。

Loader 的 path/digest/file-identity 模型明确依赖这一前提。Qualification 必须包含“验证后、dlopen 前尝试替换主 Artifact / dependency”的误操作与攻击模拟：可写 release directory 必须被部署检查拒绝，或 Loader 必须 fail closed；不得把 writable production directory 当作受支持部署模式。

`RTLD_LOCAL` 限制新符号进入全局作用域，但不是独立 dynamic-link namespace。ROS 2、MQTT、MCAP、SQLite3 等插件仍必须执行同进程依赖兼容资格验证。

DSO static initializer/destructor：

- 不得启动长期线程、callback source、网络连接；
- 不得依赖 Application Service / Plugin config；
- 不得把可观察业务初始化放入 static initializer；
- 长期资源只能在受管 Plugin Instance `configure()` 之后创建。

这样 Loader 验证失败或未发布 handle 回滚时，不需要执行 Plugin Instance lifecycle cleanup。

## 6.9 Catalog / Loader 阻塞与健康监督

`PluginManager::load()` 是 control-plane API，不得从 realtime/data-plane thread 调用。

V1 不强制取消卡死在 dynamic loader / DSO static constructor 内的 native `dlopen()`。但是 `load()` 必须纳入 §5.10 的 Lifecycle Supervision，而不能只记录 duration：

- load work 使用有界 lifecycle execution capacity；
- 独立 health monitor 在 worker 卡死时仍能推进；
- operational threshold 耗尽后，等待方返回 `DeadlineExceeded`，Manager → Degraded，`restart_required = true`；
- hung load 不阻止 Manager 同时关闭其他已经 Open 的健康 Instance；
- native load 如果以后迟返，只执行安全的 validation/rollback，不在 Degraded/ShuttingDown Manager 中发布新的 Library；
- 不因此新增 Public LoadTicket/LoadFuture，也不为 `load()` 增加每次调用的 Public Deadline 参数。

`PluginManager::refresh()` 同样是 control-plane API，并受 §5.4 的 Catalog refresh 生命周期监督：

- 文件系统扫描、Manifest 读取/解析或 Catalog build 卡死时，等待方最终得到 `DeadlineExceeded`；
- Manager 进入 Degraded/restart_required，且不阻塞其他健康 Instance 关闭；
- 迟返 refresh 只能执行验证、清理和丢弃操作，不得在 Manager 非 Running 时替换 Catalog；
- 并发 refresh 共享同一个 single-flight 流程，不能通过重复调用创建并行无界扫描。

## 6.10 CMake 与安装

Backend：

```
find_package(RoDAQ REQUIRED)

add_library(my_plugin SHARED
    src/my_plugin.cpp
)

target_link_libraries(my_plugin
    PRIVATE
        my_private_dependencies
)

rodaq_register_plugin(my_plugin
    PLUGIN_ID com.example.my_plugin
    PLUGIN_NAME "Example Plugin"
)
```

`rodaq_register_plugin()` 的第一个参数必须是已经存在的 CMake target。它不创建 target、不接受 `SOURCES`，只负责把现有 shared-library target 注册为 RoDAQ Plugin，并附加 Plugin authoring、Descriptor/Manifest、visibility、package 与 install 规则。

Plugin Version 解析规则：

```
if PLUGIN_VERSION is specified
    validate and use full SemVer string
else if PROJECT_VERSION is available
    use PROJECT_VERSION
else
    CMake configuration error
```

需要 prerelease/build metadata 时显式声明：

```
rodaq_register_plugin(my_plugin
    PLUGIN_ID com.example.my_plugin
    PLUGIN_NAME "Example Plugin"
    PLUGIN_VERSION "1.2.3-rc.1"
)
```

`PLUGIN_VERSION` 与 `PROJECT_VERSION` 不形成两个独立版本域；Tooling 只产生一个最终 Plugin Version，并同时写入 Descriptor 与 Manifest。

Plugin 私有配置不通过 `rodaq_register_plugin()` 声明结构或校验规则。插件如果希望随包提供示例 JSON、说明文档或 UI 资源，可以作为普通 private/package resource 安装，但 Plugin Framework 不解释这些文件。

Tooling 自动生成：

- Descriptor metadata；
- Plugin ABI Major/Minor；
- Manifest；
- artifact SHA-256；
- platform / architecture；
- install layout。

安装后必须支持：

```
find_package(RoDAQ REQUIRED)
target_link_libraries(app PRIVATE rodaq::plugin)
```

以及独立 Plugin 工程使用 `rodaq_register_plugin()`，不依赖源码树中的 private target。

# 7. 生命周期与并发安全

本章是 Plugin Instance 生命周期安全的权威说明。V1 生命周期只使用 Initializing、Open、Closing、Closed、Unresolved 五个状态。

## 7.1 Plugin Instance 状态机

```
Initializing
    │ configure success
    ▼
   Open
    │ close / destructor / manager shutdown
    ▼
 Closing
   ├── shutdown + drain + destroy success → Closed
   └── shutdown cannot be proven safe      → Unresolved
```

规则：

- `Initializing` 只存在于 Framework 内部，不发布给 Application；
- `Open` 是唯一允许新的 Query / Operation 调用进入的状态；
- `Closing` 拒绝新的 Query / Operation；
- `Closed` 表示 raw Plugin Instance 已 destroy，Instance-owned ServiceRegistry/backing 已释放；
- `Unresolved` 表示 Plugin `shutdown()` 已经真实返回 non-OK，或 Framework 已获得等价的终态证据表明该 Instance 不能安全 destroy；必须保留到进程退出并要求受控重启；
- Native `shutdown()` 仍在执行时，即使已经超过 policy deadline，Instance 仍保持 `Closing`；此时 Manager 可以先进入 Degraded/restart_required，但不得把尚在执行的 shutdown 伪装成 terminal Unresolved。

## 7.2 初始化与 Manager shutdown

`create_instance()` 只在 PluginManagerState == Running 时进入。

Manager shutdown 与 create 的线性化：

```
create_instance()
→ under PluginManagerState lock:
     require Running
     active_create_count++

refresh()
→ under PluginManagerState lock:
     require Running
     enter/join internal refresh single-flight
     only the single-flight creator increments active_refresh_count

PluginManager::shutdown()
→ under same lock:
     Running → ShuttingDown
     reject later refresh/load/create

→ immediately request closure of existing Open Instances
→ concurrently supervise active refresh / load / create / configure
→ late refresh must not publish Catalog
→ late initialization must not publish Open Instance
```

因此 refresh/load/create 与 Manager shutdown 的准入完全由 PluginManagerState 统一线性化。

如果 create 已经进入 Plugin ABI，Manager shutdown 监督它真实返回；Framework 不使用线程取消，也不会因此延迟关闭其他健康 Instance。

进入 Plugin ABI create 前，Framework 必须已经完成 Instance capacity、`PluginInstanceState` 和无丢失关闭提交所需控制资源的预留，并把该 State 纳入 Manager 生命周期域。

如果 configure 失败：

```
preserve original configure Status
→ phase = Closing
→ publish Manager-owned Pending Close Work
→ only then return original Status to Application
```

因此失败返回不会让部分初始化 Instance 在 cleanup 接管前失去 owner。

如果 create/configure 最终成功但在 Open Commit Point 重新检查时 Manager 已进入 ShuttingDown，则不得发布 Open Instance：

```
configure success
→ Open Commit Point observes manager != Running
→ phase = Closing
→ publish Pending Close Work
→ active_create_count--
→ create_instance() returns InvalidState
```

Manager shutdown 等待该 Instance 完成真实关闭或进入明确的 Degraded 路径。

## 7.3 Plugin shutdown 规则

`PluginBase::shutdown()` / Foundation `shutdown()` 只回答：

> Plugin Instance 是否已经停止所有依赖该 Instance 的后台活动，并进入可以安全 destroy 的状态。
>

它必须：

- 对部分初始化 Instance 安全；
- 对尚未创建的资源允许 no-op；
- 停止新的 Instance-dependent background work；
- 唤醒并停止已有 thread、executor、timer、callback/source；
- 等待这些工作结束，或在返回 Ok 前完成与 Instance 无关的 ownership handoff；
- 不销毁仍可能被已准入 Operation 使用的 backing；
- 返回 Ok 后，即使关闭前已经准入的 Operation 继续执行，也不得再次创建新的 Instance-dependent background work。

Plugin 可以使用 atomic flag、mutex/state、stop token、executor state 等任意方式实现，不要求存在统一名为“Async Work Admission”的对象。

ROS 2 Executor、MQTT callback/reconnect worker、MCAP/SQLite background writer 等都属于 Plugin shutdown 需要收敛的资源。

### 业务 Stop / Finalize 与 Plugin shutdown 分离

Plugin shutdown 是生命周期操作，不承诺业务完成成功。

例如：

```
StorageInterface::finalize()
→ DiskFull / DataLoss

PluginBase::shutdown()
→ stop writer worker
→ release callback/backing
→ Ok
```

两者可以同时成立。

因此：

- Acquisition Stop 属于 Acquisition Interface；
- Storage Finalize / Persistence Completion 属于 Storage Interface；
- Plugin shutdown 只负责实例可安全 destroy。

## 7.4 统一调用保护

所有 Plugin Instance-dependent ABI 调用使用统一：

```
phase == Open
active_call_count
CallGuard
```

Query 与 Operation 使用同一个 `active_call_count`。

Query 返回 Operation Handle 时，完整 Managed Handle 必须在当前 CallGuard 释放之前构造完成：

```
acquire CallGuard
→ query()
→ validate InterfaceReference
→ attach strong PluginInstanceState
→ construct complete OperationHandle
→ release CallGuard
```

CallGuard 释放后，Handle 构造逻辑不得再访问可能随 close 被销毁的 Provider metadata、vtable backing 或其他 Instance-dependent backing。

准入必须满足：

```
lock instance state
if phase != Open:
    reject InvalidState
active_call_count++
unlock
```

返回时：

```
active_call_count--
notify close waiter if reaches zero
```

关闭线程必须先把 state 从 Open 切换到 Closing，再等待 active_call_count 清零。

这保证：

- 已经准入的调用可以安全返回；
- Closing 之后不会有新调用进入；
- destroy 不会与 Query/Operation 并发；
- Query 构造 Handle 的过程中不会因为并发 close 提前释放依赖 backing。

如果 ManagerState 与 InstanceState 必须嵌套持锁，统一顺序为 `PluginManagerState → PluginInstanceState`。Plugin ABI、Application/Plugin callback、Provider destructor、custom deleter、外部 I/O 或任何可能重入 Framework 的代码都不得在这些控制锁内执行。需要释放对象时先在锁内 detach ownership/commit state，再在锁外析构。

## 7.5 同步与异步关闭

### async_close()

首次：

```
Open
→ atomically Closing
→ atomically publish Pending Close Work
→ signal AsyncCloseExecutor
→ return CloseFuture(shared PluginInstanceState)
```

`Closing` 必须始终能够从 Manager 的 Pending Close Work Set 中被重新发现。一次 queue insertion 失败不得丢失关闭责任；worker 完成、queue capacity 恢复和 Manager shutdown 都必须能够重新推进 pending close。同一 Instance 同时最多一个真实 close task 运行。

重复调用：

- Closing → 返回同一个 State 的 CloseFuture；
- Closed → 返回已经 ready 的 CloseFuture；
- Unresolved → 返回已经 terminal-failure 的 CloseFuture。

### close()

同步 `close(observer_deadline)` 使用 `CloseFuture`：

```
future = async_close()
wait = future.wait(observer_deadline)

TimedOut  → return DeadlineExceeded
Completed → return future.result()
```

`close().ok()` 表示 Instance 已进入 Closed。

`CloseFuture::wait()` 只报告 Observer Wait 是否完成；真实关闭终态由 `CloseFuture::result()` 返回。因此 Application 能无歧义地区分：

- Observer Timeout；
- Native shutdown terminal failure（包括真实 `DeadlineExceeded`）；
- Close success。

### callback/Operation 中关闭当前 Instance

当前 Instance 的 Operation/callback 内允许：

```cpp
instance.async_close();
return;
```

不允许形成：

```
Operation holds active_call_count
→ same thread calls close() and waits
→ close waits active_call_count == 0
```

的自等待。

Debug/qualification 实现可以通过 thread-local marker 检测直接 self-wait 并返回 InvalidState；该 marker 是实现辅助，不是架构对象。

同一规则扩展到 `PluginManager::shutdown()`：如果当前 managed Plugin invocation 的返回是 Manager shutdown 完成条件的一部分，则该调用链不得同步等待 Manager shutdown。Operation/callback 应只发出非阻塞控制请求并返回，由独立 Application control thread 执行真正的同步 shutdown。

## 7.6 实际关闭流程

```
phase = Closing
→ Framework 根据 PluginManagerOptions::shutdown_timeout
   计算 native shutdown absolute deadline
→ call Plugin shutdown(native_deadline)
→ wait actual shutdown return
→ if shutdown != Ok
     phase = Unresolved
     manager = Degraded
     restart_required = true
     stop
→ wait active_call_count == 0
→ destroy exactly once
→ detach Instance-owned ServiceRegistry / backing under control lock
→ release detached backing outside control locks
→ commit Manager instance removal / accounting
→ release max_instances capacity
→ close_result = Ok
→ phase = Closed
→ notify CloseFuture observers
```

destroy gate：

```
SafeToDestroy =
    shutdown actually returned Ok
    AND active_call_count == 0
```

`SafeToDestroy` 是关闭流程中的瞬时判定条件，不形成额外生命周期状态。

`Closed` / terminal Close Ok 是强成功边界，只能在 Instance-owned backing 已完成安全释放、Manager active ownership 已移除且 `max_instances` 容量已经归还之后发布。资源析构如果可能重入 Framework，则采用“锁内 detach + accounting commit、锁外 destructor/deleter、最终 publish terminal result”的两阶段流程；不得为了避免锁外析构而提前向 Application 发布关闭成功。

## 7.7 AsyncCloseExecutor

异步关闭执行器只负责避免：

- `PluginInstance` destructor 阻塞；
- Application 为每个 close 建线程；
- 一个普通同步调用线程直接卡在 Plugin shutdown。

要求：

- worker 数有界；
- pending close 总量受 `max_instances` 自然限制；
- Closing Instance 同时属于 Manager 可发现的 Pending Close Work Set；
- queue 满或一次调度失败不得丢失 close work；
- Pending Work 具有有界公平性，并在 worker 完成/容量恢复/Manager shutdown 时自动重试；
- 同一 Instance 最多一个真实 close task；
- `async_close() noexcept` 的提交路径不得因为临时 heap allocation failure 丢失关闭责任；
- 不无界扩线程；
- 不使用 pthread cancellation / forced thread termination；
- Manager 持有 Closing/Unresolved Instance，所以 Public wrapper 释放不会产生无主对象。

Framework 必须使用不依赖被阻塞 worker 的内部 health monitor 监督整个关闭链路，而不只监督 Native Shutdown。至少覆盖：

```
Pending Scheduling
→ Native Shutdown
→ Call Drain
→ Destroy
→ Backing Release
→ Terminal Publication
```

如果 Native Shutdown、Call Drain、Destroy、Backing Release 或 Pending Scheduling 任一阶段超过对应 operational threshold：

```
PluginManagerPhase = Degraded
restart_required = true
emit critical diagnostic(stage, duration)
```

不得强制取消 worker，也不得因为 watchdog 超时跳过真实资源状态。若阻塞调用迟返，继续按真实阶段向后推进，并保持 shutdown/destroy/terminal publication 的 exactly-once 语义。

如果所有 close worker 都无法继续提供有效关闭能力：

```
PluginManagerState.phase = Degraded
restart_required = true
reject new create_instance()
emit critical diagnostic
```

具体 worker count、queue implementation、saturation timer 属于内部实现与 Metrics，不冻结为 Public API 或新的生命周期状态。

## 7.8 所有权矩阵

| 对象/资源 | Open | Closing | Closed | Unresolved |
| --- | --- | --- | --- | --- |
| PluginLibraryState / DSO | Manager 持有，Resident | Resident | Resident | Resident |
| Raw Plugin Instance | InstanceState 持有 | InstanceState 持有 | 已 destroy / null | 保留，禁止 unsafe destroy |
| ServiceRegistry backing | InstanceState 持有 | 保留 | 已释放 | 继续保留 |
| PluginInstance wrapper | 可存在 | 可释放 | stale/closed | stale/unresolved |
| Operation Handle | 可调用 | 新调用 InvalidState | InvalidState | InvalidState |
| CloseFuture | 可创建 | 等待同一 State；native shutdown 在途时即使 Manager 已 Degraded 仍未 ready | ready success | ready terminal failure |
| PluginManagerState ownership | 持有 active Instance | 必须持有 | 从 active set 移除 | 持有到进程退出 |

成功加载 DSO 常驻不等于 Instance backing 可以常驻。Instance 业务资源仍必须尽可能通过 shutdown/destroy 释放。

## 7.9 并发结果矩阵

| 并发场景 | 确定结果 |
| --- | --- |
| create_instance × manager shutdown | Create 在 Open Commit Point 重新检查 Manager；Shutdown 已获胜时不发布 Open Instance，转入 Pending Close 并返回 InvalidState |
| Query × close | Query 先取得 CallGuard 则完成；close 先切换 Closing 则 Query 返回 InvalidState |
| Operation × close | 已准入 Operation 完成；新 Operation 被拒绝 |
| Operation × Plugin shutdown | 已准入 Operation 可以继续，但 Plugin 不得在 shutdown 安全点之后创建新的 Instance-dependent background work |
| async_close × async_close | 只有一次真实 shutdown/destroy；多个 Future 观察同一 InstanceState |
| close wait timeout × later wait | later wait 继续观察同一真实关闭流程 |
| PluginInstance destructor × Open | destructor 触发/复用 async_close，不同步等待 |
| manager shutdown × async_close | 都复用同一 Instance Closing 状态 |
| refresh × refresh | 后到调用加入/观察同一 single-flight；只有一个 Catalog generation build |
| refresh × shutdown | Shutdown 立即关闭 Open Instance；late refresh 不发布 Catalog；hung refresh 可进入 Degraded |
| close worker permanent block | 独立 health monitor 能检测；Manager 进入 Degraded；不强制取消、不 unsafe destroy |
| Faulted Library × manager shutdown | restart_required 阻止 Stopped/Ok；Manager 保持 Degraded |
| Manager shutdown × managed callback | 不允许同步自等待；由独立 control thread 执行 shutdown |

## 7.10 失败处理

| Failure | 处理 |
| --- | --- |
| Plugin constructor exception | 不发布 Instance；释放容量 |
| create normal failure | 返回原始 Status |
| create ABI violation | Library → Faulted；restart_required；禁止后续 create |
| configure failure | 返回原始 Status；Framework 异步 shutdown 部分初始化状态 |
| Plugin configuration invalid | Plugin `configure()` 返回 InvalidArgument；Framework 保留原始 Status 并按初始化失败路径安全清理 |
| close observer timeout | DeadlineExceeded；真实 close 继续 |
| Plugin shutdown non-OK | Instance → Unresolved；Manager → Degraded |
| Plugin shutdown 永久不返回 | 保留 Instance；close worker 被占用；Manager 可能 Degraded |
| Catalog refresh 永久不返回 | 保持 refresh backing；Manager → Degraded/restart_required；其他 Open Instance 仍完成关闭 |
| destructor violates noexcept | 可能 std::terminate；通过编码/测试降低风险 |
| max_instances exhausted | Plugin ABI create 前 ResourceExhausted |
| max_loaded_plugins exhausted | 新 dlopen 前 ResourceExhausted |
| loader validation failure | 回滚未发布 transient handle |
| Service Provider code lifetime 非 process-resident | unsupported integration；资格验证必须发现 |

## 7.11 Destroy 与析构

Foundation `destroy()`：

- 只在 `shutdown() == Ok` 且 `active_call_count == 0` 后调用；
- 最多一次；
- 不得与其他 Instance ABI call 并发；
- Plugin destructor 必须 no-throw；
- 返回后 raw context 不再可访问；
- destroy 不返回 Status，不重试。

## 7.12 延迟工作

如果某项 deferred work 必须在 Instance destroy 后继续：

```
shutdown() == Ok
⇒ deferred work no longer depends on PluginInstanceState
∧ no borrowed Service Reference remains
    unless the Service Interface defines explicit ownership handoff
∧ callback Context/Backing have independent owners
∧ executable code remains valid
```

因为 Plugin DSO Resident，Plugin code mapping 在进程退出前有效；但 context、buffer、Service Provider backing 仍必须单独保证生命周期。

## 7.13 常驻 Plugin Library

成功发布的 Plugin DSO：

```
Loaded
→ Resident
→ process exit
```

规则：

- Public PluginLibrary 析构不触发 `dlclose()`；
- 失败且尚未 publish 的 transient handle 可以正常回滚；
- Plugin 升级通过新的 Plugin Version + 进程重启；
- 不实现 hot reload / Mapping Generation；
- `max_loaded_plugins` 限制 Framework 管理的常驻 Plugin 数量；
- RSS、FD、native thread 和第三方库 cache 属于进程资源监督，不由 `max_loaded_plugins` 单独给出数学上界。

# 8. 验证、交付与实施门禁

本章不再定义新的架构，而是把前面各章的设计规则转换为可执行证据。阅读时可以把每个 PF-* 编号理解成一条“必须通过的验收用例”；正文负责说明设计，Qualification 负责证明实现确实遵守设计。

## 8.1 验证层级

| 层级 | 目标 |
| --- | --- |
| 单元测试 | Registry、Catalog、版本选择、状态转换、错误路径 |
| ABI 验证 | C Header、struct size、required prefix、旧二进制兼容 |
| SDK 验证 | PluginBase、PluginSetup、Provider、trampoline、异常转换 |
| Runtime 验证 | load / create / query / close / shutdown |
| 并发验证 | create/close/query/operation/shutdown 的线性化 |
| 故障注入 | create ABI violation、shutdown hang、Loader failure、DiskFull |
| 独立工程 | 安装后独立构建，不依赖源码树 private target |
| 性能验证 | Operation hot path、资源上限、写入链路 |
| 长期运行验证 | DSO 常驻、重复 create/close、异常恢复、资源稳定性 |

每个关键测试至少说明：

```
Test ID
Safety Property
Precondition
Action / Interleaving
Expected Result
Expected Resource State
```

Qualification 验证可观察行为，不要求某个内部 helper/class 必须存在。

## 8.2 Runtime / Lifecycle 资格验证

| ID | 场景 | 必须证明 |
| --- | --- | --- |
| PF-RT-001 | create success | Application 只得到 Open Instance |
| PF-RT-002 | configure failure cleanup | 返回原始 configure Status 前，Manager 已取得 Closing/Pending cleanup ownership；业务资源最终安全收敛 |
| PF-RT-003 | Query × close | Managed Handle 在 CallGuard 内完整构造；已准入 Query 完成；后续 Query InvalidState |
| PF-RT-004 | Operation × close | 已准入 Operation 完成；新调用拒绝；destroy 等待 active call |
| PF-RT-005 | repeated async_close | 只有一次真实 shutdown/destroy |
| PF-RT-006 | close deadline source semantics | `async_close()` 的 wait/result 能无歧义区分 Observer Timeout 与真实 terminal DeadlineExceeded；同步 `close()` 返回 DeadlineExceeded 时允许两种来源，调用方不得仅凭 StatusCode 推断来源 |
| PF-RT-007 | destructor × Open | destructor 不阻塞；触发/复用异步关闭 |
| PF-RT-008 | same-instance blocking self-wait | callback/Operation 中同步 `close()` 或 `CloseFuture::wait()` 等待自身关闭时 fail-fast/不形成死锁 |
| PF-RT-009 | manager shutdown self-wait | managed callback 中不得同步等待包含自身返回条件的 Manager shutdown |
| PF-RT-010 | Plugin shutdown terminal failure | Plugin `shutdown()` 已真实返回 non-OK 时，Instance → Unresolved；Manager → Degraded；不 unsafe destroy |
| PF-RT-011 | whole-close-pipeline supervision | 独立 health monitor 覆盖 Pending Scheduling / Native Shutdown / Call Drain / Destroy / Backing Release / Terminal Publication；任一阶段异常持续都可诊断并进入 Degraded/restart_required，且不强制取消/unsafe destroy |
| PF-RT-012 | late native shutdown return | timeout 后迟返只继续完成 drain/destroy once，不重复 Destroy，也不自动撤销 restart_required |
| PF-RT-013 | pending close queue pressure | queue full/临时调度失败不丢 close work；容量恢复后自动推进 |
| PF-RT-014 | pending close fairness | 多 Closing Instance 最终都能被调度；同一 Instance 同时最多一个真实 close task |
| PF-RT-015 | async_close noexcept takeover | 进入 Plugin ABI create 前已预留关闭控制资源；queue insert failure、临时 allocation failure、worker pressure 下仍不丢 Pending Close、不产生无主 Instance，并可自动重试推进 |
| PF-RT-016 | manager shutdown × create | Open Commit Point 重新检查 Manager；Shutdown 获胜时不发布 Open Instance，转入 Pending Close 并返回 InvalidState |
| PF-RT-017 | manager shutdown success | Success Predicate 全部满足后才进入 Stopped/Ok |
| PF-RT-018 | Faulted Library × shutdown | Faulted + restart_required 时即使没有正常 Instance，也不得返回 Ok |
| PF-RT-019 | manager observer timeout | 仅 Observer Deadline 到期且关闭仍正常推进时，返回 DeadlineExceeded、保持 ShuttingDown、不自动设置 restart_required |
| PF-RT-020 | manager actual shutdown failure | Native Shutdown Policy Timeout、Faulted、Unresolved 或已确认 executor liveness failure → Degraded + restart_required |
| PF-RT-021 | ManagerState / InstanceState lock order | 嵌套锁遵循统一顺序；Plugin ABI/callback/destructor/custom deleter 在控制锁外执行 |
| PF-RT-022 | close success publication | CloseFuture 只能在 backing 释放、Manager ownership/accounting 提交和 max_instances 容量归还后观察到 Completed/Ok |
| PF-RT-023 | CloseFuture result before ready | result() 在 terminal publication 前返回 InvalidState；ready/wait/result 观察同一发布点 |
| PF-RT-024 | repeated create/close | live instance/resource count 回到基线；close().ok() 后紧随其后的 create 不因旧 Instance 占用 capacity 而失败 |
| PF-RT-025 | max_instances | 在进入 Plugin create 前确定 ResourceExhausted |
| PF-RT-026 | configure failure capacity | configure failure 返回原始错误时 cleanup 可以仍在 Closing；capacity 在安全 cleanup 完成前不归还，立即重试允许暂时 ResourceExhausted |
| PF-RT-027 | PluginManager lifecycle | Manager move/destructor/repeated shutdown/Stopped/Degraded 行为与 process-lifetime single authority 规则一致；析构不阻塞、不伪装 shutdown success |
| PF-RT-028 | hung create | Native create 超过 operational threshold 后等待方得到 DeadlineExceeded；Manager → Degraded/restart_required；不取消 native call、不丢失 backing |
| PF-RT-029 | hung configure | configure 卡死不发布 Open Instance；Manager → Degraded；其他健康 Open Instance 仍可完成关闭 |
| PF-RT-030 | manager shutdown × hung create/configure | shutdown 立即关闭已有 Open Instance，同时监督 in-flight initialization；不得先串行等待 active_create_count 归零 |
| PF-RT-031 | late initialization return | Manager 已 ShuttingDown/Degraded 时，迟返初始化不得 publish Open Instance；valid Instance 进入 exactly-once cleanup |
| PF-RT-032 | refresh × load × shutdown | refresh 原子发布 Catalog generation；load 观察完整 generation；shutdown 后无新 Library/Instance publish |
| PF-RT-033 | public handle threading | 独立 Handle 上的只读/close/query 并发满足 §5.1 矩阵；同一 wrapper 的 move/destroy 与成员访问不得 data race |
| PF-RT-034 | refresh × refresh | 并发 refresh 共享同一 single-flight；只有一个 Catalog build/publication，后到调用观察同一结果 |
| PF-RT-035 | refresh × shutdown | Shutdown 立即关闭 Open Instance；late refresh 不替换 Catalog；hung refresh 进入 Degraded 且不阻塞健康实例关闭 |

这些测试只验证生命周期行为，不冻结具体 queue、watchdog、TLS marker 或同步原语的类名和实现形式。

## 8.3 ABI 资格验证

| ID | 场景 | 必须证明 |
| --- | --- | --- |
| PF-ABI-001 | C11 header compile | Public Header 可由纯 C11 编译器独立编译 |
| PF-ABI-002 | C++17 header compile | 同一 ABI Header 可安全被 C++17 include |
| PF-ABI-003 | struct size / required prefix | append-only struct/vtable 可被旧 consumer 安全读取 |
| PF-ABI-004 | Interface version negotiation | Major 精确匹配，Minor 选择双方最高交集 |
| PF-ABI-005 | invalid output state | non-OK 后 output 保持 default-invalid |
| PF-ABI-006 | Error Buffer | 截断、空 buffer、UTF-8 boundary 和 size 语义正确 |
| PF-ABI-007 | Plugin ABI mismatch | 不解释不兼容高层结构，直接拒绝加载 |
| PF-ABI-008 | create normal failure | default-invalid output；不发布 Instance |
| PF-ABI-009 | create malformed success | 不调用未验证 function pointer；Library → Faulted；restart_required |
| PF-ABI-010 | create non-OK + non-default output | 按 ABI violation 处理 |
| PF-ABI-011 | shutdown-before-destroy | destroy 只在 shutdown Ok + active_call_count == 0 后执行 |
| PF-ABI-012 | old binary compatibility | 使用保留旧 Header + 旧 DSO/consumer binary 验证新 Runtime |
| PF-ABI-013 | vtable byte-range validation | 先验证 context/vtable 非空且 `vtable_size >= sizeof(rodaq_struct_header_t)`，再读取 header.struct_size；之后按 required prefix 与 `offsetof + sizeof(slot)` 对 readable byte range 做机器检查 |
| PF-ABI-014 | Foundation nullability/default-invalid | configure/query/shutdown/destroy 的 nullable/non-null 参数与 PluginInstance/InterfaceReference 唯一 default-invalid 表示可由 Header test 验证 |
| PF-ABI-015 | numeric boundary | `max_config_size &lt;= UINT32_MAX`、config length 边界、deadline conversion overflow/expired/infinite 行为可重复验证 |
| PF-ABI-016 | unknown plugin status | 未发布状态码在 create/configure 中触发 Faulted；在 query/operation 中返回 Internal 且不发布 partial output；在 shutdown 中进入 Unresolved 收敛 |
| PF-ABI-017 | malformed Error Buffer | null pointer/capacity/size 不一致、invalid UTF-8 和 OK + non-empty error 均被识别；合法 non-OK 主状态不被 diagnostic buffer 覆盖 |

同一 Plugin ABI Major 下，不只 layout 要兼容，既有函数的错误、线程、生命周期和 shutdown 语义也必须兼容。

## 8.4 Loader / Package 资格验证

| ID | 场景 | 必须证明 |
| --- | --- | --- |
| PF-LD-001 | discovery | 从配置 search path 发现合法 Manifest |
| PF-LD-002 | invalid package | 单个坏 Package 产生诊断，不污染其他 entry |
| PF-LD-003 | exact version load | `load(id, version)` 只选择 exact version |
| PF-LD-004 | default load | `load(id)` 只在 compatible Stable Release 中选择最高版本；只有 prerelease 时返回 Unavailable，prerelease 必须通过 exact-version overload 显式加载 |
| PF-LD-005 | duplicate ID/version different artifact | Conflict |
| PF-LD-006 | concurrent same artifact load | single-flight；只发布一个 PluginLibraryState |
| PF-LD-007 | artifact digest mismatch | fail closed |
| PF-LD-008 | transient dlopen validation failure | 未 publish handle 正常回滚 |
| PF-LD-009 | Resident DSO | Public PluginLibrary 释放不触发 runtime dlclose |
| PF-LD-010 | static initializer restriction | validation failure rollback 不需要 Plugin lifecycle cleanup |
| PF-LD-011 | Plugin ABI metadata | Manifest ABI Major/Minor 可用于 prefilter；加载后必须与 `rodaq_plugin_abi_version()` 完全一致 |
| PF-LD-012 | session artifact audit | 正式采集 Session metadata 自动记录实际 plugin_id/version/artifact_sha256 |
| PF-LD-013 | ROS2 + MQTT + MCAP + SQLite3 same process | 关键依赖/SONAME 组合可同时加载 |
| PF-LD-014 | max_loaded_plugins | 新 dlopen 前确定 ResourceExhausted |
| PF-LD-015 | SemVer build metadata tie | exact load 对完整 SemVer identity 做字符串匹配；default load 遇到相同最高 precedence、不同 build metadata identity 时返回 Conflict，不依赖发现顺序 |
| PF-LD-016 | hung dlopen/static initializer | load 进入 Lifecycle Supervision；threshold 后 Manager → Degraded/restart_required，其他 Open Instance shutdown 不被阻塞 |
| PF-LD-017 | bootstrap symbol provenance | ABI version / Entry symbol 的 defining ELF object 必须是已验证主 Artifact；dependency 同名 symbol 必须 fail closed |
| PF-LD-018 | immutable release directory | production writable release directory 被部署检查拒绝或 Loader fail closed；验证后替换主 Artifact 的测试不能加载未验证内容 |
| PF-LD-019 | package-local dependency replacement | 明确验证 V1 完整性边界：Framework SHA 只覆盖主 Artifact；private dependency 依赖 immutable release directory/包管理保护 |
| PF-LD-020 | Catalog resource extremes | plugin roots、package entries、manifest/file/scan/path/diagnostic 上界达到时无无界分配，旧 Catalog generation 保持可用 |
| PF-LD-021 | artifact audit public path | Core 通过 PluginLibrary::artifact() 或等价公开集成通路取得实际 SHA；不得访问 PluginLibraryState 私有实现 |
| PF-LD-022 | Manifest ingress | strict JSON、format version、未知/缺失字段、类型、共享 metadata、platform、path 和 digest 非法输入均 fail closed |
| PF-LD-023 | hung Catalog refresh | filesystem scan/Manifest parse/build 卡死进入 Lifecycle Supervision；threshold 后 Degraded，late completion 不发布 Catalog |

## 8.5 SDK 资格验证

| ID | 场景 | 必须证明 |
| --- | --- | --- |
| PF-SDK-001 | Minimal Plugin | 只实现 configure + shutdown 即可构建 |
| PF-SDK-002 | provide Operation | Backend 不写 Adapter/vtable/trampoline |
| PF-SDK-003 | Service Reference | `service&lt;T&gt;()` 返回 typed borrowed Reference |
| PF-SDK-004 | missing Service | NotFound 由 Plugin 自行决定 required/optional 策略 |
| PF-SDK-005 | configure failure | 不发布部分 Operation Registry |
| PF-SDK-006 | partial initialization shutdown | configure 中途失败后 shutdown 能安全清理已建立资源 |
| PF-SDK-007 | exception barrier | Plugin C++ exception 不越过 C ABI |
| PF-SDK-008 | Provider lifetime misuse | configure-local temporary Provider 通过 ASan/qualification 暴露 |
| PF-SDK-009 | Service Reference lifetime | shutdown Ok 后默认不再使用 borrowed Reference |
| PF-SDK-010 | install-prefix build | 独立 Plugin 工程只依赖安装后的 RoDAQ Package |
| PF-SDK-011 | full SemVer tooling | PROJECT_VERSION 默认路径与显式 PLUGIN_VERSION prerelease/build metadata 路径均生成唯一一致的 Descriptor/Manifest Plugin Version |
| PF-SDK-012 | Plugin configuration ownership | Framework 原样传递有界 UTF-8 JSON；Plugin `configure()` 自己解析/校验并对无效配置返回 InvalidArgument；不依赖 Framework Schema/validator |
| PF-SDK-013 | CloseFuture shared observer | CloseFuture copy/move 不创建第二次 close；多个 Future 观察同一个 terminal publication；moved-from Future 的 `operator bool()==false`，只能析构或重新赋值后再使用 |
| PF-SDK-014 | ServiceRegistry backing | move-only Registry wrapper 析构后，已创建 Instance 继续通过强 backing 安全使用 Service Reference；`add_shared()` Provider deleter 在控制锁外执行 |
| PF-SDK-015 | PluginManager RAII contract | Manager 为 move-only；析构 noexcept/non-blocking；正常退出必须显式 shutdown；同进程第二次 create 始终 Conflict |
| PF-SDK-016 | CMake registration contract | `rodaq_register_plugin()` 只接受已有 shared-library target，不接受 SOURCES；target 创建/依赖配置由标准 CMake 命令完成 |
| PF-SDK-017 | Application lifecycle cleanup | refresh/load/create/query/business 任一阶段提前失败后，都显式完成 Instance close（若已创建）与 Manager shutdown；最早 root cause 不被后续 cleanup failure 覆盖 |
| PF-SDK-018 | Plugin type construction contract | Concrete Plugin 必须 default-constructible；构造函数不执行长期 I/O/worker 启动；authoring support 对可检查条件 static_assert |
| PF-SDK-019 | trampoline exception mapping | create/configure/query/provider/shutdown 对 bad_alloc 与其他异常按 §4.8 统一映射；异常不越过 C ABI |
| PF-SDK-020 | Descriptor ingress validation | plugin_id/name/version 的长度、UTF-8/ASCII、canonical ID 与 strict SemVer 规则均在 publish 前验证 |
| PF-SDK-021 | PluginInstance move contract | move construction 后源 Handle 为空；move-assignment 在 API 层删除；析构 Open Handle 只触发非阻塞 async close |

## 8.6 独立工程资格验证

### Counter 四工程

必须提供四个相互独立的安装后工程：

```
counter_interface_sdk/
atomic_counter_plugin/
shared_memory_counter_plugin/
example_application/
```

必须证明：

- 两个 Backend 共用同一 Counter Interface SDK；
- Backend 不写 Adapter/vtable/trampoline；
- Backend 只使用 `PluginBase`、`PluginSetup`、Interface SDK、`RODAQ_DEFINE_PLUGIN`；
- Application 只通过安装后的 `rodaq::plugin` 和 Counter Interface SDK；
- `PluginLibrary::create_instance()` 直接返回 Open Instance；
- `close()` / `async_close()` 都可验证；
- generated Manifest/Descriptor 正确；
- 重复 load 同一 Artifact 不增加 loaded plugin 数；
- 示例只使用本章定义的当前 Public API，不依赖 Framework 私有生命周期或版本选择对象。

### ROS 2 → MCAP Vertical Slice

Counter 通过后立即进入真实链路：

```
ROS 2 Acquisition Plugin
→ Core data path
→ MCAP Storage Plugin
```

必须覆盖：

- ROS 2 callback / executor shutdown；
- Core backpressure wakeup；
- Payload ownership；
- MCAP finalize；
- DiskFull / DataLoss；
- close during in-flight callback；
- Humble / Jazzy 读取；
- PlotJuggler / Foxglove 回放。

随后补充 MQTT Acquisition 与 rosbag2 SQLite3 Storage。

## 8.7 性能与长期运行门禁

### Operation Hot Path

至少比较：

```
native direct call
raw C ABI call
managed Operation Handle call
```

覆盖 single-thread / multi-thread、amd64 / arm64、P50/P95/P99。

硬性要求：

- Framework mandatory heap allocation = 0 / call；
- mandatory cross-thread scheduling = 0 / call；
- PluginManagerState / PluginCatalog global lock = 0 / call；
- CallGuard acquire/release = O(1)。

### 长期运行

至少执行：

- 24h / 72h 持续运行；
- 高频 create/close；
- 多 Plugin 并存；
- repeated load same Artifact；
- shutdown failure / close worker block；
- ROS 2 + MQTT + MCAP + SQLite3 同进程；
- RSS / FD / native thread / queue/backlog 监控。

必须满足：

- loaded Plugin 数受 `max_loaded_plugins` 限制；
- active Instance 数受 `max_instances` 限制；
- repeated create/close 后 Instance 和业务资源回到基线；
- Faulted Library / Unresolved Instance 会触发明确 critical diagnostic 与 restart_required；
- Pending Close 在 queue pressure / worker recovery 后能够继续推进，不出现永久遗失的 Closing Instance；
- 整条 Close Pipeline 的 Pending Scheduling / Native Shutdown / Call Drain / Destroy / Backing Release / Terminal Publication 均有独立阶段时长监督；迟返不会重复 shutdown/destroy/terminal publication；
- Manager/Instance 锁顺序与锁外析构在 TSAN/故障注入下不产生死锁或 UAF；
- 正式采集 Session metadata 始终包含实际 plugin_id / plugin_version / artifact_sha256；
- 没有明确原因的 thread/fd/heap/backing 持续增长视为 leak。

Framework 只严格保证 Plugin/Instance 数量上限；RSS/FD/thread/第三方 cache 属于进程级监督资源。

## 8.8 开发体验资格验证

标准插件开发者路径必须保持：

```cpp
class MyPlugin final : public PluginBase {
public:
    Result<void> configure(PluginSetup& setup) override {
        auto service = setup.service<MyService>();
        if (!service &&
            service.status().code() != StatusCode::NotFound) {
            return service.status();
        }

        RODAQ_RETURN_IF_ERROR(
            backend_.configure(setup.config()));

        return setup.provide<MyInterface>(backend_);
    }

    Result<void> shutdown(Deadline deadline) override {
        return backend_.shutdown(deadline);
    }

private:
    MyBackend backend_;
};

RODAQ_DEFINE_PLUGIN(MyPlugin)
```

Application Golden Path 必须展示完整 cleanup，而不是依赖 `PluginManager` 的 non-blocking destructor：

```cpp
auto manager_result = PluginManager::create(options);
if (!manager_result) {
    return manager_result.status();
}
auto manager = std::move(manager_result.value());

auto refresh_result = manager.refresh();
if (!refresh_result) {
    auto primary = refresh_result.status();
    auto shutdown_result = manager.shutdown(deadline);
    if (!shutdown_result) {
        // 记录 cleanup failure；返回 primary root cause。
    }
    return primary;
}

auto library_result =
    manager.load("com.rodaq.acquisition.ros2", "1.2.0");
if (!library_result) {
    auto primary = library_result.status();
    auto shutdown_result = manager.shutdown(deadline);
    if (!shutdown_result) {
        // 记录 cleanup failure；返回 primary root cause。
    }
    return primary;
}
auto library = std::move(library_result.value());

auto instance_result =
    library.create_instance(services, config);
if (!instance_result) {
    auto primary = instance_result.status();

    // create/configure failure 可能仍有 Framework-owned async cleanup。
    // Manager shutdown 必须观察/收敛同一 cleanup 流程。
    auto shutdown_result = manager.shutdown(deadline);
    if (!shutdown_result) {
        // 记录 cleanup failure；返回 primary root cause。
    }
    return primary;
}
auto instance = std::move(instance_result.value());

auto acquisition_result =
    instance.query<AcquisitionInterface>();
if (!acquisition_result) {
    auto primary = acquisition_result.status();

    auto close_result = instance.close(deadline);
    if (!close_result) {
        // 记录 Instance cleanup failure；不覆盖 primary。
    }

    auto shutdown_result = manager.shutdown(deadline);
    if (!shutdown_result) {
        // 记录 Manager cleanup failure；不覆盖 primary。
    }
    return primary;
}

auto acquisition = std::move(acquisition_result.value());

// 业务阶段：Acquisition Stop / Storage Finalize 等业务结果
// 与 Plugin lifecycle cleanup 结果分别保存。
auto business_result = run_business(acquisition);

// 无论业务成功还是失败，都执行生命周期 cleanup。
auto close_result = instance.close(deadline);
auto shutdown_result = manager.shutdown(deadline);

if (!business_result) {
    if (!close_result || !shutdown_result) {
        // 记录后续 cleanup failure。
    }
    return business_result.status();
}

if (!close_result) {
    if (!shutdown_result) {
        // 同时记录 Manager cleanup failure。
    }
    return close_result.status();
}

return shutdown_result;
```

错误优先级固定为：

```
最早的业务/初始化根因
    > 后续 PluginInstance close failure
    > 后续 PluginManager shutdown failure
```

如果存在更早的 root-cause error，后续 cleanup failure 只记录到 Logging/Diagnostics，不覆盖根因；如果此前没有错误，则 cleanup failure 可以成为最终返回错误。

实际 Application 可以使用 application-local RAII/scope guard 或 Task 生命周期协调器减少重复 cleanup 代码，但这不是 Plugin Framework 新增的 Public Session/Task 对象。`PluginManager` 的 noexcept/non-blocking destructor 只承担异常路径 best-effort 防线，不能替代正常退出所需的显式 `shutdown()`。

规范性示例必须显式检查每一个 `Result&lt;T&gt;`，不得在失败状态下直接访问 `value()` 或通过 `operator-&gt;` 使用对象。

普通开发路径只允许使用当前公开的 Manager / Library / Instance、ServiceRegistry、PluginBase / PluginSetup 和业务 Interface SDK；Framework 内部加载选择、关闭调度、状态对象和 ABI bridge 不得泄漏到插件或应用代码。

如果标准 Plugin 仍需要理解 Entry、Descriptor、Plugin ABI、OperationRegistry、raw ServiceRegistry binding、CallGuard、Manifest format 或 Framework 内部 CMake target，则开发体验验证失败。

## 8.9 跨模块集成门禁

以下验证不改变 Plugin Foundation，但在 Acquisition/Storage Interface Freeze 前必须与 Core/Task 一起完成。

| ID | 场景 | 必须证明 |
| --- | --- | --- |
| PF-INT-001 | Task Stop ordering | 先关闭业务 admission，再停止采集/唤醒阻塞，再 drain/finalize，最后关闭 Plugin |
| PF-INT-002 | Backpressure × Acquisition callback | Task Stop 能唤醒等待 Core capacity 的提交 |
| PF-INT-003 | Acquisition Stop × Plugin shutdown | 业务停止与生命周期 shutdown 语义分离 |
| PF-INT-004 | Payload Release | destroy 前完成仍依赖 Plugin native context 的 Payload Release，或完成独立 ownership handoff |
| PF-INT-005 | Async Storage completion | Submission/Handoff、Payload Release、Persistence Completion 独立建模 |
| PF-INT-006 | Storage Finalize failure | DiskFull/DataLoss 独立传播；Plugin shutdown 仍可安全成功 |
| PF-INT-007 | blocking call wakeup | Stop 路径不存在 Acquisition/Drain/Finalize/Plugin shutdown 循环等待 |

推荐顺序：

```
1. close Task business admission
2. wake/cancel Task-bound blocking submission
3. Acquisition business stop
4. drain accepted records / Payload Release
5. Storage finalize / Persistence Completion
6. PluginInstance close
7. PluginManager shutdown（Application 退出时）
```

## 8.10 ABI/API Freeze 门禁

进入 0.3.0 / ABI/API Freeze 前必须满足：

- 第 3 章可以直接生成真实 C11 Public Header；
- Foundation 只包含 `configure / query / shutdown / destroy`；
- `rodaq_struct_header_t` 只包含 `struct_size`；
- Public Descriptor 只使用 `plugin_id / plugin_name / plugin_version`，不重复保存 Plugin ABI minimum runtime minor；
- 第 4 章可以直接实现 `PluginBase / PluginSetup / Provider Template`；
- Service C++ API 使用完整命名 `Reference`，并保留单一 `service&lt;T&gt;()`；
- 第 5 章只需要 `PluginManagerState / PluginLibraryState / PluginInstanceState` 三个主要共享状态对象；`State` 只表示共享对象，生命周期枚举统一使用 `PluginManagerPhase / PluginLibraryPhase / PluginInstancePhase`；
- Public close API 只有 `close()` / `async_close()`；异步 API 使用 `async_*` 前缀；同步 `close()` 的 DeadlineExceeded 允许来自 Observer Timeout 或真实 terminal failure，需要区分来源时使用 `async_close()+wait()+result()`；
- `PluginManager` 为 move-only Public Handle，析构 `noexcept`/non-blocking；`shutdown()` 是幂等观察接口，Stopped 再调用返回 Ok，Degraded 返回已记录终态失败；process-lifetime single authority 规则已经冻结；
- `PluginLibrary` Public 声明只有 §5.1 一个来源，包含 move-only 特殊成员、`info()` 和 `artifact()`；
- `PluginCatalog / Resolver / LoadKey` 都是 internal；
- Plugin Configuration 由 Plugin `configure()` 解析与校验；PluginManager / PluginLibrary 不提供通用配置 Schema/validator API 或结构化配置错误类型；
- Faulted Library / restart_required 阻止 Normal Shutdown Success；
- Pending Close Work 在 queue pressure 下不可丢失；
- Configure failure 在返回原始错误前完成 Manager cleanup ownership adoption；
- Catalog refresh 使用 single-flight；ShuttingDown 后不发布新 generation，Normal Shutdown Success Predicate 包含 active refresh；
- Native Shutdown Deadline 与 Observer Deadline 分离；独立 health supervision 覆盖 Catalog refresh、初始化、Pending Scheduling、Native Shutdown、Call Drain、Destroy、Backing Release 和 Terminal Publication；
- Managed OperationHandle 在 CallGuard 释放前完整构造；
- ManagerState/InstanceState 锁顺序与锁外 third-party execution 已冻结；
- `CloseFuture` 固定为可复制/可移动的 shared-state observer；复制不创建第二次 close；moved-from Future 的 `operator bool()==false` 且只能析构/重新赋值后再使用；`ready()/wait()/result()` 观察同一个 terminal publication，`result()` 在未 ready 时返回 InvalidState；
- `create_instance()` 具备明确 Open Commit Point，Manager Shutdown 获胜时不发布 Open Instance；
- Close Success Publication Point 晚于 backing release、Manager accounting commit 与 `max_instances` 容量归还；
- 同步 `close()` / `CloseFuture::wait()` / `PluginManager::shutdown()` 的 self-wait 限制已验证；
- Framework 只执行 Plugin configuration size 与 Public String contract 检查；JSON syntax 和业务配置校验由 Plugin 负责；`max_config_size &lt;= UINT32_MAX`，deadline conversion 不允许 signed overflow；Public 数值参数/字段的单位通过 Doxygen 定义，不编码进名称；
- Manifest ingress 固定为 strict JSON，`format_version == 1`，未知/缺失字段、类型、path、digest 或 Descriptor cross-check 非法时 fail closed；
- Plugin 返回的未知 StatusCode、非法 Error Buffer 和 malformed output 按 §3.2 契约违反规则 fail closed；
- `rodaq_register_plugin()` 只注册已有 shared-library CMake target，不接受 SOURCES；支持可选完整 SemVer `PLUGIN_VERSION`，并保证 Descriptor/Manifest 使用唯一最终版本值；默认 `load(plugin_id)` 只选择 Stable Release，prerelease 必须通过 exact-version overload 显式加载；Resolver 区分完整 Version Identity 与 SemVer precedence，相同最高 precedence 的不同 build metadata identity 必须 Conflict；
- Foundation ABI 已冻结 `vtable_size` byte-range 规则：必须先验证 context/vtable 非空和 `vtable_size >= sizeof(rodaq_struct_header_t)`，再读取 header；之后执行 required-prefix/slot 的机器可检查访问；参数 nullability 以及 PluginInstance/InterfaceReference 的唯一 default-invalid 表示也已冻结；
- `ServiceRegistry` 固定为 move-only wrapper + immutable shared backing；`add_shared()` 表示 Registry backing 共享持有 Provider，Instance 强持有 backing，Provider deleter 在控制锁外执行；
- configure failure 返回只表示初始化结果已确定，不表示 cleanup/capacity 已完成；安全 cleanup 完成前立即重试允许暂时 ResourceExhausted；
- 规范性 Application Golden Path 对全部 `Result&lt;T&gt;` 正确检查，并说明 RAII/scope-guard cleanup；
- 正式采集 Session metadata 自动记录实际 Artifact SHA-256；
- Counter 四工程通过；
- 旧 Header / 旧 DSO binary compatibility 测试通过；
- 当前尚未 Freeze 的草案之间如果存在 Foundation 方法集、struct layout 等 ABI 不兼容变化，必须明确作为新的 ABI baseline 处理；真实二进制发布后，同一 ABI Major 不得再进行此类不兼容修改；
- 安装后独立构建通过。

## 8.11 实施里程碑

| Milestone | 通过条件 |
| --- | --- |
| V1 Architecture Simplification | 本轮简化后的 Public API、Foundation 生命周期、Runtime State、Package 与 Configuration ownership 全部同步 |
| ABI/API Freeze Candidate | C11 Header + C++17 SDK + Counter + old-binary tests 通过 |
| Acquisition/Storage Interface Freeze | PF-INT-001～007 + ROS 2→MCAP stop/completion 流程通过 |
| Implementation Qualification | ROS 2/MQTT/MCAP/SQLite3、Humble/Jazzy replay、故障注入、long-run、performance 通过 |

<aside>
✅

**当前基线**

Document Version 0.2.3 继续保持为 Implementation-Ready Detailed Design Candidate；本次是 Plugin Framework Closure Fix，不等同于 production qualification 或 ABI/API Freeze。

在 V1 API / Runtime Simplification、F0 Safety Closure、Final API Closure 与 Freeze Contract Closure 基础上，本轮完成 **Plugin Framework Closure Fix**。仍保留四个 Foundation 方法、三个主要内部 State 对象、process-lifetime Single Manager、Resident DSO 和 Interface-first 总体架构。

本轮新增闭合：

- CMake helper 固定为 `rodaq_register_plugin(existing_target)`；target 创建和自身编译/依赖配置由标准 `add_library()/target_*` 完成，注册 helper 不接受 `SOURCES`；
- 内部长期对象统一使用 `State`：`PluginManagerState / PluginLibraryState / PluginInstanceState`；生命周期枚举统一使用 `PluginManagerPhase / PluginLibraryPhase / PluginInstancePhase`；`Status` 只表示操作结果；
- `PluginInstance::phase()` 与 `PluginDiagnostics::phase` 使用 Phase 语义，不再把共享 State object 与枚举混为一谈；
- `ServiceRegistry::add_shared()` 明确表达 immutable Registry backing 对 Provider 的 shared ownership；
- 同步 `close()` 的 `DeadlineExceeded` 明确可能来自 Observer Timeout 或真实 terminal failure；需要区分来源时必须使用 `async_close() + wait() + result()`；
- Lifecycle health supervision 现在覆盖 Catalog refresh、Load Scheduling / Native Load、Create Scheduling / Native Create、Configure、Pending Close、Native Shutdown、Call Drain、Destroy、Backing Release、Terminal Publication；hung refresh/load/create/configure 会进入 Degraded/restart_required，但不会阻塞其他健康 Instance 的关闭；
- Manager Shutdown 改为关闭现有 Open Instance 与监督 in-flight refresh/initialization 并行推进，禁止“先等待 active refresh/load/create 全部返回，再开始关闭”的串行顺序；
- Catalog refresh 纳入 Lifecycle Supervision 和 Normal Shutdown Success Predicate；并发 refresh 使用 single-flight，ShuttingDown 后 late completion 不得替换 Catalog；
- Public Handle 已补齐 copy/move/destructor 契约：PluginLibrary 为完整 move-only handle；PluginInstance 删除 move-assignment，析构只触发非阻塞异步关闭；并新增 Public API threading matrix；
- `PluginLibrary` Public 声明收敛到 §5.1 单一来源，并包含 `artifact()`；
- Session Artifact audit 通过公开只读 PluginArtifactInfo / PluginLibrary::artifact() 获取，不再允许 Core 访问内部 PluginLibraryState；
- Catalog/Manifest/Package 扫描与 diagnostics 增加明确的 Framework qualification hard limits；主 Artifact SHA 与 package-local dependency 的完整性责任边界被显式区分；
- Loader 增加 Bootstrap symbol provenance 与 loaded-object identity 校验，并明确 path-based loading 依赖 immutable release directory 的部署前提；
- Manifest 增加 strict JSON、format version、字段集合、metadata、platform、path 和 digest ingress validation；
- 未知 Plugin StatusCode、非法 Error Buffer 和 OK + malformed output 已定义 fail-closed 映射；
- Typed VTable 验证顺序已冻结：先证明 context/vtable 非空及 `vtable_size >= sizeof(rodaq_struct_header_t)`，再读取 Header，之后才执行 readable-size / required-prefix / slot byte-range 检查；
- Application Golden Path 已展示完整的 Manager/Instance cleanup 和 root-cause preservation；Manager 的 non-blocking destructor 明确不能替代正常显式 shutdown；
- `CloseFuture` 的 moved-from 行为已冻结，并提供 `operator bool()` 判断空 handle；
- 默认 `load(plugin_id)` 只选择 compatible Stable Release；prerelease 必须通过 exact-version overload 显式选择；相同 precedence 的不同 build metadata identity 仍返回 Conflict；
- Loader 的 publish-race rollback 表述和 CMake 示例格式已经修复；
- PF-RT / PF-ABI / PF-LD / PF-SDK Qualification 已同步覆盖 Catalog refresh 并发/关闭、unknown status、malformed Error Buffer、Manifest ingress、whole-close supervision、无丢失 async-close takeover、VTable header precheck、stable-only default load、moved-from CloseFuture、CMake registration contract 和完整 Application lifecycle cleanup。
- Interface Minor negotiation 使用 Provider `[0, provider_minor]` 与 Consumer `[min_minor, max_minor]` 的交集；不存在 ID、Major 不匹配和 Minor 不兼容分别映射为 NotFound、Unavailable 和 Unavailable；
- create non-OK 仅在 PluginInstance exact default-invalid 时视为普通失败；任何非默认 output 都作为 ABI violation 进入 Faulted Library / Unresolved Instance；
- 成功 Manager shutdown 在内部 lifecycle、close 和健康监督 worker 全部 join 后才发布 Stopped；process-lifetime authority 不要求 worker process-lifetime；
- Public SDK 不再公开 Managed Operation Access、Managed Interface Reference、OperationBuilder、Registry backing 或 sdk namespace；Service callback 通过 size-checked registry call scope 参与 self-wait 防护；
- Lifecycle Policy 与 Plugin Native `shutdown_timeout` 分离，refresh/load/create/configure 使用独立内部阶段监督；
- ROS Reference Plugin 使用 plugin-owned Context、rosbag2 QoS adaptation、system-clock timestamp 和显式 schema configuration；
- MCAP Reference Plugin 映射 complete WriterOptions 子集并对未注册 channel fail closed；F6 harness 使用容量 64、drop-newest、可观测 counters 的 test-only handoff；
- Catalog 的 roots/packages/entries/files/scan/path/diagnostic limits 与 Plugin DSO 两 symbol visibility closure 已纳入实现和资格测试。

此前已经冻结并继续保持的关键规则包括：Plugin Configuration ownership 属于 Plugin；Open Commit Point；Close Success Publication Point；统一 CallGuard；shutdown-before-destroy；Faulted Library / Unresolved Instance；Observer/Native Deadline 分离；ServiceRegistry immutable shared backing；完整 SemVer identity/precedence；Stable C ABI nullability/default-invalid；Resident DSO；跨模块 Business Stop / Payload / Persistence Completion 分离。

当前仍不升级到 0.3.0。正式 ABI/API Freeze 仍必须等待 install-prefix Counter 四工程、旧二进制兼容、完整 sanitizer/stress、长期运行和 deployment qualification。后续不再扩大 Plugin Framework Foundation；只有真实 Header/Skeleton 或 Reference Plugin 暴露通用机制缺陷时才回改设计。
