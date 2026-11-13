# RoDAQ Plugin Framework 开发计划

> 本文档是 RoDAQ Plugin Framework 当前开发阶段的直接实施基线。其目标是让 Codex 可以按阶段完成 Framework 主体实现，并使用两个真实 Reference Plugin 验证 Framework，而不是重新设计 RoDAQ 数据采集业务架构。

| 文档属性 | 内容 |
| --- | --- |
| 文档名称 | RoDAQ Plugin Framework 开发计划 |
| 文档标识 | plugin_development_plan.md |
| 计划版本 | 0.3.4 |
| 更新日期 | 2026-11-13 |
| 状态 | Development Baseline Candidate |
| 设计基线 | docs/design/plugin.md 0.2.3 |
| 编码基线 | docs/coding_guidelines/cpp_coding_guidelines.md |
| 语言基线 | Stable C11 ABI + C++17 SDK |
| 当前实现平台 | Linux ELF；Ubuntu 22.04；amd64；GCC 11.x |
| 当前 ROS 验证环境 | ROS 2 Humble |
| 当前范围 | Plugin Framework + ROS 2 / MCAP Reference Plugin functional validation |
| 非当前范围 | RoDAQ Acquisition/Storage 正式业务协议、Task、Pipeline、Recorder、MQTT、SQLite3、工业长期运行资格验证 |

# 1. 开发目标、权威输入与边界

## 1.1 开发目标

当前阶段首先完成一套不知道 Acquisition、Storage、ROS 2、MCAP 等业务概念存在的通用 Plugin Framework：

~~~
Stable C ABI
    ↓
Generic Interface / Service model
    ↓
C++17 Plugin SDK
    ↓
Package / Manifest / Catalog / Loader
    ↓
PluginManager / PluginLibrary / PluginInstance
    ↓
Lifecycle / CallGuard / supervision
    ↓
rodaq_register_plugin()
~~~

Framework 主体完成后，再在项目根目录 plugin/ 下实现两个真实 Reference Plugin：

~~~
plugin/
├── ros2_acquisition/
└── mcap_storage/
~~~

它们用于证明：

- Framework 能加载真实第三方依赖；
- Plugin 可以提供 typed Operation；
- Plugin 可以查询 Application Service；
- create / configure / query / call / close / shutdown 全链路可以工作；
- 生命周期和 ownership 规则可以支撑真实异步系统；
- CMake 注册、Package、Manifest、Catalog 和 Loader 可以支撑真实 Plugin DSO。

这两个 Reference Plugin 是 Framework 的消费者，不是 Framework 的设计输入。

## 1.2 唯一规范性输入

当前任务只允许以下内容作为规范性输入：

1. docs/design/plugin.md 0.2.3；
2. docs/coding_guidelines/cpp_coding_guidelines.md；
3. 当前 next 分支已经实现的 Status / Result / Logging 基础设施；
4. 当前 Linux / CMake / C++17 工程约束；
5. 本开发计划。

以下业务设计文档不得作为当前 Framework API、ABI、对象模型或生命周期设计输入：

~~~
docs/design/acquisition.md
docs/design/storage.md
docs/design/task.md
docs/design/pipeline.md
以及其它业务模块详细设计
~~~

如果仓库 Agent Guide 要求阅读系统架构文档，只能用于理解仓库整体约定，不得把其中的 Acquisition、Storage、Task、Pipeline 业务模型带入 Plugin Framework。

特别禁止因为现有业务文档中的 Role Protocol、Storage Commit、Payload、Stream、Segment 等模型而修改 Plugin Foundation。

## 1.3 Framework 必须保持业务无知

Framework Public ABI/API 只能理解：

~~~
Plugin
Operation Interface
Service Interface
Interface ID
Interface Version
typed vtable
opaque context

create
configure
query
shutdown
destroy
~~~

Framework 不得理解：

~~~
Acquisition
Storage
ROS 2
MCAP
MQTT
SQLite3
Topic
Record
Payload
Descriptor
Stream
Segment
Volume
Commit
~~~

因此 Framework 源码中不得出现按业务类型分支：

~~~
if acquisition ...
if storage ...
~~~

也不得在 include/rodaq/plugin/ 下增加 acquisition.hpp、storage.hpp 等业务 Interface。

## 1.4 Reference Plugin 的地位

ROS 2 和 MCAP 在本计划中属于 Reference Plugin / Integration Validation。

它们可以定义自己的 plugin-local typed Interface，但这些 Interface：

- 不属于 rodaq::plugin 公共 Framework；
- 不代表未来 RoDAQ Canonical Acquisition/Storage Contract；
- 不受 acquisition.md / storage.md 约束；
- 当前只服务于真实功能验证；
- 后续正式数据采集业务 Interface 可以替换它们，而不要求修改 Plugin Foundation。

允许的结构：

~~~
plugin/ros2_acquisition/include/rodaq/plugins/ros2_acquisition/
plugin/mcap_storage/include/rodaq/plugins/mcap_storage/
~~~

不允许：

~~~
include/rodaq/plugin/acquisition.hpp
include/rodaq/plugin/storage.hpp
~~~

## 1.5 当前阶段完成范围

当前必须完成：

| 能力 | 当前要求 |
| --- | --- |
| Stable C ABI | 完整功能 baseline |
| Generic Operation / Service | 必须 |
| C++17 Plugin SDK | 仓库内可用 baseline |
| ServiceRegistry | 必须 |
| PluginManager / Library / Instance | 必须 |
| Package / Manifest / Catalog | 必须 |
| Linux dlopen Loader | 必须 |
| SemVer resolution | 必须 |
| Artifact SHA-256 / provenance | 必须 |
| Resident DSO | 必须 |
| refresh lifecycle supervision | 必须 |
| load/create/configure supervision | 必须 |
| CallGuard / active call accounting | 必须 |
| async close / CloseFuture | 必须 |
| rodaq_register_plugin() | 仓库内可用 baseline |
| Counter test Plugin | 必须 |
| ROS 2 Reference Plugin | 必须 |
| MCAP Reference Plugin | 必须 |
| ROS 2 → MCAP integration smoke | 必须 |

当前明确延期：

- MQTT；
- SQLite3；
- DDS；
- Runtime / Task / Recorder；
- Server / Web；
- 安装前缀第三方独立工程 qualification；
- old-binary ABI compatibility matrix；
- 全量 ASan / UBSan / TSan；
- 大规模 stress；
- 多小时/多天 soak；
- 完整 benchmark；
- 工业长期运行 qualification；
- AppImage 最终 packaging qualification。

## 1.6 开发阶段测试原则

当前目标是尽快完成正确且可用的 Framework，不追求一次性执行全部 qualification。

每个阶段只要求：

~~~
related target build
必要 unit test
必要 contract test
关键 lifecycle test
少量关键 concurrency test
compiler warnings
clang-format
~~~

设计文档中的全部 PF-* 测试仍是正式 Freeze / Qualification 的完整集合；本计划只选择当前开发阶段必须证明的安全属性，其余明确延期。


# 2. 工程结构与构建基线

## 2.1 目标目录

Framework 通用 Public C ABI 统一放在 include/rodaq/plugin/c/：

~~~
include/
└── rodaq/
    └── plugin/
        ├── c/
        │   ├── abi.h
        │   └── interface.h
        │
        ├── deadline.hpp
        ├── interface.hpp
        ├── service_registry.hpp
        ├── plugin.hpp
        └── manager.hpp
~~~

Framework 实现：

~~~
src/
└── plugin/
    ├── abi_support.hpp
    ├── abi_support.cpp
    ├── semver.hpp
    ├── semver.cpp
    ├── manifest.hpp
    ├── manifest.cpp
    ├── artifact.hpp
    ├── artifact.cpp
    ├── catalog.hpp
    ├── catalog.cpp
    ├── loader.hpp
    ├── loader.cpp
    ├── service_registry.cpp
    ├── plugin_manager_state.hpp
    ├── plugin_library_state.hpp
    ├── plugin_instance_state.hpp
    ├── manager.cpp
    ├── library.cpp
    ├── instance.cpp
    ├── call_guard.hpp
    ├── lifecycle_executor.hpp
    ├── lifecycle_executor.cpp
    ├── close_executor.hpp
    ├── close_executor.cpp
    ├── diagnostics.hpp
    └── diagnostics.cpp
~~~

可以根据最终规模合并少量 private 文件，但禁止形成职责模糊的大型万能实现，例如 plugin_utils.cpp、plugin_common.cpp、plugin_runtime.cpp。

Reference Plugin：

~~~
plugin/
├── CMakeLists.txt
│
├── ros2_acquisition/
│   ├── CMakeLists.txt
│   ├── README.md
│   ├── include/
│   │   └── rodaq/plugins/ros2_acquisition/
│   │       ├── c/
│   │       │   └── interface.h
│   │       └── interface.hpp
│   └── src/
│       ├── plugin.cpp
│       ├── recorder.cpp
│       ├── topic_discovery.cpp
│       ├── subscription.cpp
│       ├── qos.cpp
│       └── message_definition.cpp
│
└── mcap_storage/
    ├── CMakeLists.txt
    ├── README.md
    ├── include/
    │   └── rodaq/plugins/mcap_storage/
    │       ├── c/
    │       │   └── interface.h
    │       └── interface.hpp
    └── src/
        ├── plugin.cpp
        ├── writer.cpp
        ├── schema_registry.cpp
        ├── channel_registry.cpp
        └── writer_options.cpp
~~~

Reference Plugin Interface 只在仓库内 integration 使用；当前阶段不把它们安装为 RoDAQ Framework 的正式业务 SDK。

测试：

~~~
tests/
└── plugin/
    ├── support/
    ├── abi/
    ├── counter/
    ├── framework/
    ├── loader/
    ├── lifecycle/
    └── integration/
~~~

## 2.2 CMake Target

Framework 唯一公开 target：

~~~
rodaq::plugin
~~~

不得公开：

~~~
rodaq::plugin_sdk
rodaq::plugin_runtime
rodaq::plugin_c_api
~~~

内部可以使用 private target，但不得进入安装接口。

Framework target 内部拓扑必须明确区分 authoring support 与 Runtime implementation。私有的 target 名称可以调整，但职责和依赖方向固定：

~~~
rodaq_plugin_authoring
    Public C/C++ headers + PluginBase/PluginSetup/Provider templates

rodaq_plugin_runtime
    Manager / Library / Instance / Loader / State / Executor

rodaq::plugin
    Application 使用的唯一 Public umbrella target
~~~

规则：

- `rodaq_plugin_authoring` 不依赖 `rodaq_plugin_runtime`；
- Plugin DSO 只获得 authoring support 与自身 plugin-local headers；
- Plugin DSO 不得链接 Runtime implementation target，避免携带第二份全局 Manager/State；
- Application 和 test harness 通过 `rodaq::plugin` 使用 Runtime；
- `rodaq_register_plugin()` 只注入 authoring include/definitions 与生成规则，不把 Runtime target 传给 Plugin DSO；
- Framework Public target 的 link interface 不得包含 ROS 2、MCAP 或任何 Reference Plugin target。

Reference Plugin 必须可完全关闭。CMake 至少提供：

~~~
RODAQ_BUILD_REFERENCE_PLUGINS=OFF
RODAQ_BUILD_ROS2_REFERENCE_PLUGIN=OFF
RODAQ_BUILD_MCAP_REFERENCE_PLUGIN=OFF
~~~

默认普通 Framework build 不启用 Reference Plugin，也不得执行 `find_package(rclcpp)`、查找 rosbag2 或查找 MCAP。F5 验证时显式打开对应选项。

Reference Plugin target 建议：

~~~
rodaq_ros2_acquisition_plugin
rodaq_mcap_storage_plugin
~~~

它们必须是真正 SHARED library，并通过 rodaq_register_plugin() 注册。

Application/test harness 不直接通过 link target 调用 Plugin implementation，而是通过 PluginManager 发现和 dlopen。

### Framework Isolation Build

每次 F4 门禁和最终验收必须额外执行一次不包含任何 Reference Plugin 的干净构建：

~~~
cmake -S . -B build-plugin-framework \
    -DRODAQ_BUILD_REFERENCE_PLUGINS=OFF \
    -DBUILD_TESTING=ON

cmake --build build-plugin-framework
ctest --test-dir build-plugin-framework --output-on-failure
~~~

该构建必须：

- 在未 source ROS 2 环境、未提供 MCAP package 的 shell 中可以配置；
- 不配置 `plugin/ros2_acquisition` 或 `plugin/mcap_storage`；
- Framework target link closure 中不存在 `rclcpp`、`rosbag2`、`mcap`；
- 通过全部 Framework-only tests；
- 不要求下载或安装 Reference Plugin 依赖。

静态隔离检查至少验证：

- `include/rodaq/plugin/` 不存在 `acquisition*`、`storage*`、`ros2*`、`mcap*` 业务 Header；
- `src/plugin/` 不按 Acquisition/Storage/ROS 2/MCAP 类型分支；
- `rodaq::plugin` 的 Public link interface 不引用 Reference Plugin target。

## 2.3 真正启用 C11

根工程当前只有 CXX，因此 F1 必须修改为真正启用 C：

~~~
project(rodaq LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
~~~

C ABI compile test 必须使用 .c 文件，并由 CMAKE_C_COMPILER 编译。

禁止使用 g++ 以 C++ 模式 include C Header 后声称 C11 已验证。

对应最低证据：

~~~
tests/plugin/abi/plugin_abi_c_test.c
tests/plugin/abi/plugin_abi_cpp_test.cpp
~~~

## 2.4 测试断言机制

现有部分测试使用 assert，而 Release/RelWithDebInfo 会定义 NDEBUG。新的 Plugin tests 不得依赖会被 NDEBUG 删除的标准 assert。

F1 建立最小 test support，例如：

~~~
RODAQ_TEST_CHECK(expr)
RODAQ_TEST_REQUIRE(expr)
~~~

失败必须：

- 始终执行；
- 输出 expression、file、line；
- 以非零 exit code 结束测试；
- 不依赖 NDEBUG。

同时增加一个 assertion self-test，证明 RelWithDebInfo 中失败条件仍能让测试失败。

当前阶段不要求立即把仓库全部旧测试迁移，但 Plugin Framework 新测试一律使用该机制或等价的不会被 NDEBUG 删除的测试框架。

## 2.5 编码附加约束

遵守 cpp_coding_guidelines.md。

此外 Plugin Framework 采用以下更严格的项目级约束：

- 不使用 std::optional；这是 Plugin Framework 附加约束，不代表 Coding Guidelines 全局禁止；
- 不使用 detail namespace；
- 代码命名中不出现 v1/v2；
- 不使用 raw owning new/delete；
- 不新增无明确职责的 Utils/Common/Runtime/Host 抽象；
- Public C++ API 不暴露第三方类型；
- Public API 不暴露 tl::unexpected；
- 失败操作统一使用 Status / Result；
- C ABI 使用 rodaq_*_t 与 RODAQ_STATUS_*；
- 参数方向和单位由 Doxygen 说明，不用 in_/out_ 或 _ns/_ms 等参数名表达。

Public Header 必须 self-contained，并满足 Include What You Use。

## 2.6 Compiler Warning Baseline

F0 必须在仓库 CMake 中建立可复现 Warning 配置，而不是依赖开发者手工创建 strict build。

项目自身 target 至少启用：

~~~
-Wall
-Wextra
-Wpedantic
~~~

CI、F4 门禁和最终验收启用：

~~~
-Werror
~~~

第三方 Header 和 third-party target 使用 SYSTEM include 隔离。建议提供 `RODAQ_WARNINGS_AS_ERRORS` 选项，默认普通开发构建可以关闭，F0/F4 CI 与最终验证必须打开。

不接受“当前 build 没有 Warning”但实际未启用 Warning flags 的证据。


# 3. Plugin Framework 实现合同

## 3.1 Stable C ABI

F1 必须机械实现 plugin.md 0.2.3 第 3 章，不允许为了实现方便改变 ABI。

至少包括：

- rodaq_status_code_t；
- rodaq_string_t；
- rodaq_bytes_t；
- rodaq_buffer_t；
- rodaq_deadline_t；
- rodaq_struct_header_t；
- rodaq_error_t；
- Interface Version / Query；
- InterfaceReference；
- Service Registry ABI；
- Plugin Descriptor；
- Foundation Plugin Instance；
- Plugin Entry；
- ABI version/bootstrap symbol。

Plugin lifecycle surface 由 Entry 的 `create` 加上 Foundation Instance VTable 的四个动作组成：

~~~
create
configure
query
shutdown
destroy
~~~

其中 Foundation Instance VTable 本身只允许 `configure / query / shutdown / destroy`，不得把 `create` 加入 VTable。

不得增加 prepare/start/stop/flush/finalize 等业务生命周期。

C ABI 必须：

- C11 可编译；
- fixed-width integer；
- opaque typed context；
- pointer + size；
- caller-owned Error Buffer；
- append-only struct/vtable；
- output default-invalid；
- C++ exception 不得跨 ABI。

## 3.2 StatusCode 与 Error Buffer ingress

0.2.3 新增合同必须在 F1/F2 实现，不能推迟到 production qualification。

Framework 不允许把任意 uint32_t 直接 static_cast 为 C++ StatusCode。

未知 Plugin status：

| 场景 | Framework 行为 |
| --- | --- |
| create / configure | ABI contract violation；Library Faulted；restart_required |
| query / typed Operation | 当前调用 Internal；不得发布 partial output |
| shutdown | Internal；不得 destroy；Instance 进入 Unresolved 收敛 |

Error Buffer：

- {nullptr, 0, 0} 是唯一 empty buffer；
- nullptr + nonzero capacity 非法；
- size > capacity 非法；
- size != 0 且 data == nullptr 非法；
- 非空 message 必须是有效 UTF-8；
- 合法 non-OK StatusCode 是主结果，malformed diagnostic 不覆盖主失败；
- StatusCode == OK 但 Error Buffer 非空或 malformed 时返回 Internal，output 按 default-invalid 规则丢弃。

这些转换和验证集中在 abi_support，不允许每个 trampoline/调用点各自解释。

## 3.3 Generic Interface / Service

interface.hpp 只实现通用 Interface 机制：

~~~
Interface ID
Interface Version
Interface query
typed vtable validation
Operation Handle
Service Reference
~~~

typed vtable 验证必须严格执行 0.2.3 顺序：

1. context 非空；
2. vtable 非空；
3. vtable_size 至少可读 rodaq_struct_header_t；
4. 此后才能读取 header.struct_size；
5. 验证 required prefix；
6. 每个 slot 通过 byte-range 验证后才解引用。

Operation Handle 调用必须进入统一 CallGuard：

~~~
lock Instance
require Open
active_call_count++
unlock

call typed ABI

lock
active_call_count--
notify drain if needed
unlock
~~~

Hot path 不得：

- 获取 PluginManager 全局结构锁；
- 访问 Catalog；
- 跨线程调度；
- 为普通调用执行不必要 heap allocation。

## 3.4 ServiceRegistry

实现：

~~~
add_shared<T>()
add_static<T>()
freeze()
frozen()
~~~

规则：

- Public wrapper move-only；
- freeze 后 backing immutable；
- add_shared 由 Registry backing 强持有 Provider；
- add_static 只用于 process-lifetime Provider；
- create_instance 捕获同一 immutable backing 的 strong ownership；
- Provider destructor/custom deleter 必须在 Framework control lock 外执行；
- Instance shutdown 成功后不再允许 Plugin 使用 borrowed Service Reference。

## 3.5 Plugin SDK

提供：

~~~
PluginBase
PluginSetup
RODAQ_DEFINE_PLUGIN
~~~

Plugin 作者正常只实现：

~~~
configure(PluginSetup&)
shutdown(Deadline)
~~~

Concrete Plugin：

- default-constructible；
- destructor noexcept；
- constructor 不执行长时间 I/O；
- constructor 不启动长期 worker/callback；
- 真正 backend 初始化放到 configure。

RODAQ_DEFINE_PLUGIN 自动生成：

- Descriptor；
- Foundation vtable；
- create/configure/query/shutdown/destroy trampoline；
- Bootstrap symbols；
- exception barrier。

异常映射：

~~~
std::bad_alloc → ResourceExhausted
other exception → Internal
~~~

destroy 必须 no-throw。

## 3.6 Manifest strict ingress

Manifest 必须完全遵守 plugin.md 0.2.3。

Parser 必须 fail closed：

- UTF-8 strict JSON；
- 拒绝 comments；
- 拒绝 trailing comma；
- 拒绝 NaN/Infinity；
- 拒绝 duplicate object keys；
- format_version 必须为整数 1；
- required 字段全部存在；
- unknown 字段拒绝；
- 字段类型严格匹配，不做宽松 coercion；
- plugin metadata 满足 Descriptor ingress；
- ABI major/minor 为 unsigned 16-bit；
- os 必须 linux；
- arch 只允许 amd64 / arm64；
- artifact path 必须非空相对路径；
- canonical path 不得逃逸 Package root；
- sha256 必须 64 lowercase hex；
- DSO 加载后 Manifest ABI 与 bootstrap ABI 必须完全一致；
- Manifest/Descriptor ID、Name、Version 必须 byte-identical。

JSON dependency 的选择必须验证能实现 duplicate-key detection；如果默认 parser 会覆盖重复 key，则必须使用 parse callback/SAX/等价 strict mode 解决，不允许忽略该合同。

## 3.7 Catalog 与 refresh

Catalog refresh 是受监督 lifecycle operation，不是普通无界同步扫描。

PluginManagerState 必须包含：

~~~
active_refresh_count
active_load_count
active_create_count
~~~

refresh stage 至少区分：

~~~
Refresh Scheduling
Catalog Scan
Manifest Read / Parse
Catalog Build
Catalog Publish
~~~

规则：

- 只有 Manager == Running 才准入 refresh；
- 并发 refresh 使用 internal single-flight；
- 只有 single-flight creator 增加 active_refresh_count；
- 后到 refresh 等待并观察同一个结果；
- filesystem scan / manifest parse 不在 Manager structural lock 内执行；
- health monitor 独立于被阻塞 worker；
- operational threshold 到期后 Manager → Degraded，restart_required = true；
- late refresh 可以完成内部验证/清理，但 Manager 已非 Running 时不得发布新 generation；
- refresh 不新增 Public Ticket/Future 或 per-call Deadline。

Catalog publication 必须是完整 generation 的原子替换。

plugins() 返回一致 snapshot。

load() 观察一个完整 Catalog generation。

## 3.8 Package / Loader

Package：

~~~
my_plugin/
├── manifest.json
└── lib/
    ├── libmy_plugin.so
    └── optional_private_dependency.so
~~~

主 Artifact SHA-256 由 Framework Manifest 管理。

Private dependency 不逐文件进入 Manifest，其完整性依赖 immutable release directory / package management。Framework 不扩展成通用软件包管理器。

Loader：

~~~
CatalogEntry
→ canonicalize
→ containment
→ SHA-256
→ capture pre-load file identity
→ artifact single-flight
→ dlopen(RTLD_NOW | RTLD_LOCAL)
→ dlsym bootstrap
→ dladdr provenance
→ ABI validation
→ Entry/Descriptor validation
→ Manifest cross-check
→ loaded-object identity check
→ publish only if Manager == Running
~~~

Bootstrap symbol 必须来自被验证的主 Artifact，dependency 同名 symbol 必须 fail closed。

成功 publish 后 DSO resident until process exit。

Public PluginLibrary handle 释放不得触发 runtime dlclose。

## 3.9 Manager / Library / Instance

内部主要共享对象只允许：

~~~
PluginManagerState
PluginLibraryState
PluginInstanceState
~~~

不要引入第二套 PluginRuntime/PluginHost/Activation/Session 等架构。

PluginManagerState：

~~~
phase
catalog
loaded_plugins
instances
active_refresh_count
active_load_count
active_create_count
LifecycleExecutor
AsyncCloseExecutor
capacity
diagnostics
restart_required
~~~

Manager phase：

~~~
Running
→ ShuttingDown
→ Stopped

Running / ShuttingDown
→ Degraded
~~~

一个进程只有一个 process-lifetime PluginManager authority。

PluginLibraryState：

~~~
NativeLibraryHandle
PluginInfo
PluginArtifactInfo
PluginDescriptor
PluginEntry
ArtifactIdentity
instance_count
Loaded/Faulted
diagnostics
~~~

PluginInstanceState：

~~~
instance_id
phase
raw Plugin Instance
ServiceRegistry backing
active_call_count
close state/result
strong PluginLibraryState
diagnostics
~~~

Instance：

~~~
Initializing
    ↓
   Open
    ↓
 Closing
  ↙    ↘
Closed  Unresolved
~~~

Initializing 永不发布给 Application。

## 3.10 create/configure 与 Open Commit

create_instance：

~~~
require Manager == Running
require Library == Loaded
require frozen ServiceRegistry
validate config size/string
reserve instance capacity
allocate PluginInstanceState = Initializing
Manager takes strong ownership
active_create_count++

supervised create
→ Foundation validation
→ supervised configure
→ freeze Operations
→ Open Commit
~~~

Open Commit 必须重新观察 Manager：

~~~
Manager == Running
→ phase = Open
→ publish PluginInstance

Manager != Running
→ do not publish Open
→ phase = Closing
→ publish Manager-owned cleanup
→ return InvalidState
~~~

configure failure：

~~~
preserve original Status
→ phase = Closing
→ publish Manager-owned pending cleanup
→ then return original Status
~~~

capacity 只有安全 cleanup 完成后归还。

## 3.11 Async close

PluginInstance：

- copy deleted；
- move constructor allowed；
- move assignment deleted；
- destructor noexcept；
- destructor 不阻塞，只触发/继续同一 async close。

Open → Closing 与 Pending Close Work publication 必须形成无丢失提交点。

queue 满、临时 allocation failure 或一次 scheduling failure 都不得产生 orphan Instance。

Close worker：

~~~
Plugin shutdown(native deadline)
→ if safe shutdown not established: preserve state / Degraded / Unresolved rules
→ drain active_call_count
→ destroy exactly once
→ detach ServiceRegistry/backing under lock
→ release backing outside locks
→ commit Manager accounting
→ release max_instances capacity
→ publish terminal close result
→ notify CloseFuture
~~~

CloseFuture 多个 observer 只观察同一流程。

Observer deadline 不取消真实 close。

## 3.12 Lifecycle Supervision 与 Manager shutdown

refresh/load/create/configure 和完整 close pipeline 都进入有界 supervision。

禁止：

- 每个 timeout 创建新线程；
- pthread_cancel；
- timeout 后提前 destroy；
- hung initialization 阻止其它健康 Instance 关闭。

Manager shutdown：

~~~
under Manager lock:
    Running → ShuttingDown
    reject later refresh/load/create

immediately:
    request close for all current Open Instances

concurrently:
    supervise active refresh
    supervise active load
    supervise active create/configure
    drain close work

late refresh:
    never publish Catalog

late initialization:
    never publish Open Instance
~~~

Normal Shutdown Success Predicate 至少包括：

~~~
restart_required == false
no Faulted Library requiring recovery
no Unresolved Instance
active_refresh_count == 0
active_load_count == 0
active_create_count == 0
no pending/running refresh work
no pending/running initialization work
no pending/running close work
all normal instances == Closed
~~~

不得先等待 active refresh/load/create 全归零再关闭已有健康 Instance。


# 4. 分阶段开发计划

## 4.1 F0 — Baseline 与测试基础设施

目标：让后续实现不再发生设计版本漂移。

任务：

1. 把本计划设计基线固定为 plugin.md 0.2.3；
2. 明确 acquisition.md / storage.md 等业务文档不是当前规范性输入；
3. 根 CMake 启用 C 与 CXX；
4. 设置 C11 / C++17 标准；
5. 建立 tests/plugin/ 结构；
6. 建立 NDEBUG-safe test assertion；
7. 建立 rodaq_plugin target skeleton，并区分 authoring/runtime 私有 target 职责；
8. 增加 Reference Plugin 构建选项，默认全部关闭；
9. 增加项目 Warning baseline 与可选 `-Werror` 配置；
10. 建立 Framework-only isolation configure/build/test；
11. 保证现有 Result/Logging tests 不回归。

阶段门禁：

- C compiler 可用；
- test assertion self-test 在 RelWithDebInfo 有效；
- Plugin Framework target skeleton 可以配置和编译；
- `RODAQ_BUILD_REFERENCE_PLUGINS=OFF` 的 Framework-only 配置成功；
- 项目 target 实际携带 `-Wall -Wextra -Wpedantic`，strict 门禁可使用 `-Werror`。

## 4.2 F1 — Stable C ABI 与 Public API Skeleton

实现：

~~~
include/rodaq/plugin/c/abi.h
include/rodaq/plugin/c/interface.h
include/rodaq/plugin/deadline.hpp
include/rodaq/plugin/interface.hpp
include/rodaq/plugin/service_registry.hpp
include/rodaq/plugin/plugin.hpp
include/rodaq/plugin/manager.hpp
~~~

同时建立 Public Handle 声明：

- PluginManagerOptions；
- PluginInfo；
- PluginArtifactInfo；
- PluginDiagnostics；
- PluginManagerPhase；
- PluginInstancePhase；
- PluginManager；
- PluginLibrary；
- PluginInstance；
- CloseFuture；
- ServiceRegistry。

实现基础 ABI validation：

- StatusCode；
- Error Buffer；
- String/Bytes；
- Deadline conversion；
- vtable readable-range validation；
- default-invalid output。

必要测试：

- PF-ABI-001 C11 Header compile；
- PF-ABI-002 C++17 include；
- PF-ABI-005 default-invalid；
- PF-ABI-006 Error Buffer 基础；
- PF-ABI-013 vtable byte-range；
- PF-ABI-014 nullability/default-invalid；
- PF-ABI-015 numeric/deadline boundary；
- PF-ABI-016 unknown status；
- PF-ABI-017 malformed Error Buffer。

F1 结束前不得开始 Reference Plugin。

## 4.3 F2 — Generic Interface、Service 与 Plugin SDK

实现：

- typed Operation Handle；
- typed Service Reference；
- ServiceRegistry immutable backing；
- PluginBase；
- PluginSetup；
- provide<T>()；
- service<T>()；
- RODAQ_DEFINE_PLUGIN；
- common ABI trampoline；
- exception barrier。

建立 test-only Counter Interface 与 Counter Plugin。

Counter Plugin 必须只依赖通用 SDK，不包含 ROS/MCAP。

F2 使用直接 C ABI / Provider trampoline fixture 验证 SDK，不构造 Public `PluginInstance`。F2 的测试 host 可以自行构造 frozen ServiceRegistry fixture，调用生成的 configure/query trampoline 并直接取得 typed InterfaceReference。

它至少验证：

~~~
Application Service
        ↓
Counter Plugin configure()
        ↓
provide Counter Operation
        ↓
direct Provider / trampoline query
        ↓
typed call
~~~

当前阶段需要证明：

- Backend 不手写 Foundation trampoline；
- Interface ID/version negotiation 正确；
- Service lifetime 正确；
- configure failure 不发布 partial Operations；
- exception 不跨 C ABI；
- Provider vtable、Interface version 与 trampoline 行为正确。

公共 `PluginInstance::query<CounterInterface>() → managed call` 路径属于 F4 验收；不得为了提前完成 F2 而引入一套临时 Manager，也不得把 F4 Runtime 提前声明为已完成。

## 4.4 F3 — Manifest、Catalog、Loader 与 CMake 注册

实现：

- SemVer；
- strict Manifest parser；
- Artifact SHA-256；
- path containment；
- Catalog generation；
- refresh single-flight；
- Loader；
- symbol provenance；
- rodaq_register_plugin(existing_target)。

rodaq_register_plugin()：

- 只接受已存在 SHARED library target；
- 不接受 SOURCES；
- 注入 authoring support；
- 生成/嵌入 Descriptor 所需 metadata；
- 生成 Manifest；
- 计算主 Artifact SHA-256；
- 构建 Package layout。

当前阶段 SDK/CMake 的目标是 repository-local functional baseline。

安装后 find_package + 外部独立 Plugin 工程 qualification 延后，不把“完整第三方 SDK 交付”作为当前 DoD。

F3 必须验证：

- discovery；
- invalid package 不污染其它 entry；
- exact version load；
- stable-only default load；
- duplicate identity conflict；
- concurrent same artifact load；
- digest mismatch；
- ABI metadata cross-check；
- SemVer build-metadata tie；
- provenance；
- strict Manifest ingress；
- concurrent refresh single-flight；
- refresh publication generation 一致性。

## 4.5 F4 — Runtime Lifecycle 与 Supervision

实现：

- PluginManagerState；
- PluginLibraryState；
- PluginInstanceState；
- create/configure；
- Open Commit；
- CallGuard；
- async_close；
- CloseFuture；
- close；
- AsyncCloseExecutor；
- LifecycleExecutor；
- health monitor；
- Manager shutdown。

优先使用简单可证明的 C++17 基础：

~~~
std::mutex
std::condition_variable
std::shared_ptr
有界 queue / slot
明确 state machine
~~~

当前阶段不要为了性能提前引入复杂 lock-free queue、intrusive refcount、自定义 allocator 或复杂 atomic protocol。

F4 必须验证：

- create/configure/query/call/close happy path；
- configure failure cleanup；
- repeated async_close exactly-once shutdown/destroy；
- Operation × close；
- create × shutdown；
- CloseFuture `wait()` 与 `result()` 的 Observer Timeout / terminal failure 区分；
- `result()` 在未 ready 时返回 InvalidState，ready/wait/result 观察同一个 publication；
- Open PluginInstance destructor 不阻塞并触发/复用 async close；
- 同 Instance self-wait 不形成死锁；
- Close success 只在 backing release、Manager accounting 和 capacity 归还后发布；
- 重复 create/close 后 capacity 回到基线，超过 `max_instances` 在 Plugin create 前拒绝；
- close queue pressure、临时 allocation/scheduling failure 不丢失 Pending Close；
- hung create；
- hung configure；
- shutdown × hung initialization；
- late initialization return；
- refresh × refresh；
- refresh × shutdown；
- hung refresh；
- late Catalog publication 被拒绝；
- `RODAQ_BUILD_REFERENCE_PLUGINS=OFF` 且未加载 ROS/MCAP 环境时完整 Framework build/test 通过；
- Warning strict build 在 `-Wall -Wextra -Wpedantic -Werror` 下通过。

F4 完成后，Plugin Framework 主体才算 Functional Complete。

## 4.6 F5 — 两个真实 Reference Plugin

只有 F1～F4 达到阶段门禁后才开始。

Reference Plugin 验证必须显式启用对应 target：

~~~
cmake -S . -B build-reference-plugins \
    -DRODAQ_BUILD_REFERENCE_PLUGINS=ON \
    -DRODAQ_BUILD_ROS2_REFERENCE_PLUGIN=ON \
    -DRODAQ_BUILD_MCAP_REFERENCE_PLUGIN=ON \
    -DBUILD_TESTING=ON
~~~

两个 Reference Plugin 可以分别启用并独立 smoke；F6 才要求两者同时启用。

ROS 2 / MCAP Plugin 只能消费 Framework，不得反向要求修改 Foundation，除非真实实现证明存在通用机制本身不可实现或自相矛盾的缺陷。

任何此类缺陷必须单独记录：

- 对应 plugin.md 条款；
- 最小复现；
- 为什么 generic mechanism 无法表达；
- 最小修订方案。

不得直接由 Reference Plugin 开发者新增 Foundation API。

## 4.7 F6 — Integration Validation

使用 test-only harness 完成：

~~~
ROS publisher
→ ROS 2 Reference Plugin
→ test-local Service / bounded handoff
→ MCAP Reference Plugin
→ generated .mcap
~~~

F6 的目的不是模拟完整 RoDAQ Core/Pipeline，而是验证：

- 一个真实 Plugin 可以调用 Application Service；
- 另一个真实 Plugin 可以提供 typed Operation；
- 两个不同第三方依赖 Plugin 能由同一 PluginManager 加载；
- serialized payload 可以经过真实 plugin boundary；
- close/shutdown 顺序不会产生 use-after-free；
- 输出 MCAP 可以由独立工具读取。

F6 不冻结未来 RoDAQ Acquisition/Storage 正式业务 Contract。


# 5. Reference Plugin 实现要求

## 5.1 开源复用原则

Reference Plugin 不允许对成熟能力从零重新设计。

实施顺序：

~~~
优先使用 upstream 公共 API
→ 复用成熟 algorithm/behavior
→ 仅实现 RoDAQ Plugin Interface glue
→ upstream 确实没有时才自行实现
~~~

每个 Plugin README 必须记录：

- upstream repository；
- branch/tag/commit 或系统 package version；
- 参考文件；
- 哪些机制直接使用；
- 哪些实现经过适配；
- 哪些只作为行为参考；
- license / attribution。

如果复制或改写 Apache-2.0 等 upstream 代码，保留必要 copyright 和 license notice。

## 5.2 当前依赖基线

当前机器实际环境：

| 组件 | 当前版本/来源 | 当前用途 |
| --- | --- | --- |
| ROS 2 | Humble | 当前必测 ROS environment |
| rclcpp | ros-humble-rclcpp 16.0.21 | GenericSubscription / graph / executor |
| rosbag2 | ros-humble-rosbag2 0.15.15 | ROS recording 行为参考 |
| rosbag2_storage_mcap | 0.15.17 | ROS-compatible MCAP 行为参考 |
| mcap_vendor | 0.15.16 | 当前 MCAP C++ library 来源 |

这些版本是当前开发环境证据，不是 Plugin Framework Public compatibility contract。

当前不得同时再引入另一份 third_party/mcap，除非确认 mcap_vendor 无法满足构建/打包需求并记录理由。

最终 AppImage / non-ROS deployment 的 MCAP packaging 策略留到后续 packaging qualification。

## 5.3 ROS 2 Reference Plugin

目录：

~~~
plugin/ros2_acquisition/
~~~

Plugin ID：

~~~
com.rodaq.reference.ros2
~~~

建议名称：

~~~
ROS 2 Reference Acquisition
~~~

这里的 “Acquisition” 是 Plugin 产品用途，不意味着 Framework 定义 Acquisition Role。

### 参考实现

重点参考 ROS 2 Humble 对应 rosbag2 recorder：

- topic graph discovery；
- GenericSubscription；
- serialized message callback；
- QoS adaptation；
- discovery loop/event；
- existing publisher endpoint inspection；
- subscription lifetime/error handling。

不要整体嵌入 rosbag2 Recorder，也不要复用：

- CLI；
- keyboard handler；
- pause/resume ROS services；
- snapshot；
- rosbag Writer；
- event publisher；
- 与 Framework 验证无关的 recording orchestration。

### plugin-local Interface

定义在：

~~~
plugin/ros2_acquisition/include/rodaq/plugins/ros2_acquisition/
~~~

该 Interface 仅为 Reference Plugin 测试使用，不进入 include/rodaq/plugin。

建议最小 Operation：

~~~
start()
stop()
~~~

其职责仅是控制 ROS subscription/discovery worker。

数据输出通过 test-local Application Service 完成，以同时验证 Framework ServiceRegistry。

Service 可以表达最小事实：

~~~
topic
type
serialization format
optional message-definition encoding/data
receive timestamp
serialized payload
~~~

这些字段只属于 Reference Plugin integration contract，不是 RoDAQ 正式 Record/Payload Contract。

### 有界 discovery

即使支持 explicit topics / all topics / regex，也必须有：

~~~
max_topics
max_descriptors
~~~

或等价硬上限。

运行时 discovery 超过上限时：

- 不创建新 subscription；
- 记录 bounded diagnostic；
- 返回/维持明确状态；
- 不允许 subscription、descriptor、schema 数量无界增长。

当前优先实现 explicit topics。

all topics / regex 可以在同一阶段实现，但必须受同一 cardinality 上限；如果实现明显增加复杂度，可延期而不阻塞 Framework DoD。

### QoS

不得为所有 Topic 固定一种 QoS。

优先复用 rosbag2 的成熟 QoS adaptation 行为或其公开 API。

至少正确处理：

- reliability；
- durability；
- history/depth；
- optional per-topic override。

### Message definition

Humble 与较新 ROS 2 distro 的 message-definition API 不同。

当前实现必须以 Humble 实际可用 API 为准，参考 Humble rosbag2/rosbag2_storage_mcap 对 ROS 2 Schema 的实现，不得为了匹配 Jazzy API 在 Framework 中加入 distro 特例。

无法可靠获得 definition 时，必须显式记录当前 capability，不得伪造 schema。

Humble/Jazzy 差异只允许存在于 ROS Plugin private implementation。

### ROS executor

Plugin 自己拥有 node/executor/worker lifecycle。

不能要求 Application “碰巧” spin 某个 Node。

constructor 不启动 worker。

configure 只建立可安全 cleanup 的资源。

start 后才开始 discovery/subscription。

stop 必须停止 discovery、释放 subscriptions、停止 executor activity 并 join owned threads。

Foundation shutdown 负责保证剩余 Plugin-owned async work 完成并进入可 destroy 状态。

### 当前 ROS 验证环境

当前必须通过：

~~~
Ubuntu 22.04 + ROS 2 Humble
~~~

当前机器只有：

~~~
/opt/ros/humble
/opt/ros/rolling
~~~

没有 Jazzy。

Rolling 不能替代 Jazzy。

因此 Jazzy build/smoke 从当前 DoD 移到后续 Compatibility Gate：

~~~
Ubuntu 24.04 + ROS 2 Jazzy CI/container/on-target
~~~

代码仍应避免无必要绑定 Humble private API，但当前验收只以 Humble 实证。

## 5.4 MCAP Reference Plugin

目录：

~~~
plugin/mcap_storage/
~~~

Plugin ID：

~~~
com.rodaq.reference.mcap
~~~

建议名称：

~~~
MCAP Reference Storage
~~~

“Storage” 同样只是 Plugin 用途名称，不形成 Framework Role。

### 参考实现

直接使用 Foxglove MCAP C++ writer。

重点参考：

- mcap::McapWriter；
- McapWriterOptions；
- Schema；
- Channel；
- Message；
- open/write/close；
- chunk/compression/CRC/index/summary。

同时参考 rosbag2_storage_mcap：

- ROS 2 profile；
- Schema name/encoding/data；
- Channel topic/messageEncoding；
- writer option mapping；
- independent readback behavior。

禁止自己实现：

- MCAP binary record encoding；
- chunk format；
- compression format；
- footer/index/summary layout。

### plugin-local Interface

定义在：

~~~
plugin/mcap_storage/include/rodaq/plugins/mcap_storage/
~~~

当前只作为 Reference integration contract。

允许使用最小接口，例如：

~~~
open(test_output_path)
register_channel(...)
write(...)
close()
~~~

这里的 output path 和 close 仅属于 Reference Plugin vertical slice，明确不得解释为未来 RoDAQ Storage Public Contract 或 Volume/Commit authority。

计划和代码注释必须明确该边界。

### ROS-compatible MCAP smoke

为了验证 ROS 2 Reference Plugin 与 MCAP library 能组合，Reference MCAP Plugin 应支持 ROS 2 profile 所需 metadata：

~~~
profile = ros2
schema name = ROS message type
schema encoding = actual definition encoding
schema data = actual encoded definition
channel topic = ROS topic
channel messageEncoding = actual serialization format
~~~

不得在 MCAP Plugin 中猜测 ros2msg/ros2idl；事实由 integration harness/ROS Reference Plugin 提供。

Schema/Channel 必须缓存 ID，不能每条消息重复注册。

### Writer options

当前只暴露必要 plugin-local config：

- chunk size；
- compression；
- compression level；
- CRC；
- message index；
- summary。

这些配置直接映射成熟 MCAP Writer options，不再设计一套新的通用 Storage 配置词汇。

## 5.5 F6 test harness

Integration Harness 只存在：

~~~
tests/plugin/integration/
~~~

它不是 Production Core，也不形成 Public API。

职责严格限制：

- 构造 ServiceRegistry；
- 接收 ROS Reference Plugin serialized message；
- 建立 bounded in-memory handoff；
- 调用 MCAP Reference Plugin Operation；
- 管理 test output path；
- 完成顺序化 stop/close；
- 使用独立工具验证结果。

禁止在 Harness 中实现：

- Task；
- Pipeline；
- Volume；
- Commit Ledger；
- Artifact Catalog；
- production backpressure；
- production crash recovery。

Harness 中任何 convenience contract 都必须标记 test-only / reference-only。


# 6. 当前测试、验收与 Codex 执行规则

## 6.1 当前阶段最小安全属性追踪

当前不跑全部 PF-*，但必须明确选择以下设计测试：

| 当前安全属性 | 设计测试 |
| --- | --- |
| C11 / C++ Header | PF-ABI-001 / 002 |
| default-invalid / Error Buffer | PF-ABI-005 / 006 |
| vtable readable range | PF-ABI-013 |
| nullability / numeric boundary | PF-ABI-014 / 015 |
| unknown StatusCode | PF-ABI-016 |
| malformed Error Buffer | PF-ABI-017 |
| create/open commit | PF-RT-001 / 016 等对应 create publication tests |
| configure cleanup | PF-RT-002 / 026 |
| Operation × close | PF-RT-003 / 004 |
| repeated close | PF-RT-005 |
| Observer wait / terminal result | PF-RT-006 / PF-SDK-013 |
| destructor / self-wait / manager self-wait | PF-RT-007 / 008 / 009 |
| close queue pressure / fairness | PF-RT-013 / 014 / 015 |
| lock order | PF-RT-021 |
| close publication / capacity baseline | PF-RT-022 / 023 / 024 / 025 |
| hung create/configure | PF-RT-028 / 029 |
| shutdown × hung init | PF-RT-030 |
| late init return | PF-RT-031 |
| refresh/load/shutdown | PF-RT-032 |
| refresh × refresh | PF-RT-034 |
| refresh × shutdown | PF-RT-035 |
| Manifest strict ingress | PF-LD-022 |
| hung refresh | PF-LD-023 |
| symbol provenance | PF-LD-017 |
| Resident DSO | PF-LD-009 |
| Framework-only build | F4 isolation gate；Reference Plugins OFF 且无 ROS/MCAP |
| compiler warning baseline | F0/F4 strict build；`-Wall -Wextra -Wpedantic -Werror` |

PF-* 编号以 plugin.md 0.2.3 为准。实现过程中如果表中某个旧编号与实际设计章节发生变化，应以设计文档名称/场景为事实，不自行修改合同。

## 6.2 当前必须存在的测试 target

建议控制为：

~~~
plugin_abi_c_test
plugin_abi_cpp_test
plugin_test_support_self_test
plugin_counter_sdk_test
plugin_manifest_test
plugin_catalog_loader_test
plugin_lifecycle_test
plugin_refresh_test
plugin_close_contract_test
plugin_framework_isolation_test
plugin_reference_ros2_test
plugin_reference_mcap_test
plugin_reference_integration_test
~~~

不要求把每一个 edge case 都拆成独立 executable。

## 6.3 Reference Plugin smoke

ROS 2：

~~~
test publisher
→ load ROS Reference Plugin
→ create instance
→ start
→ discover/subscribe
→ receive serialized message
→ call Application Service
→ stop
→ close
~~~

MCAP：

~~~
load MCAP Reference Plugin
→ create instance
→ open test output
→ register schema/channel
→ write known message
→ close
→ PluginInstance close
~~~

独立验证：

优先使用现有 mcap reader/tool；如果生成的是完整 ROS 2-compatible MCAP，再使用 ros2 bag info -s mcap 做 smoke。

不要为了测试 Writer 再自己实现一套 MCAP Reader。

F6：

~~~
ROS publisher
→ ROS Reference Plugin
→ ServiceRegistry callback
→ bounded test handoff
→ MCAP Reference Plugin
→ .mcap
→ independent readback
~~~

## 6.4 当前 Definition of Done

Framework：

- [x] plugin.md 0.2.3 是唯一 Framework 设计基线；
- [x] 没有从 acquisition.md/storage.md 引入业务模型；
- [x] include/rodaq/plugin/c/ 承载 Stable C ABI；
- [x] include/rodaq/plugin/ 只承载 generic C++ SDK；
- [x] authoring support 与 Runtime implementation 在私有 CMake target 中分离；
- [x] Reference Plugin DSO 不链接 Runtime implementation target；
- [x] `RODAQ_BUILD_REFERENCE_PLUGINS=OFF` 时 Framework target 不查找或链接 ROS 2/MCAP；
- [x] 根工程真实启用 C11 + C++17；
- [x] C11 Header 由真正 C compiler 编译；
- [x] 新 Plugin tests 不依赖 NDEBUG-sensitive assert；
- [x] rodaq::plugin 可构建；
- [x] Status/Error Buffer ingress 满足 0.2.3；
- [x] Generic Operation/Service 可工作；
- [x] ServiceRegistry immutable lifetime 正确；
- [x] PluginBase/PluginSetup/RODAQ_DEFINE_PLUGIN 可工作；
- [x] Manifest strict ingress 可工作；
- [x] duplicate JSON key 被拒绝；
- [x] SemVer selection 正确；
- [x] Catalog generation 原子发布；
- [x] refresh single-flight 正确；
- [x] active_refresh_count 纳入 shutdown predicate；
- [x] hung refresh 进入 Degraded；
- [x] late refresh 不发布 Catalog；
- [x] Loader SHA/provenance/identity validation 正确；
- [x] Resident DSO；
- [x] create/configure Open Commit 正确；
- [x] configure failure cleanup 正确；
- [x] CallGuard 正确；
- [x] async_close 非阻塞且无丢失；
- [x] CloseFuture wait/result publication 语义正确；
- [x] Open Instance destructor 非阻塞；self-wait 不形成死锁；
- [x] destroy exactly once；
- [x] create × shutdown 正确；
- [x] Close success 晚于 backing release、Manager accounting 与 capacity 归还；
- [x] 重复 create/close 后 capacity 回到基线；
- [x] hung init 不阻塞其它健康 Instance close；
- [x] Manager shutdown 并行收敛 refresh/init/close；
- [x] successful shutdown 停止并 join lifecycle/close/health workers；
- [x] Interface Minor negotiation 与 NotFound/Unavailable 区分正确；
- [x] create non-OK output 按 exact default-invalid 分类；
- [x] Public SDK 隐藏 raw access、builder、registry backing 和 sdk namespace；
- [x] Service callback 纳入 managed-call self-wait 防护；
- [x] `shutdown_timeout` 仅用于 Native Shutdown，其他 stage 使用内部 policy；
- [x] 无 Reference Plugin 的 shell 中 Framework build/test 通过；
- [x] Framework link closure 不含 rclcpp、rosbag2 或 mcap；
- [x] Framework-only 静态检查不存在业务 Public Header 或业务类型分支；
- [x] existing Result/Logging tests 不回归；
- [x] clang-format 通过；
- [x] 项目代码在 `-Wall -Wextra -Wpedantic -Werror` 下无 Warning。

Reference Plugin：

- [x] 位于项目根目录 plugin/；
- [x] Interface 位于各自 plugin-local include，不进入 rodaq::plugin；
- [x] ROS Plugin 基于 rosbag2/rclcpp 成熟实现路径；
- [x] ROS Plugin 使用 GenericSubscription；
- [x] ROS discovery 有 cardinality 上限；
- [x] ROS Plugin lifecycle 自持有并可收敛；
- [x] ROS Plugin 使用 plugin-owned Context、QoS adaptation 与 system timestamp；
- [x] Humble build + smoke 通过；
- [x] MCAP Plugin 直接使用 upstream MCAP library；
- [x] 不自行实现 MCAP format；
- [x] MCAP Plugin 参考 rosbag2_storage_mcap 的 ROS profile 行为；
- [x] ROS/MCAP upstream version/license 已记录；
- [x] F6 真实 ROS serialized message 可以写入可独立读取的 MCAP；
- [x] F6 harness 使用 bounded drop-newest handoff 与 counters；
- [x] 两个 Reference Plugin 均通过 PluginManager 真实发现/load/create/query/close。

明确不是当前 DoD：

- Jazzy smoke；
- install-prefix third-party Plugin build；
- old-binary compatibility；
- 全量 sanitizer；
- 全量 stress；
- long-run qualification；
- MQTT/SQLite3；
- 正式 RoDAQ Acquisition/Storage business interface。

## 6.5 Current Validation Evidence

截至 2026-11-13，当前工作树的实际验证结果：

| 验证 | 命令/环境 | 结果 |
| --- | --- | --- |
| Framework-only build/test | `RODAQ_BUILD_REFERENCE_PLUGINS=OFF`，RelWithDebInfo，`-Werror` | 30/30 CTest passed |
| 无 ROS 环境 Framework-only | 清除 `AMENT_PREFIX_PATH`、`CMAKE_PREFIX_PATH`、`ROS_DISTRO`、`ROS_VERSION`、`PYTHONPATH` | 30/30 CTest passed |
| Reference build/test | ROS 2 Humble + `mcap_vendor` 0.15.16，`-Werror` | 34/34 CTest passed |
| ASan + UBSan Framework-only | Debug，ASan/UBSan，`detect_leaks=0`（process-lifetime Manager/worker 设计） | 30/30 CTest passed |
| TSan | GCC 11 / `setarch -R` | 受 Ubuntu 22.04 TSan memory mapping 与 condition-variable false-positive 限制，不作为通过证据 |
| ROS 2 MCAP readback | `ros2 bag info -s mcap` | Reference MCAP 与 integration MCAP 均可读取，各 1 条消息 |
| Framework dependency isolation | `plugin_framework_isolation_test` + Framework-only link file | Public/源码/link closure 未发现 ROS 2/MCAP |
| clang-tidy | clang-tidy 14，仓库 `.clang-tidy`，Framework Runtime 源文件 | passed |
| Formatting | clang-format 14 `--dry-run --Werror` | passed |
| Whitespace/error check | `git diff --check`、`git diff --cached --check` | working tree passed；cached 由文档更新后复验 |

运行时依赖证据：

```text
ROS 2 Humble
rclcpp 16.0.21
rosbag2 0.15.15
rosbag2_storage_mcap 0.15.17
mcap_vendor 0.15.16
rosbag2_transport 0.15.15
nlohmann/json 3.10.5（MIT License）
OpenSSL 3.0.2（Apache License 2.0，Artifact SHA-256）
GCC 11.4.0
CMake 3.22.1
clang-format 14.0.0
```

Reference Plugin DSO 各自依赖 `rclcpp` 或 `libmcap`，但不链接
`rodaq_plugin_runtime`。Framework-only build 不配置 Reference Plugin target。

本轮最终门禁补充了以下确定性回归证据：

- `plugin_lifecycle_test` 阻塞 typed vtable validation，验证 Query CallGuard
  在完整 Operation Handle 构造前保持有效，close 必须等待；
- `plugin_close_concurrency_test` 验证一个 hung shutdown 不阻止另一个健康
  Instance 完成 close，并验证超时后 Manager 进入 Degraded；
- `plugin_invalid_success_test` 验证 Plugin 返回 OK 但携带 Error Buffer
  message 时按 ABI 违约 fail closed；
- `plugin_lifecycle_test` 验证 `max_loaded_plugins`、`max_instances` 和
  `max_config_size` 在进入后续 Plugin ABI 操作前执行限流。
- Interface version negotiation matrix 覆盖 exact、provider/consumer newer、
  below-minimum、major mismatch 和 missing ID；
- dependency DSO 同名 `rodaq_plugin_get_entry` fixture 验证两个 bootstrap
  symbols 都执行 provenance 检查；
- async Service callback self-wait、partial create failure output、unknown
  Operation status diagnostic 和 executor saturation accounting 均有独立回归；
- `nm -D --defined-only` 显示两个 Reference DSO 各只导出 ABI version 与
  Entry 两个 bootstrap symbols；
- `plugin_bounded_handoff_test` 验证容量 4 时非阻塞 drop-newest，integration
  smoke 使用容量 64 且 dropped counter 为 0；
- MCAP readback 验证 zstd chunk、ROS profile 与 system timestamp 不再是
  CLOCK_MONOTONIC epoch。

## 6.6 Codex 执行规则

Codex 必须按照：

~~~
F0
→ F1
→ F2
→ F3
→ F4
→ F5
→ F6
~~~

逐阶段实现。

每阶段：

~~~
implement
→ build related targets
→ run required tests
→ inspect git diff
→ fix local issues
→ next phase
~~~

不得一次性生成全部代码后最后统一修编译错误。

开始工作前：

1. 检查 branch 和 git status；
2. 保留用户已有修改；
3. 阅读 plugin.md 0.2.3；
4. 阅读 cpp_coding_guidelines.md；
5. 不把 acquisition.md/storage.md 等业务设计作为当前 Framework 设计输入。

实现过程中禁止：

- 为 Reference Plugin 增加 Framework 业务 API；
- 恢复 Role ABI；
- 增加 rodaq::plugin::Acquisition 或 Storage；
- 新增 runtime/sdk/host/detail 架构目录；
- 通过 message string 判断机器控制流；
- 在 control lock 内调用 Plugin/native callback/第三方 destructor/阻塞 I/O；
- timeout 后 force-cancel Plugin code；
- 为每个 timeout 新建线程；
- 为未来 MQTT/SQLite3 提前设计抽象；
- 为解决业务 Plugin 问题绕过 plugin.md 生命周期合同。

如果发现 plugin.md 0.2.3 的 generic contract 确实不可实现，必须先输出：

~~~
design clause
minimal reproduction
implementation blocker
why this is generic Framework problem
minimal proposed correction
~~~

不得自行修改 Public ABI/API。

## 6.7 最终交付范围

正常修改应主要集中：

~~~
CMakeLists.txt
include/rodaq/plugin/
src/plugin/
cmake/
plugin/
tests/plugin/
third_party/ 或 dependency configuration（仅确有必要）
docs/development/plugin_development_plan.md
~~~

不应为了当前任务修改：

~~~
docs/design/acquisition.md
docs/design/storage.md
Task
Pipeline
Runtime
Server
Web
其它业务模块
~~~

完成本计划后，应得到：

~~~
Generic RoDAQ Plugin Framework
        │
        ├── Counter contract proof
        ├── ROS 2 real Plugin proof
        └── MCAP real Plugin proof
                │
                ▼
      ROS 2 → MCAP integration smoke
~~~

该结果证明 Plugin Framework 已经具备继续承载后续正式业务 Interface 的工程基础，但不提前冻结任何 Acquisition/Storage 业务协议。
