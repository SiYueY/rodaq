# RoDAQ Plugin 公共框架设计

| 文档属性 | 内容 |
| --- | --- |
| 文档类型 | 模块详细设计 |
| 状态 | V1 详细设计基线 |
| 适用范围 | RoDAQ 动态插件公共 ABI、装载、配置、能力、资源、生命周期与兼容性 |
| 上位设计 | [系统架构设计](architecture.md) |
| 关联设计 | [Pipeline](pipeline.md) · [Acquisition](acquisition.md) · [Storage](storage.md) · [技术选型](technology_selection.md) |

## 1. 目标与边界

Plugin 框架负责让 Acquisition 和 Storage 后端能够以稳定、可验证、资源有界的方式装载到 RoDAQ 守护进程中。

Plugin 公共框架负责：

- 动态库发现与装载；
- 具有显式 ABI/Protocol 版本字段的稳定 C ABI；
- 插件元数据与角色识别；
- Configuration Schema；
- Capability；
- Resource Plan；
- Extension Query；
- Plugin Services；
- 结构大小、所有权、错误码和字符串/缓冲区规则；
- 线程安全和重入基础规则；
- 插件实例公共生命周期；
- ABI conformance 与依赖共存资格。

Plugin 公共框架不负责：

- Acquisition 的 Source/Stream/Record 语义；
- Storage 的 Batch/Segment/Commit 语义；
- Task 状态机；
- Pipeline Admission；
- HTTP 或 Web；
- 把所有后端业务参数统一成一个大配置结构。

**设计参考与取舍。** 本文借鉴成熟插件框架中已经被长期验证的机制，但不复制其业务对象模型。关键映射如下：

| 参考机制 | RoDAQ 吸收的原则 | 明确不照搬的部分 |
| --- | --- | --- |
| Vulkan Loader/Driver | manifest/pre-load discovery、独立 bootstrap/interface negotiation、协商后再解释具体 ABI | 不引入 Vulkan 式全局 proc-address API |
| OpenSSL Provider | 双向能力交换、framework services 与 plugin operations 分离、opaque state ownership | 不采用 `function_id + void(*)()` 的通用 dispatch；RoDAQ 保留 typed table |
| PipeWire SPA | Factory/Handle/Interface、support injection、低依赖 C ABI | 不采用字符串 type + `void*` 的通用 support array |
| GStreamer | Plugin → Feature/Factory → Instance 分层、统一 logging、静态注册事实不依赖瞬时环境 | 不引入 GObject/property 体系和运行期任意属性修改 |
| OBS | role descriptor 与 create/start/stop/destroy 的实例职责分离 | 不引入 module-level mutable lifecycle 或 UI settings API |
| PlotJuggler SDK | package/manifest、C ABI、CMake authoring helper、export/ABI gate | 不把其业务 plugin 类型直接映射为 RoDAQ role |
| ROS class_loader/pluginlib | C++ 插件作者体验、Factory/RAII wrapper | C++ inheritance/RTTI/STL 不作为跨 DSO 稳定 ABI |

因此 RoDAQ 的稳定边界采用 **Bootstrap Foundation + Common C ABI + Role Protocol + typed Services + C++ authoring adapter**。跨 DSO 不传 C++ 对象、STL、RTTI 或 exception，不把 GObject/SPA 对象系统引入项目。[Vulkan Loader Interface](https://vulkan.lunarg.com/doc/view/latest/windows/LoaderDriverInterface.html) · [OpenSSL Provider](https://docs.openssl.org/3.3/man7/provider/) · [PipeWire SPA Plugins](https://pipewire.pages.freedesktop.org/pipewire/page_spa_plugins.html) · [GStreamer GstPlugin](https://gstreamer.freedesktop.org/documentation/gstreamer/gstplugin.html) · [OBS Modules](https://docs.obsproject.com/reference-modules) · [PlotJuggler SDK](https://github.com/PlotJuggler/plotjuggler_sdk) · [ROS class_loader](https://github.com/ros/class_loader)

**V1 边界。** 一个受管插件 package 对应一个逻辑 `plugin_id`、一个角色和一个主 DSO；一个 DSO 不同时注册多个 RoDAQ plugin id，也不同时承担 Acquisition 与 Storage 两种角色。V1 不支持进程内热升级、热卸载或插件自行发现/加载其它 RoDAQ 插件。插件属于受信任的同进程组件，ABI 与资格测试用于约束合作式行为，不能提供恶意代码或内存破坏隔离；需要进程级隔离属于后续能力。

**双层开发接口。** Plugin SDK 同时提供 C ABI 与 C++ authoring API。C ABI 是唯一跨 DSO 的稳定二进制边界，也是 RoDAQ Loader、第三方语言绑定和 ABI conformance 的权威契约；C++ API 是插件作者的首选开发接口，用 `Status/Result`、RAII、强类型、抽象基类和自动 adapter 降低开发成本。C++ 对象、STL、RTTI 和异常**不跨 DSO**：插件内部 C++ 实现通过 SDK adapter 转换为同一套 C descriptor/function table，因此 C++ 开发效率不能削弱 C ABI 的稳定性。

**命名规则。** 文件名、导出符号名、函数名、类名和宏名不编码发布版本，例如使用 `manifest.schema.json`、`rodaq_plugin_get_descriptor`、`rodaq_bootstrap_request_t`，不使用 `*_v1`、`*V2*` 或 `*_1_1`。版本只作为 manifest/schema/ABI 字段和数值存在。公开 C ABI 采用 **project prefix + snake_case + `_t`** 的类型命名：`rodaq_string_t`、`rodaq_plugin_descriptor_t`、`rodaq_acquisition_instance_t`；对应 struct tag 使用 `struct rodaq_string` 这类无 `_t` tag，函数使用 `rodaq_xxx()`，常量/枚举值使用 `RODAQ_XXX`。callback/function-pointer typedef 使用 `*_fn_t` 或语义更明确的 `*_cb_t`。公开 C++ SDK 位于 `rodaq::plugin`，类型使用 PascalCase、函数保持项目统一的 lowercase_snake_case。这样 C/C++ 两层在调用点可立即区分，同时保持文件和符号身份与发布版本解耦。POSIX 对 `_t` 存在保留规则，但 RoDAQ 接受这一已知工程取舍，依靠唯一 `rodaq_` 前缀、受控公共头和 ABI conformance gate 避免命名冲突。

变量、字段和类型名优先表达**领域语义**，不表达数据流方向或占位角色。公共接口中禁止把裸 `in`、`out`、`args`、`context`、`info`、`data`、`value` 作为缺少领域限定的命名；应使用 `descriptor`、`capability`、`resource_plan`、`instance_config`、`member`、`element`、`batch_result`、`segment_size`、`submission`、`services` 等具体名。只有当所属类型已经完全限定语义时才允许简短字段，例如 `rodaq_string_t::data`、`rodaq_bytes_t::data` 和 typed getter 的 `value`。同一规则适用于 C++ API，避免 `CreateArgs`、`Context`、`Info` 这类为了以后扩展而预留的宽泛对象。

## 2. 插件接口契约与实例生命周期

### 2.1 模型、版本与配置

**设计原则。**

**ABI 保持稳定，业务语义分层。**

公共边界采用 C ABI，插件内部可以使用 C++17。

ABI 只表达稳定跨库事实：

- fixed-width scalar；
- 明确大小的 POD struct；
- opaque handle；
- borrowed buffer/string view；
- RoDAQ-provided output buffer；
- function table；
- explicit result code。

禁止稳定 C ABI 暴露：

- STL；
- exceptions；
- RTTI；
- `std::string` / `std::vector`；
- `nlohmann::json`；
- ROS 2 / MQTT / MCAP 等后端 C++ 类型；
- 编译器相关对象布局。

**公共 ABI 小而稳定。**

核心 ABI 只放所有 Acquisition/Storage 都必须具备的能力。

可选能力使用版本化 Extension Query。

不能为了避免扩展机制而持续把未来字段塞入核心 operation table。

**所有权必须显式。**

任何跨 ABI 的对象必须声明：

- owner；
- borrow duration；
- acquire/release；
- callback validity；
- destroy 前静止条件。

“调用返回后通常还有效”不能作为可依赖的接口契约。

**Plugin Package 与动态库模型。**

V1 插件以受管 package 交付，而不是把裸 `.so` 直接暴露给 Task：

```text
plugins/<package>/
  manifest.json
  lib/<plugin-artifact>.so
  schemas/<schema-digest>.json
  licenses/...
```

`manifest.json` 是 **pre-dlopen admission manifest**，至少包含：manifest format version、`plugin_id`、role、plugin semantic version、最低 Common ABI/RoDAQ feature floor、role protocol version、目标 OS/architecture、build identity、主 artifact 相对路径及 SHA-256、配置 schema catalog 及各 schema digest、关键共享依赖事实。所有相对路径必须在 package 根内解析，禁止绝对路径、`..` 逃逸和远程引用。

Task Configuration 的单个插件配置对象只保存 `plugin_id + schema_version + config`，不保存磁盘路径。字段已经处于 PluginConfig 作用域，不重复 `plugin_`/`config_` 前缀。Deployment Configuration 定义受管 `plugin_roots`、`allowed_plugins`、可选 `required_plugins`、可选 exact version/artifact digest pin 以及依赖共存支持矩阵。V1 每个 plugin root **只扫描一级 package 子目录**，不做任意递归文件发现；Deployment 同时冻结 `max_plugin_roots`、`max_plugin_packages`、`max_manifest_bytes`、`max_schemas_per_plugin`、`max_schema_bytes`、`max_dependencies_per_plugin`、`max_plugin_artifact_bytes` 和扫描总字节/诊断条目上限。

Plugin package discovery/admission/DSO validation 在 daemon 初始化阶段完成，形成该进程生命周期内 immutable catalog；Task Create/Start 不触发目录重扫或新的 `dlopen()`。单个非 required plugin 校验失败时将其排除并记录有界诊断，Core 仍可启动；Deployment 声明的 `required_plugins` 任一无法完成 package+DSO validation 时，daemon 不进入 control-ready。RoDAQ 在每个 `dlopen()` 前完成 manifest、路径/权限、目标架构、allow-list 和 artifact digest 校验，因此未获准 artifact 不得先进入 daemon 地址空间再判断身份。

`plugin_id` 是 ASCII、大小写敏感、无首尾空白的稳定命名空间标识，V1 最大 128 bytes，推荐 `rodaq.<role>.<name>`，例如 `rodaq.acquisition.ros2`、`rodaq.storage.mcap`。同一扫描范围内出现多个同 `plugin_id` artifact 且 Deployment 未通过 exact version+digest 唯一选择时，Catalog 必须 fail-closed，不能依赖目录顺序、首次发现或最后发现决定胜者。

Manifest version 使用 `<major>.<minor>` 十进制形式。V1 RoDAQ 支持 `1.0`：major 必须为 1，minor 必须 `<= RoDAQ 支持的最大 minor`；较新 minor 对旧 RoDAQ 报 `Unavailable`，旧 RoDAQ 不尝试忽略未知字段实现“猜测式前向兼容”。每个支持的 manifest minor 都由 Core-owned schema 精确定义允许字段，未知顶层字段拒绝。V1 `1.0` 顶层字段固定为：

```json
{
  "manifest_version": "1.0",
  "plugin_id": "rodaq.acquisition.ros2",
  "plugin_version": "1.2.3",
  "role": "acquisition",
  "required_common_abi": { "major": 1, "min_minor": 0 },
  "required_rodaq_features": [],
  "role_protocol": { "major": 1, "min_minor": 0 },
  "target": { "os": "linux", "arch": "amd64" },
  "build_id": "...",
  "artifact": { "path": "lib/librodaq_acquisition_ros2.so", "sha256": "..." },
  "schemas": [
    { "id": "rodaq.acquisition.ros2", "version": "1", "dialect": "draft-07", "path": "schemas/<schema-sha256>.json", "sha256": "..." }
  ],
  "environment_fingerprint_keys": [],
  "dependencies": []
}
```

`plugin_version` 使用 SemVer 2.0.0 文本并限制 64 bytes；`build_id` 最大 128 bytes；SHA-256 使用 64 个小写十六进制字符。Manifest role 只允许 `"acquisition"` / `"storage"`，分别映射 ABI role 1/2；V1 target 只允许 `os="linux"` 且 `arch="amd64" | "arm64"`；其它合法 target 对当前 RoDAQ 视为 `Unavailable`，格式非法则为 `InvalidArgument`。Manifest 中的 feature 名称在打包工具中转换为冻结的 bit registry，运行时 ABI 只使用 bitset，不在热路径解析字符串。

Manifest 自身由 Core-owned `manifest.schema.json` 按 strict JSON + Draft 7 校验；不是由插件提供 schema 来定义“插件清单如何解释”。`environment_fingerprint_keys[]` 列出该 backend 参与 fingerprint 的稳定 namespaced fact key，必须排序、唯一且与后端 conformance golden test 一致；没有环境依赖时为空数组。`dependencies[]` 每项固定包含 `kind = process_shared | plugin_private`、SONAME/逻辑库名、版本或 build identity 约束以及 package-private 时的相对路径/digest。`rodaq_add_plugin()` 从最终 ELF 的 `DT_NEEDED/RUNPATH` 生成并交叉检查 dependency facts；未声明的关键共享依赖、声明但实际缺失的 package-private dependency、RUNPATH 越出 package root 都使 package gate 失败。

`config_schema_catalog_digest` 不依赖 JSON 文本键顺序。按 `(schema_id, schema_version)` 的 UTF-8 字节字典序排序 `schemas[]`，输入以 ASCII domain separator `rodaq.schema_catalog\0` 开始，随后追加 `uint16 big-endian format_revision = 1`，再对每项依次追加 `schema_id / version / dialect / normalized relative path` 的 `uint32 big-endian length + raw UTF-8 bytes`，再追加解码后的 32-byte schema SHA-256，最后对整个字节流取 SHA-256。`rodaq_add_plugin()` 生成该 digest 并嵌入 descriptor，RoDAQ 在 pre-dlopen manifest 校验后独立重算，再与 runtime descriptor 比较。重复 `(id,version)`、非规范相对路径或长度超过 `uint32` 直接拒绝。Schema 文件本身仍逐项按各自 SHA-256 校验，因此 catalog digest 证明的是“版本集合与引用元数据一致”，而不是替代 schema artifact digest。

Manifest 本身有独立最大字节数/深度/成员数限制。SHA-256 只提供 package 内部完整性，不提供对未受信任目录的身份认证；V1 的 trust root 是受管安装流程、Deployment allow-list 以及 package root 的 owner/permission。Plugin root 只能来自 Deployment 配置的受信任绝对路径；package root、manifest、artifact、schema 和 package-local dependency 必须是 regular file/directory，canonical path 位于 package root 内，且 owner 属于 daemon 配置的 trusted UID/root，group/world writable 默认拒绝。symlink 不作为 V1 package artifact/schema/dependency 载体。

Package 在 daemon 运行期间视为不可变。安装/升级必须构造完整新 package、完成 digest/permission 校验后以受管发布步骤切换，并通过 daemon 重启生效；不得原地覆盖正在加载的 DSO/schema/dependency。未来若需要第三方市场分发签名，应在 package admission 层增加签名/证书协议，不改变插件 C ABI。

**Bootstrap、Common ABI 与 Role Protocol。**

插件兼容性分为一个永久稳定的 Bootstrap Contract 与五个独立版本域：

| 版本层 | 作用 | 兼容策略 |
| --- | --- | --- |
| Bootstrap Foundation | 在不知道 Common ABI layout 的前提下协商一个 Common ABI major/minor，并返回 opaque descriptor 地址/大小 | **永久固定**；只使用 `abi/bootstrap.h` 自己的 primitive 类型，不依赖 Common ABI struct/result |
| Common Plugin ABI | 公共基础类型、Plugin Services、descriptor 公共字段 | major 不兼容；minor 只做向后兼容尾扩展 |
| Role Protocol | Acquisition/Storage operation table | 各角色独立 major/minor + minimum table size |
| Extension Protocol | 单个可选扩展 | 每个 extension 独立 major/minor 协商 |
| Config Schema | `plugin_config` 的持久数据契约 | 与二进制 ABI 独立，多版本并存，不隐式迁移 |
| Plugin/SDK Version | 产品发布与开发工具版本 | 用于审计和支持矩阵，不直接代替 ABI 协商 |

Bootstrap 导出入口固定为：

```text
rodaq_plugin_get_descriptor
```

该符号名与函数签名属于永久稳定的 Bootstrap Foundation，后续版本**不得**通过 `_v2`、`_v3` 等新符号名演进。Bootstrap Foundation 不包含 Common ABI 的 `rodaq_result_code_t`、`rodaq_error_buffer_t`、`rodaq_plugin_descriptor_t` 或任何 Role/Service 类型；因此即使未来 Common ABI major 的 layout 完全不兼容，旧 Loader 仍能安全完成“是否支持该 major”的探测。

一次 bootstrap 调用只请求一个 `requested_common_abi_major`，并给出该 major 下 Loader 能理解的最大 minor。插件若支持该 major，返回不高于请求 minor 的最高兼容 minor及该 Common ABI descriptor 的 opaque pointer/size；若不支持则返回 bootstrap-level `UNSUPPORTED_ABI`。Bootstrap 调用必须纯、幂等、无副作用，因此支持多个 Common ABI major 的 Loader 可以按自身优先级重复探测不同 major；不得依赖第一次 probe 创建任何需要 teardown 的状态。

初始冻结值为：Common Plugin ABI `1.0`；Acquisition Role Protocol `1.0`；Storage Role Protocol `1.0`。ABI role 数值固定为 `0 = Invalid`、`1 = Acquisition`、`2 = Storage`，其它值当前为 unknown。所有 `0` major version 都是 invalid，不作为“自动协商”哨兵。

Minor 协商遵循统一规则：major 必须完全匹配；调用方声明自己支持的最大 minor 和实际 `struct_size`；接收方只能使用双方都理解且落在实际 size 内的字段。对于 Common/Role protocol，plugin descriptor/header 的 `version_minor` 表示该 artifact **支持的最大兼容 minor**，manifest 的 `min_minor` 表示它要求 RoDAQ 至少支持的 minor；RoDAQ 只有在 `rodaq_major == plugin_major && rodaq_minor >= plugin_min_minor` 时才继续，并以 `min(rodaq_minor, plugin_max_minor)` 作为 negotiated minor。新增字段不能改变旧字段语义或默认值；需要新 RoDAQ 能力的插件还必须声明 **RoDAQ Feature Floor**，不能只靠字段“恰好存在”推断功能可用。

**Plugin Descriptor。**

Bootstrap 返回的 descriptor 是 library-lifetime immutable table，至少包含：Common ABI version、descriptor size、`plugin_id`、plugin version、role、build identity、required RoDAQ feature bits、role protocol version/operation table、配置 schema catalog digest、extension-query entry。运行时 descriptor 的 `plugin_id/role/version/build identity/schema catalog digest` 必须与 pre-dlopen manifest 一致；manifest 的 feature 名称经 SDK registry 映射后的 bitset 也必须与 descriptor required bits 一致，不一致视为 Plugin Contract Violation 并拒绝注册。

RoDAQ Feature Registry 是 Common ABI 的一部分，bit 编号发布后不重排、不复用。当前 V1 baseline 没有可选 Common RoDAQ feature 时所有 required bits 必须为 0；后续 minor 分配新 bit 后，旧 RoDAQ 对任何未知且被插件标记为 required 的 bit 必须拒绝装载并报告 `Unavailable`，而不是忽略。Feature bit 只表达可选 Common ABI 能力，不能代替 Role/Extension protocol version。

Feature authority 固定为单向链路：Deployment/Loader 持有本进程可提供的 Common feature registry；Descriptor 的 `required_rodaq_features_*` 只是插件最低要求；创建实例时 `PluginServices.feature_bits_*` 是本次真正注入的 optional Common features。必须满足 `descriptor.required ⊆ services.feature_bits ⊆ RoDAQ available` 中的实际供给约束，具体是所有 required bit 都必须在 RoDAQ available 与本实例 services 中同时存在。Bootstrap 不携带 feature bit，service table pointer/struct tail 也不能自行推导 feature 已启用。

Descriptor 中的字符串和静态表由插件保持到 library lifetime 结束；RoDAQ 若需要跨 library 生命周期保存则复制。V1 library 不热卸载，因此正常运行期可借用这些 immutable descriptor facts。

**配置 Schema 多版本。**

Task 保存：

```text
plugin_id
schema_version
config
```

V1 manifest 的 `required_common_abi` 只声明一个 Common ABI major/minor floor，因此一个 V1 package 只按该 Common ABI major 进入运行时协商；Bootstrap Foundation 的 repeated-major probe 是为未来 manifest schema 明确声明多-major alternatives 时预留，不表示 V1 package 已支持跨 major 多描述符发布。

一个插件 package 可以同时支持多个历史 schema。`schema_id` 使用与插件身份相同的 ASCII namespace 规则，V1 推荐等于 `plugin_id`；`schema_version` 是十进制 `uint32` 的规范文本形式，范围 `1..4294967295`，禁止前导零和别名写法。Manifest 的 schema catalog 对每个版本记录 `schema_id`、`schema_version`、本地相对文件、digest 和 dialect。RoDAQ 按 Task 保存的 `(schema_id, schema_version)` 取得**精确 schema**进行结构校验；package schema lookup 缺失返回 `NotFound`，Task Prepare 将“已安装插件无法解释所需 schema”报告为 `Unavailable`，不猜测兼容，也不自动升级 Task Config。配置迁移只允许通过显式 Configure 生成新的不可变 Task Configuration version。

V1 配置 Schema 固定采用 **JSON Schema Draft 7 validation dialect**，RoDAQ 实现基线使用 `nlohmann-json-schema-validator` 2.4.1（`9abddfa905bb4889cc4ae55f87adcaa71dd3bc23`）；Schema validator 和 `nlohmann::json` 都只存在于 Core 配置边界，不进入插件 C ABI。

V1 只开放配置所需的有限 validation subset：`$schema`、`$id`、受限 `$ref`、`type`、`properties`、`required`、`additionalProperties`、`items`、`minItems/maxItems`、`minProperties/maxProperties`、`enum`、`const`、`minimum/maximum/exclusiveMinimum/exclusiveMaximum`、`multipleOf`、`minLength/maxLength`、以及分支数受限的 `allOf/anyOf/oneOf/not`。`pattern`、`patternProperties`、`format`、`content*`、`dependencies/propertyNames/contains/if/then/else` 等 V1 不需要的关键字默认拒绝，而不是静默忽略；协议地址、Topic、QoS、URI、文件格式等复杂语义由插件 `validate_config` 检查。以后新增 Schema keyword 必须作为配置契约变更单独资格化。

`$id` 只作为逻辑 schema identity，必须使用 `urn:rodaq:plugin-schema:<plugin_id>:<schema_version>` 形式，不参与网络定位。V1 禁止 remote/network `$ref` 和任意 `file://` 引用；允许的 `$ref` 只能是同一 schema 文档内 fragment 或 package `schemas/` 根内、manifest 已列出并校验 digest 的相对资源。Schema 文档字节数、引用深度、分支数量、对象/数组成员和累计 validation error 数受 Deployment 硬上限约束。RoDAQ 不应用 schema `default` 产生的 patch 去隐式修改持久 Task 配置；默认值必须在 Configure 时显式物化或由插件语义层给出确定行为。结构 schema 是 RoDAQ 权威，协议/后端语义仍由插件 `validate_config` 最终检查。

配置边界固定为：

```text
Strict JSON Task Config
  ↓ exact schema version lookup
bounded Core schema validation
  ↓
read-only Config API
  ↓
plugin semantic validation
```

配置校验严格分两级：RoDAQ Schema validation 负责类型、required、enum、范围、结构和 `additionalProperties` 等声明性约束；插件 `validate_config()` 只负责 schema 难以表达的字段关系与 backend 语义。两级都是有界、无副作用的 pre-instance 操作，不得联网、打开设备、创建线程、写文件或改变 process-global state。需要实际外部资源才能验证的条件必须延迟到 `prepare()`。

插件不得保存 Config API node/string 超过当前 ABI 调用，也不得自行从 Task 参数打开额外配置文件来绕过 schema 和 Deployment 边界。

### 2.2 能力、资源与实例生命周期

**Capability。**

Plugin 能力分为三个互不混用的层次：

- **Static Protocol Support**：由 `role`、Role Protocol version/required prefix、operation-table tail 和 Extension Query 表达的 library-lifetime 静态协议支持，描述该 DSO **实现了哪些协议接口**；它不是额外的 Capability struct。
- **Configured Capability**：`validated config + explicit stable environment facts` 的确定结果，描述本次配置在创建 instance 前实际可用什么，并参与 admission。
- **Runtime State**：连接、发现、吞吐、队列、last error 等运行事实，由 Acquisition/Storage Snapshot 暴露，不属于 Capability。

RoDAQ 不从配置字段反推 Static Protocol Support，也不把瞬时设备在线状态或当前系统负载伪装成 Configured Capability。Library-level extension support 只通过 `query_extension()` 判断；Configured Capability 只表达当前配置实际启用/可用的 role feature bits，不能维护一份与 Role/Extension protocol 重复且可能漂移的“静态能力列表”。

Acquisition 和 Storage 的具体 Configured Capability 在各自文档定义。Configured Capability 对一个实例生命周期必须稳定；若配置或稳定环境事实改变能力，必须重新执行 validation → capability → resource plan → admission → create，不能在现有实例上静默改变能力集合。

**Plugin Resource Plan。**

在创建实际运行实例前，插件根据已验证配置和显式运行环境事实生成 Resource Plan。所有 count/byte 字段都是该配置下**可归属到插件实例的有限硬峰值上界**：`0` 表示不使用，不表示 unknown；禁止使用 `UINT*_MAX` 等哨兵表达“无限/不知道”。无法给出有限上界的资源不能伪装成可准入字段，而应归入架构定义的 shared/external resource qualification，或使该配置不能进入 V1 production support。

Resource Plan 至少覆盖 rodaq-facing buffers、plugin-private buffers、max in-flight records/bytes、threads/executors、FD/socket、blocking control slots、callback concurrency、max record、role-specific reserve。线程/FD 计数必须包含插件自身和其可归属第三方库创建的后台资源；进程共享且无法归属到单实例的资源必须单独声明。RoDAQ 聚合所有 plan 时使用 checked arithmetic，任何整数溢出直接拒绝准入。

Resource Plan 是资源申请与上界承诺，不是 CPU、disk throughput、memory bandwidth、scheduler latency 的性能保证；后者通过 production qualification 验证。插件属于同进程可信组件，但 RoDAQ 不能盲信声明：conformance、stress、实际 RSS/thread/FD/HWM 和 backend qualification 必须验证实际占用没有突破声明。超出声明即 Plugin Contract Violation，不允许静默扩容掩盖。

Capability 和 Resource Plan 都携带相同 `environment_fingerprint`。RoDAQ 完成准入后把该值作为 `expected_environment_fingerprint` 传入 `create()`；插件必须在 create 前再次核对影响 capability/plan 的运行环境。如果环境已经变化，create 返回 Conflict/Unavailable，RoDAQ 重新执行 capability → plan → admission，不能继续使用过期准入结果。

**Instance Lifecycle。**

公共框架区分 **pre-instance admission** 与 **instance lifecycle**，不再定义没有实际 ABI 调用对应的 `Configured` 状态：

```mermaid
stateDiagram-v2
    [*] --> PackageValidated
    PackageValidated --> DescriptorValidated: dlopen + bootstrap validation
    DescriptorValidated --> ConfigValidated: schema + semantic validation
    ConfigValidated --> Admitted: capability + resource plan + Core admission
    Admitted --> Created: create success
    Created --> Prepared: role prepare success
    Prepared --> Operational: role start/open as applicable
    Created --> Quiescing: failure / shutdown
    Prepared --> Quiescing: failure / shutdown
    Operational --> Quiescing: quiesce / shutdown
    Quiescing --> Quiescent: all access settled
    Quiescent --> Destroyed: destroy
    Destroyed --> [*]
```

`Operational` 只是公共占位，Acquisition 细化为 Started/callback-capable，Storage 细化为自己的 Segment/Writer 状态。**一旦 create 成功返回 instance，无论后续 prepare/start/open 等成功还是失败，在 destroy 前都必须完成 quiesce。** `quiesce()` 是幂等收敛操作：对 Created/Prepared/Operational/failed instance 均合法；已 Quiescent 再调用返回 OK。create 失败时 `instance` 必须保持 null，插件自行清理未发布 partial state，RoDAQ 不对半实例调用 destroy。

Plugin DSO/bootstrap 本体没有独立业务生命周期：`dlopen + rodaq_plugin_get_descriptor()` 只完成静态 ABI/descriptor 暴露，不联网、不打开设备、不启动线程、不创建业务资源；成功进入 Catalog 后 DSO 常驻到 daemon 退出。真正有副作用的生命周期全部属于 role instance。Task 的 Pause/Resume 也不是 Plugin lifecycle：Pause 不映射为 `quiesce()`，Resume 不映射为第二次 `start()`；暂停期间如何关闭 admission/丢弃或抑制数据由 Pipeline 与 role 详细设计决定。

**DSO-global state policy。** V1 允许 DSO 级 immutable descriptor/table、只读 lookup data 和经过资格验证的 thread-safe backend metadata；默认禁止插件拥有 mutable operational singleton、DSO-global worker thread/queue、共享业务连接/设备句柄，或任何必须依赖 plugin-level teardown 才能正确回收的业务资源。不可避免的第三方 process-global state 必须进入 dependency qualification 与 shared/external resource model，证明线程安全、并发边界和 daemon-exit 回收行为；不得把运行期 `dlclose()` 或一个不存在的 `plugin_shutdown()` 当作恢复机制。

Quiescence 是唯一销毁前安全证明。destroy 只释放已经 Quiescent 的本地资源，不执行首次 unsubscribe、flush、seal、外部 I/O、未知线程 join 或 RoDAQ callback；不得成为 quiesce fallback。destroy 必须 noexcept-equivalent，并通过资格测试证明在已 Quiescent 前提下具有有界完成时间。destroy 返回后插件不得再访问 instance、RoDAQ service state 或借用对象；library handle 生命周期长于所有实例、callback、operation/extension table 和 function pointer。

**Plugin Services。**

V1 公共 Plugin Services 固定为：**Clock、Logging**；read-only Config API 是 per-call 配置读取协议，不属于运行时 service directory。Acquisition 另有显式 typed role services。V1 不定义通用 Diagnostic、Reporting、Event 或 Metrics service；非返回路径的人类可读运行信息统一经 Logging，正式调用失败通过 result code 与 ErrorBuffer 表达。V1 也不提供任意 allocator、filesystem、Task/Pipeline controller、全局 registry、隐式 cancellation service 或通用 service discovery。未来新增公共能力必须先证明现有 Result/ErrorBuffer、Logging 和 role-specific interface 无法满足，再通过独立 typed service table 与 Common ABI 向后兼容 tail/feature bit 协商。

每个 RoDAQ callback 必须逐函数冻结 `version/struct_size`、owner/lifetime、允许调用的线程域、是否可并发、是否可阻塞/分配、是否可能反调插件、失败输出和最大工作量。公共基线为：`get_clock_sample()` thread-safe、bounded、无外部 I/O、不得反调插件；role callback 的更严格规则由 Acquisition/Storage 文档定义。

Plugin Services 不暴露 Task state machine、Pipeline private object、HTTP、arbitrary filesystem service、global singleton 或 mutable system container。

## 3. ABI 行为与并发

### 3.1 Result、Buffer 与 Extension

**Result Model。**

C ABI 使用稳定的 plugin result code，不直接暴露 C++ Status。

公共 result code 与当前 Core `StatusCode` **一一同值**：Ok、InvalidArgument、InvalidState、NotFound、Conflict、ResourceExhausted、DeadlineExceeded、Unavailable、PermissionDenied、DataLoss、Internal。`PersistenceUnknown` 等角色级数据语义不扩张公共错误字符串，而由 Storage 等 role result struct 的结构化字段表达。

已发布 result 数值不重排、不复用。RoDAQ 收到当前 Common ABI 未定义的 result 数值时不得直接 cast 成 Core Status；应按 Plugin Contract Violation 记录原始数值，并对本次调用映射为 Internal/Unavailable，停止依赖该未知结果推进状态机。每次失败可通过 caller-owned ErrorBuffer 提供有界 error detail；error text 不参与恢复逻辑；C++ exception 必须在插件 ABI wrapper 内捕获，`std::bad_alloc` 映射 ResourceExhausted，其它未知异常映射 Internal，任何 exception 都不得跨越 C ABI。

**Buffer 与字符串规则。**

跨 ABI buffer/string 使用：

```text
pointer + size
```

必须明确：

- null + zero 是否合法；
- max size；
- encoding；
- 是否 NUL-terminated；
- input borrow duration；
- output capacity；
- truncation rule；
- required size reporting。

稳定 ABI 不返回由另一侧用 `free()` 释放的隐式堆内存。

V1 公共 ABI 优先使用 caller-owned output buffer 或 plugin-owned immutable lifetime-defined view；公共 Plugin Services 不提供通用跨 ABI allocator。若某个角色确实需要跨调用动态所有权，必须在该 role/extension 中定义成对 acquire/release 和资源计费，不能要求另一侧调用普通 `free()`。

**Extension Query。**

可选能力通过 `query_extension(extension_id, requested_major, requested_minor, ...)` 获取 library-lifetime versioned operation table。协商算法固定为：extension id 大小写敏感且全局稳定；major 必须精确匹配；`requested_minor` 表示 RoDAQ 可理解的最大 minor，插件返回 `<= requested_minor` 的最高支持 minor，并在返回 table header 中声明实际版本和 `struct_size`。调用方还要检查实际 minor 不低于自己所需的 feature floor；不存在兼容 major/minor 时返回 `NotFound`，表示请求的 extension protocol 不存在可用版本。

Extension presence 只表示该 DSO 实现该 protocol，不表示当前配置一定可启用。配置相关可用性必须由 configured capability 显式给出；例如 library 提供 payload-retain extension，不代表每个 Acquisition 配置都支持 retain。未知 extension id、major 不匹配或没有满足 required minor floor 的兼容版本都返回 `NotFound`，并将输出 pointer/size 置为安全空值。

Extension 不能绕过公共 lifecycle、quiescence、resource accounting、RoDAQ feature floor、threading 和 ownership 规则；Extension operation table 的函数指针同样属于 library lifetime，并接受与 role table 相同的 symbol/contract qualification。

### 3.2 Thread Safety、Reentrancy 与 Quiescence

**Thread Safety 与多实例。**

V1 默认**不同 plugin instance 可以被 RoDAQ 并发调用**；插件不得依赖“整个 DSO 被 RoDAQ 全局串行化”这一隐含条件。descriptor、operation/extension table、schema/manifest cross-check facts 都必须是 library-lifetime immutable 并可并发读取。实例 mutable state 必须归属该 instance；若第三方 backend 使用 process-global state，该状态必须 thread-safe、进入 dependency qualification，并且其线程/FD/内存等共享资源不能逃出资源模型。

同一实例公共默认：

| 调用类型 | 默认调用规则 |
| --- | --- |
| `create` | RoDAQ 可与其它 instance create 并发；同一 `instance` 只有一个 owner |
| mutable control calls | 同一实例 RoDAQ 串行；一个未返回 mutable call 存在时不启动冲突 mutable call |
| role-specific data callbacks | 由 capability 明确声明最大并发 |
| Storage mutable operations | 单逻辑 lane |
| immutable descriptor/query | library-level thread-safe，可并发 |
| extension query | library-level thread-safe，可并发 |
| `destroy` | 仅 Quiescent instance，RoDAQ 保证与该 instance 的其它调用不并发 |

插件不得把未声明的 library-global mutex/queue/thread pool 当成资源和并发语义的隐藏前提。如果确实需要 DSO-wide serialization，V1 视为不受支持的插件能力，不能要求 RoDAQ 猜测。

**Reentrancy。**

默认禁止隐式重入。插件在 RoDAQ→Plugin 调用期间调用 RoDAQ callback 时，必须遵守该 callback 的逐函数规则；RoDAQ callback 默认不得同步反调同一 instance 的 mutable operation：

```text
RoDAQ -> Plugin mutable call
Plugin -> RoDAQ callback
RoDAQ -> same Plugin mutable call   // forbidden
```

插件不得持有会与 quiesce/control 路径互斥的全局锁去调用 RoDAQ callback。需要重入的未来能力必须作为独立 protocol 明确设计和资格验证。

**Deadline、Timeout 与 Late Return。**

ABI 中的 deadline 是 operation 的绝对 RoDAQ monotonic deadline，不是“RoDAQ 到点就能取消同步函数”。必须区分：

1. plugin 自己观察 deadline 并**返回** `DEADLINE_EXCEEDED`：调用已经结束；
2. RoDAQ watchdog 到期但 plugin 函数仍未返回：状态是 `TimedOutButInFlight`，调用仍拥有 instance、输入借用/retained payload、execution slot 和其它可能被访问资源。

第二种情况下 RoDAQ 立即停止启动与该调用冲突的新 mutable operation，并关闭/保护对应 Pipeline 入场，但不得 destroy instance、释放可能仍被访问的 buffer、归还执行容量或把 timeout 当成 quiescence。迟返结果仍归原 `work_id/instance/generation`，只能按原工作结算；它不能推进已经被新控制代次接管的 Task。若迟返长期不发生，按系统定义的局部清理期限升级为进程级恢复，而不是在同进程中强行取消未知第三方代码。

**Quiescence。**

Quiescence 是销毁安全条件，不是“调用了 stop”。成功返回必须同时证明：没有未返回 RoDAQ→Plugin call、没有未返回 Plugin→RoDAQ callback、插件后台任务不再访问 instance/RoDAQ service state、Payload/Buffer 不再被插件访问、role-specific I/O ownership 已结束。`quiesce()` 必须幂等，并允许在此前 operation 已失败后、或一个 RoDAQ-watchdog 超时调用**最终迟返后**继续收敛。

如果某个 same-instance mutable ABI call 在 watchdog 到期后仍实际执行，RoDAQ 只登记 quiesce pending，不与它并发调用 quiesce；等旧调用迟返后再进入 quiesce。旧调用永久不返时无法通过同进程 quiesce 获得静止证明，达到局部清理期限后升级整进程恢复。`quiesce` 自身 timeout/RoDAQ watchdog 到期时 instance、library、buffer 和 execution capacity 均保持原 owner；只有最终证明 Quiescent 或通过整进程终止消除全部访问可能性后才能回收这些资源。

## 4. 装载、故障与角色边界

### 4.1 装载、依赖与打包

**Package Admission 与 Library Lifetime。**

Loader 的顺序固定为：

```text
scan managed package roots
→ parse bounded manifest.json
→ canonicalize package/artifact/schema paths
→ verify owner/permission + no symlink/path escape
→ verify OS/architecture + Deployment allow-list
→ verify artifact/schema digests + dependency policy
→ resolve duplicate plugin_id
→ dlopen exact artifact with RTLD_NOW | RTLD_LOCAL
→ dlsym fixed bootstrap symbol
→ verify bootstrap symbol provenance
→ call descriptor entry
→ validate Common ABI / RoDAQ feature floor / role protocol
→ cross-check runtime descriptor against manifest
→ register immutable Catalog entry
```

`dlopen()` 可能执行 ELF constructor/C++ static initializer，因此 plugin identity、artifact digest、路径、权限和 allow-list 必须在它之前由 sidecar manifest 决定。V1 使用 `RTLD_NOW | RTLD_LOCAL`；禁止 `RTLD_GLOBAL`、`RTLD_DEEPBIND` 和 `dlmopen` namespace。成功装载后的 library handle 保留到 RoDAQ daemon 退出，不执行运行期 `dlclose()`；插件升级通过整进程重启生效。

`dlsym(handle, "rodaq_plugin_get_descriptor")` 成功还不够。RoDAQ 必须用 `dladdr()` 或等价机制验证 bootstrap function 的 defining object canonical path 就是当前候选主 DSO，不能接受从 dependency closure 偶然解析到的同名 symbol。**所有由 descriptor 返回的 Role/Extension ABI function pointer 也必须解析到同一主 DSO**；第三方依赖只能由插件内部 wrapper 调用，不能把依赖库函数直接暴露成 RoDAQ ABI operation。这样 provenance 规则单一、可测试，也避免依赖升级后无意改变 ABI owner。CI 同时用 ELF symbol table 证明主 DSO 只导出允许的 RoDAQ bootstrap/必要平台 symbol。

**Dependency Coexistence。**

C ABI 稳定不等于进程内依赖一定兼容。Manifest 与 qualification 必须区分：

- process-shared dependency：如目标系统 libstdc++、ROS 2/RMW/DDS、OpenSSL、protobuf/grpc 等可能与 RoDAQ/其它插件共享的 SONAME；
- plugin-private dependency：只为当前 package 使用且经过符号可见性/共存资格验证的依赖。

插件私有依赖使用受管 package-local RUNPATH（优先 `$ORIGIN` 相对路径）解析，不依赖当前工作目录，不要求 daemon 在运行时修改 `LD_LIBRARY_PATH`。Deployment/package root 及 DSO/schema/dependency 文件不得由非授权普通用户写入。发布支持矩阵至少记录 critical SONAME、exact version/build identity、symbol visibility、C++ runtime/allocator、TLS/OpenSSL、compression、ROS 2/DDS、protobuf/grpc 等共享依赖；未通过联合验证的组合在加载/Deployment admission 阶段 fail-closed。

**Process-global 行为限制。**

插件不得把以下进程级副作用作为正常初始化/运行机制：修改 signal handler、cwd、environment、locale、umask、process-wide terminate/new handler、RoDAQ logging backend 或全局 CPU affinity。DSO static initializer/constructor 不得启动后台线程、联网、打开业务设备、写文件或调用 RoDAQ；真正的运行资源只能在 capability/plan 准入后的 `create/prepare` 中建立并受实例生命周期管理。Plugin-owned mutable DSO singleton、全局业务 queue/thread/connection/device 默认禁止；无法满足这些规则的第三方库组合不得进入 V1 production support。

### 4.2 异常与进程级故障

同进程插件只能隔离**合作式故障**：结构化 error、声明范围内的 resource exhaustion、RoDAQ watchdog/late-return 所有权、可验证的 malformed value。不能承诺隔离 segmentation fault、非法指针/wild write、UB、heap corruption、process-global deadlock、无限分配或 signal misuse；这些属于 process-fatal plugin failure，由外部 supervisor 重启 daemon。Conformance 对这类破坏性样例必须使用独立子进程，不能把“RoDAQ 在同地址空间抵抗任意插件内存破坏”写成验收承诺。

**Plugin Contract Violation。**

以下事实不是普通业务失败，而是插件违反已发布契约：未知/不可能 result code、invalid enum、short/oversized contradictory struct、manifest/descriptor identity mismatch、capability/resource fingerprint 不一致、实际 callback concurrency 超声明、callback after successful quiesce、Resource Plan HWM 超界、required output 未按规则初始化等。RoDAQ 必须 fail-closed：关闭受影响 Pipeline 的新入场，保留仍在途 owner，停止对该 instance 发起新的非清理 mutable work，记录 bounded contract-failure context through RoDAQ logging，并将 instance/plugin 标记 unhealthy。若违例可能已经破坏进程级不变量或无法安全收敛，则升级为整进程恢复；不得无限 retry 同一 contract violation。

**SDK 与插件作者工具。**

Plugin SDK/交付必须同时包含 self-contained C ABI headers、C++17 authoring headers、package/manifest/schema specification、C 与 C++ 两套最小 Acquisition/Storage 示例、RoDAQ-side conformance harness、compatibility matrix，以及统一 CMake authoring helper `rodaq_add_plugin()`。该 helper 负责 hidden-by-default visibility、唯一 bootstrap export/linker script、package-local RUNPATH policy、manifest/schema validation、artifact digest/build identity 生成、post-build `nm/readelf` export gate 和可复用 ABI test fixture；插件作者不应在每个 backend 中手工复制这些规则。

公共 ABI 头必须能由 C11/C17 编译器独立包含，不依赖 C++、ROS 2、DDS、MCAP、JSON 库或其它第三方头。

### 4.3 ABI / Package Conformance 与兼容门禁

Plugin ABI Freeze 不是一次 `sizeof` 测试，而是 SDK、RoDAQ、package 和真实 DSO 的持续兼容门禁。至少覆盖：

| 类别 | 必须验证 |
| --- | --- |
| Header/Layout | C11/C17 C ABI headers 与 C++17 authoring headers 分别独立编译；amd64/arm64 `sizeof/align/offsetof` baseline；min prefix；old/new struct size；unknown tail；null/zero；canary；固定 enum/result 数值 |
| ABI Evolution | old RoDAQ + compatible new plugin；new RoDAQ + old plugin；Common/Role major mismatch；minor tail compatibility；RoDAQ feature floor 不满足必须拒绝 |
| Bootstrap | `bootstrap.h` 可独立 C11/C17 编译且不 include Common ABI；固定 `rodaq_plugin_get_descriptor` symbol 缺失/错误；unsupported Common major；同一 DSO 多次 probe 必须纯/幂等；malformed response/descriptor size；bootstrap symbol 从 dependency 解析必须拒绝 |
| Services | Config/Plugin/Clock/Logging/Acquisition service 的 wrong version、short prefix、NULL required table/handle/function；required feature 未在实例 services 中实现；create fail 后 callback；destroy 后 callback |
| Manifest/Package | malformed/oversized manifest；artifact/schema digest mismatch；wrong OS/arch；symlink/`..` escape；不安全可写 package；duplicate `plugin_id` 未唯一选择必须 fail-closed |
| Config Schema | 多 schema version；旧 schema 支持/不支持；schema digest；remote/file `$ref` 拒绝；超深/超大/复杂 schema 有界失败 |
| Lifecycle | create fail 保持 null；create success + prepare/start/open fail 后必须 quiesce；repeated quiesce；Quiescent destroy；destroy 不执行首次业务清理 |
| Timeout/Late Return | plugin 自返 DeadlineExceeded；RoDAQ watchdog 先到；late success/failure；永久阻塞；资源 owner 在返回/进程终止前不提前释放 |
| Threading | 多 instance 并发；declared callback concurrency；illegal same-instance concurrent mutable call；query/descriptor 并发；callback/quiesce race；forbidden reentrancy |
| Extension | unknown id；major mismatch；minor negotiation；required feature floor；library extension present 但 configured capability disabled |
| Resource | plan 超 profile；checked arithmetic overflow；actual buffer/thread/FD HWM；共享资源声明；allocation failure；HWM 超 plan 触发 contract violation |
| Error/Contract | unknown result/enum；contradictory struct；missing required output；exception boundary；ErrorBuffer truncation；callback after quiesce；fingerprint mismatch |
| ELF/Dependency | export allow-list；RUNPATH；missing/wrong SONAME；联合加载 ROS2+MCAP 首发组合；未验证依赖组合拒绝 |
| C++ Adapter | C++ Factory/Instance → C descriptor/table 的映射与直接 C 实现语义一致；`RODAQ_EXPORT_PLUGIN` 只导出固定 C bootstrap；Result/exception/ownership/late-return 转换完整；源码树外 C++ 插件可独立构建 |
| Long Run | 多 Task/多 instance 重复 create→prepare→operate→quiesce→destroy，RSS/thread/FD/TLS/内部 registry 无未解释单调增长 |

ASan/UBSan 用于 RoDAQ-side wrapper、SDK fixture 与首发插件；并发关键 fake/plugin 使用 TSan 或等价 race qualification。会故意触发 segfault、invalid pointer、wild write、abort、constructor crash 的 hostile fixture 必须在子进程运行，只验证进程级故障分类与 supervisor 恢复，不能要求同进程 RoDAQ 保持内存安全。

发布 ABI baseline 必须版本控制。普通 minor release 只能在允许的 tail/feature 规则内演进；CI 使用 ABI layout fixture、ELF export gate 和兼容样例阻止无意改动已冻结 prefix、数值和 symbol。

### 4.4 与角色文档的分工

| 职责项 | Plugin 公共文档 | Acquisition 文档 | Storage 文档 |
| --- | --- | --- | --- |
| ABI version/layout | 权威 | 引用 | 引用 |
| loader/library lifetime | 权威 | 引用 | 引用 |
| config schema mechanism | 权威 | 定义采集参数语义 | 定义存储参数语义 |
| capability mechanism | 权威 | 定义 Acquisition capability | 定义 Storage capability |
| resource plan mechanism | 权威 | 采集侧字段 | 存储侧字段 |
| thread/reentrancy baseline | 权威 | callback/quiesce 细化 | write/seal lane 细化 |
| Source/Stream/Record | 不定义 | 权威 | 消费 Descriptor/Record |
| Batch/Unknown/Segment | 不定义 | 不定义 | 权威 |

## 5. 开发实现设计

### 5.1 ABI 入口、布局与基础类型

**ABI 头文件分层。**

Plugin SDK 的公开头采用**单独 ABI 子目录 + C++ 根目录**：只有需要长期二进制稳定的 C ABI 放入 `abi/`，C++ authoring headers 直接位于 `include/rodaq/plugin/`，文件名只表达领域职责。

```text
include/rodaq/plugin/
  abi/
    bootstrap.h
    types.h
    config.h
    services.h
    plugin.h
    acquisition.h
    storage.h
  config.hpp
  services.hpp
  acquisition.hpp
  storage.hpp
  export.hpp
```

`abi/` 是唯一跨 DSO 的稳定 C ABI；`include/rodaq/plugin/` 根目录是插件作者使用的 C++17 source API。各文件职责和 include 方向冻结为：

| 文件 | 只负责 | 允许 include |
| --- | --- | --- |
| `abi/bootstrap.h` | 永久稳定的 bootstrap result/request/response/error buffer 与唯一入口；不依赖 Common ABI | `<stdint.h>`、`<stddef.h>` |
| `abi/types.h` | Common ABI header、result、string/bytes、error buffer、强类型 ID/digest、effective-config/environment fingerprint、deadline | `<stdint.h>`、`<stddef.h>` |
| `abi/config.h` | 只读 Config API | `types.h` |
| `abi/services.h` | Clock、Logging 与 Plugin Services | `types.h` |
| `abi/plugin.h` | Common ABI plugin descriptor、extension query、role metadata 与 create-time secret bundle | `bootstrap.h`、`types.h` |
| `abi/acquisition.h` | Acquisition capability/resource/instance configuration、role operations、role services | `plugin.h`、`config.h`、`services.h` |
| `abi/storage.h` | Storage capability/resource/instance configuration、Record/Batch/Segment role operations | `plugin.h`、`config.h`、`services.h` |

`config.hpp` 对应 `abi/config.h`，`services.hpp` 对应 `abi/services.h`；根目录下的 C++ headers 只依赖所需的同级 C++ header 与 `abi/` header。禁止通过一个 umbrella header 把所有插件接口全量引入。`abi/` 是唯一额外接口层次；C++ authoring API 直接使用 `rodaq/plugin/*.hpp`，不再设置 `api/`、`sdk/` 等中间目录。

公共 C 头必须可由 C11/C17 独立包含，不依赖 C++ 或第三方 header、不依赖 packing pragma，所有跨 ABI scalar 使用明确宽度类型。

V1 公共头系统依赖只允许 `<stdint.h>` 和 `<stddef.h>`，并统一提供 C++ `extern "C"` guard。Linux export 宏固定为 default visibility，普通符号由构建系统 hidden-by-default：

```c
#if defined(__cplusplus)
extern "C" {
#endif

#define RODAQ_PLUGIN_EXPORT __attribute__((visibility("default")))

/* ABI declarations */

#if defined(__cplusplus)
}  /* extern "C" */
#endif
```

`RODAQ_PLUGIN_EXPORT` 与 bootstrap function declaration 都归 `abi/bootstrap.h`；只有该 bootstrap symbol 使用 default visibility。function-table 中的函数由普通内部符号地址填入，不要求成为 dynamic export。Windows/macOS 不属于 V1 ABI support，不在该宏中提前加入未经资格验证的分支。

**Stable Bootstrap Entry。**

公共常量先冻结为：

```c
#define RODAQ_COMMON_ABI_MAJOR 1u
#define RODAQ_COMMON_ABI_MINOR 0u
#define RODAQ_ACQUISITION_PROTOCOL_MAJOR 1u
#define RODAQ_ACQUISITION_PROTOCOL_MINOR 0u
#define RODAQ_STORAGE_PROTOCOL_MAJOR 1u
#define RODAQ_STORAGE_PROTOCOL_MINOR 0u

typedef uint32_t rodaq_plugin_role_t;
enum {
    RODAQ_PLUGIN_ROLE_INVALID = 0,
    RODAQ_PLUGIN_ROLE_ACQUISITION = 1,
    RODAQ_PLUGIN_ROLE_STORAGE = 2
};
```

`abi/bootstrap.h` 是整个插件 ABI 的永久最小根，不 include `types.h`，只依赖 `<stdint.h>` / `<stddef.h>`。Bootstrap result、error buffer、request/response 都在该头中自行定义，不能复用 Common ABI 类型：

```c
#define RODAQ_PLUGIN_BOOTSTRAP_SYMBOL "rodaq_plugin_get_descriptor"

typedef uint32_t rodaq_bootstrap_result_t;
enum {
    RODAQ_BOOTSTRAP_OK = 0,
    RODAQ_BOOTSTRAP_INVALID_ARGUMENT = 1,
    RODAQ_BOOTSTRAP_UNSUPPORTED_ABI = 2,
    RODAQ_BOOTSTRAP_INTERNAL = 3
};

typedef struct rodaq_bootstrap_error_buffer {
    char* message;
    uint32_t capacity;
    uint32_t required_size;
} rodaq_bootstrap_error_buffer_t;

typedef struct rodaq_bootstrap_request {
    uint32_t struct_size;
    uint16_t requested_common_abi_major;
    uint16_t requested_common_abi_minor;
    uint32_t reserved0;
} rodaq_bootstrap_request_t;

typedef struct rodaq_bootstrap_response {
    uint32_t struct_size;
    uint16_t common_abi_major;
    uint16_t common_abi_minor;
    const void* descriptor;
    uint32_t descriptor_size;
    uint32_t reserved0;
} rodaq_bootstrap_response_t;

typedef rodaq_bootstrap_result_t (*rodaq_plugin_get_descriptor_fn_t)(
    const rodaq_bootstrap_request_t* request,
    rodaq_bootstrap_response_t* response,
    rodaq_bootstrap_error_buffer_t* error);

RODAQ_PLUGIN_EXPORT rodaq_bootstrap_result_t
rodaq_plugin_get_descriptor(
    const rodaq_bootstrap_request_t* request,
    rodaq_bootstrap_response_t* response,
    rodaq_bootstrap_error_buffer_t* error);
```

Bootstrap Foundation 定义 `RODAQ_BOOTSTRAP_REQUEST_BASE_SIZE` 与 `RODAQ_BOOTSTRAP_RESPONSE_BASE_SIZE`，两者的 required prefix 永久固定；`request == NULL`、`response == NULL` 或 request/response 小于对应 base size 时返回 `RODAQ_BOOTSTRAP_INVALID_ARGUMENT`，且 `reserved0` 必须写 0。`request` 和 `response` 都由 Loader 提供并先清零，`response.struct_size` 由 Loader 写入自身可接受大小；插件只能写该范围内的已定义字段。`RODAQ_BOOTSTRAP_OK` 时必须满足 `response.common_abi_major == request.requested_common_abi_major`、`response.common_abi_minor <= request.requested_common_abi_minor`、`descriptor != NULL` 且 `descriptor_size` 至少覆盖所选 Common ABI 的 descriptor base prefix。`UNSUPPORTED_ABI` 时 `descriptor == NULL` 且 `descriptor_size == 0`。其它失败同样不得发布 descriptor。

Bootstrap error buffer 可为 NULL；非 NULL 时只用于有界的人类诊断。`capacity > 0` 时 `message` 必须非 NULL 且 NUL-terminated；`message == NULL && capacity == 0` 合法。`required_size` 是完整 UTF-8 消息的字节数，不含 NUL。该诊断不参与版本选择或控制逻辑。Bootstrap Foundation 自身不携带 RoDAQ feature bits：feature floor 只有在成功选定 Common ABI、能够安全解释 Plugin Descriptor 后才检查。

`abi/plugin.h` 才进入 Common Plugin ABI：它 include `bootstrap.h` 与 `types.h`，定义 `rodaq_plugin_descriptor_t`、Role metadata 和 Extension Query。Common ABI major 的 descriptor layout 只允许在 bootstrap 返回成功并确认 major 后解释。Bootstrap Foundation 允许未来经过 manifest schema 明确声明的 DSO 为不同 Common ABI major 返回不同 immutable descriptor；V1 package 只声明并实现 Common ABI `1.x`，Loader 不猜测 manifest 未声明的其它 major。

运行时装载在 pre-dlopen package admission 之后执行：

```text
dlopen(RTLD_NOW | RTLD_LOCAL)
→ dlsym(rodaq_plugin_get_descriptor)
→ verify symbol provenance
→ call bootstrap for preferred Common ABI major/minor
→ if unsupported, probe another Loader-supported major if any
→ validate bootstrap response + opaque descriptor size
→ interpret descriptor only as negotiated Common ABI
→ validate descriptor prefix + RoDAQ feature floor
→ validate role protocol header/minimum table size
→ manifest/descriptor cross-check
→ register immutable Catalog entry
```

任何校验失败都发生在创建实例前；library handle 即使校验失败也只在不存在任何回调/实例/function pointer 使用时关闭，正常已注册插件则保持到 daemon 退出。

**ABI 公共结构头。**

所有可扩展 struct 以公共 header 开头：

```c
typedef struct rodaq_abi_header {
    uint32_t struct_size;
    uint16_t version_major;
    uint16_t version_minor;
} rodaq_abi_header_t;
```

规则：

- `struct_size` 是该内存对象实际可访问的字节数；
- 对 **caller-owned output struct**：caller 先清零整个缓冲，写入自己的 `struct_size` 和请求的 version；callee 只写 caller 提供范围与自身已知字段的交集；
- 对 **callee-owned static table/descriptor**：callee 填写自己实际结构的 `struct_size/version`，caller 只读取自己理解且在该 size 内的字段；
- 接收方只读到 `min(struct_size, known_size)`；
- caller-owned 输入/输出 struct 小于调用要求的 required prefix 时返回 `InvalidArgument`；callee-owned descriptor/table 小于 RoDAQ 所需 prefix 时由 Loader 判为 `Unavailable` 并拒绝注册；
- 未知 tail 必须忽略并保持 caller 预置的零值；
- 当前版本明确命名为 `reserved` 的输入字段/字节必须由 caller 写 0，callee-owned descriptor/table 的 reserved 也必须为 0；已知 reserved 非零视为 InvalidArgument 或 Plugin Contract Violation，不能提前私用；
- 输入 enum/token 的未知值返回 InvalidArgument；插件输出未知 enum/result 属于 Contract Violation；ABI boolean 使用 `uint8_t` 时只允许 0/1；
- callee 不得覆盖 caller buffer 的 canary/tail；
- 不允许通过 `sizeof(new_struct)` 直接拒绝旧兼容结构。

每个发布的 Common/Role/Extension protocol 都必须在头文件中提供稳定的 `MAJOR/MINOR` 常量。required prefix 常量按**语义**命名，不在标识符中编码版本，例如 `RODAQ_PLUGIN_DESCRIPTOR_BASE_SIZE`、`RODAQ_ACQUISITION_OPERATIONS_BASE_SIZE`、`RODAQ_STORAGE_OPERATIONS_BASE_SIZE`；后续新增能力若需要更长 prefix，使用对应 capability/feature 的语义名，而不是 `V2/V3` 或 `_1_1` 等版本后缀。字段可用性统一按 `struct_size >= offsetof(field)+sizeof(field)` 与协商 minor/feature floor 双重判断，不能只比较 `sizeof(current_type)`。 Role operation base prefix 内的函数指针全部 required 且 non-NULL；可选函数只能作为明确的 tail capability 出现，并同时受 negotiated protocol minor/feature 与 `struct_size` 约束。不得使用“NULL required function 表示当前配置不支持”来替代 Configured Capability。

所有 output pointer 在函数返回非 OK 时也必须遵守“**只写已声明有效的输出**”规则；role 文档逐函数说明哪些输出在错误返回时仍有效。未特别声明时，非 OK 返回只保证 error buffer，有效业务 output 不得被 caller 使用。

**`rodaq_abi_header_t` 版本语义矩阵。** `version_major/minor` 不允许由每个结构自行解释，V1 固定为：

| 对象类别 | header version 的含义 |
| --- | --- |
| plugin-owned static `PluginDescriptor` / Role operation table | provider 对该 Common/Role protocol **支持的最大兼容版本** |
| RoDAQ-injected `ConfigApi` / `PluginServices` / Clock/Logging table，以及 `SecretBundle` 等 Common call-scoped helper | 当前实例/调用已协商的 **Common Plugin ABI version** |
| RoDAQ-injected Role Services | 当前实例已协商的 **对应 Role Protocol version** |
| call-scoped Role input/output/value struct | 当前调用已协商的 **对应 Role Protocol version** |
| Extension table | `query_extension()` 本次实际协商出的 **Extension Protocol version** |

因此 Acquisition/Storage 的 capability、resource plan、instance config、Record/Batch/Segment、callback input/output 等带 header 的 value struct 都写 negotiated Role version，不写插件支持的最大 minor。旧 minor caller 只初始化自己理解的 prefix；新 callee 不得通过 value struct header 宣称更高 minor 后写越界 tail。

**外层 size 与内层 header 的单一权威。** Bootstrap/descriptor 等场景保留外层 byte size，是为了在安全读取对象 header 前先界定可访问范围；一旦 header 可安全读取，V1 要求：

```text
bootstrap_response.descriptor_size == descriptor.header.struct_size
descriptor.role_operations_size    == role_operations.header.struct_size
extension_table_size               == extension_table.header.struct_size
```

不相等即 Contract Violation，不定义“取较小值继续运行”的第二套语义。`descriptor.role_protocol_major/minor` 与 Role operation table header 也必须完全一致。caller-owned value/output struct 不额外再传一份重复 size；唯一 size authority 是其 `header.struct_size`。

V1 目标平台要求 C/C++ 的 `double` 为 IEEE-754 binary64；ABI conformance 在 amd64/arm64 构建时静态/运行期验证该前提。

**基础 ABI 类型。**

字符串：

```c
typedef struct rodaq_string {
    const char* data;
    uint64_t size;
} rodaq_string_t;
```

字节：

```c
typedef struct rodaq_bytes {
    const uint8_t* data;
    uint64_t size;
} rodaq_bytes_t;
```

二者都是 borrowed input/view，除非具体字段另有 lifetime 声明。公共规则固定为：`data == NULL && size == 0` 表示空值并合法；`data == NULL && size > 0` 非法；非空 `rodaq_string_t` 必须是有效 UTF-8、不要求 NUL 终止且不得包含嵌入 NUL；`rodaq_bytes_t` 可以包含任意字节。每个调用仍必须对字段施加更严格的长度/是否允许空值约束，RoDAQ/Plugin 不得用 `strlen()` 推断 ABI string 长度。

稳定实体标识统一使用 128-bit 二进制表示；sequence/generation 使用 `uint64_t`；内容摘要使用固定 256-bit 表示。`abi/types.h` 不只声明一个可互换的通用 ID，而是冻结以下**语义独立 C struct**：

```c
typedef struct rodaq_task_id {
    uint64_t high;
    uint64_t low;
} rodaq_task_id_t;

typedef struct rodaq_pipeline_id {
    uint64_t high;
    uint64_t low;
} rodaq_pipeline_id_t;

typedef struct rodaq_stream_id {
    uint64_t high;
    uint64_t low;
} rodaq_stream_id_t;

typedef struct rodaq_descriptor_id {
    uint64_t high;
    uint64_t low;
} rodaq_descriptor_id_t;

typedef struct rodaq_instance_id {
    uint64_t high;
    uint64_t low;
} rodaq_instance_id_t;

typedef struct rodaq_segment_id {
    uint64_t high;
    uint64_t low;
} rodaq_segment_id_t;

typedef struct rodaq_operation_id {
    uint64_t high;
    uint64_t low;
} rodaq_operation_id_t;

typedef struct rodaq_system_boot_id {
    uint64_t high;
    uint64_t low;
} rodaq_system_boot_id_t;

typedef struct rodaq_source_id {
    uint64_t high;
    uint64_t low;
} rodaq_source_id_t;

typedef struct rodaq_source_session_id {
    uint64_t high;
    uint64_t low;
} rodaq_source_session_id_t;

typedef struct rodaq_digest256 {
    uint8_t bytes[32];
} rodaq_digest256_t;

typedef struct rodaq_effective_config_id {
    uint8_t bytes[32];
} rodaq_effective_config_id_t;

typedef struct rodaq_environment_fingerprint {
    uint8_t bytes[32];
} rodaq_environment_fingerprint_t;
```

这些 ID 即使 layout 相同也不是 C typedef alias，避免不同领域 ID 在调用点静默互换。V1 对所有 128-bit ID 冻结 `sizeof == 16`、`alignof == alignof(uint64_t)`、`offsetof(high)==0`、`offsetof(low)==8`；三个 256-bit 类型冻结 `sizeof == 32`。新增实体 ID 必须新增独立 struct，不能退化成 `typedef rodaq_task_id_t ...`。

持久 JSON 中 128-bit ID 使用固定 32 个小写十六进制字符（high 后 low、无前缀）；256-bit digest 使用 64 个小写十六进制字符。全零实体 ID 作为 invalid/sentinel，不分配给正常对象。ID 的生成/来源由领域决定：Task/Pipeline/Stream/Instance/Segment/Operation 使用项目的碰撞不可接受生成器，System Boot 和 Source identity 可以来自经过验证的系统/协议事实。

Deadline 使用 RoDAQ monotonic clock 的绝对值：

```c
typedef struct rodaq_deadline {
    uint64_t monotonic_ns;
} rodaq_deadline_t;
```

`monotonic_ns` 与 Clock Services 提供的 monotonic 样本属于同一进程/boot 时间域。插件不能把它解释为 realtime，也不能在内部为每个子步骤重新计算新的完整 timeout。

V1 插件 ABI 支持目标为 64-bit Linux amd64/arm64。ABI 的**字段语义和布局规则**在两种架构保持一致，但动态库本身仍是架构专用产物；不承诺同一个 `.so` 跨架构加载。所有持久/协议数值仍使用固定宽度类型，不依赖 LP64 的 `long` 或 `size_t`。

**Result Code。**

公共 ABI 结果码固定使用 `uint32_t`，匿名 enum 只提供常量：

```c
typedef uint32_t rodaq_result_code_t;

enum {
    RODAQ_RESULT_OK = 0,
    RODAQ_RESULT_INVALID_ARGUMENT = 1,
    RODAQ_RESULT_INVALID_STATE = 2,
    RODAQ_RESULT_NOT_FOUND = 3,
    RODAQ_RESULT_CONFLICT = 4,
    RODAQ_RESULT_RESOURCE_EXHAUSTED = 5,
    RODAQ_RESULT_DEADLINE_EXCEEDED = 6,
    RODAQ_RESULT_UNAVAILABLE = 7,
    RODAQ_RESULT_PERMISSION_DENIED = 8,
    RODAQ_RESULT_DATA_LOSS = 9,
    RODAQ_RESULT_INTERNAL = 10
};
```

结果字段始终使用固定宽度 `rodaq_result_code_t`；上面的匿名枚举只提供编译期常量，避免把 C enum 的实现相关宽度变成 ABI。数值发布后不重排。

角色级 Unknown/PersistenceUnknown 不靠新增随意字符串表达，而由 Storage role result struct 显式编码。

**Error detail 输出。**

失败诊断采用 caller-owned buffer：

```c
typedef struct rodaq_error_buffer {
    char* message;
    uint64_t capacity;
    uint64_t required_size;
    uint8_t truncated;
    uint8_t reserved[7];
} rodaq_error_buffer_t;
```

约束：

- plugin 不返回要求 RoDAQ `free()` 的字符串；
- `capacity` 包含结尾 NUL 的空间；
- `required_size` 是完整 UTF-8 error detail 的字节数，不含结尾 NUL；
- `capacity > 0` 时 `message` 始终 NUL-terminated；
- 实际复制字节数为 `min(required_size, capacity - 1)`；
- 未完整复制时 `truncated = 1`；
- `message == NULL && capacity == 0` 对纯查询/验证类调用可用于 required-size probe；对 create/prepare/start/write/seal/quiesce 等可能有副作用或不可安全重放的调用，RoDAQ 不得为了取得完整 error detail 而重试原操作，正常使用预分配有界 buffer，`required_size` 仅用于截断统计；
- error detail 不参与程序控制逻辑。

### 5.2 C++ 插件开发接口

C ABI 是稳定二进制契约，但**不是要求插件作者直接使用的主要开发接口**。SDK 在同一组 C ABI 之上提供 C++17 authoring API；官方 ROS 2、MQTT、MCAP、SQLite3 插件优先使用 C++ API 实现，同时 conformance fixture 保留直接 C ABI 插件以验证底层契约。

**公开头文件。** C++ authoring API 直接放在 `include/rodaq/plugin/`：

```text
include/rodaq/plugin/
  config.hpp
  services.hpp
  acquisition.hpp
  storage.hpp
  export.hpp
```

`config.hpp` 只包装 C Config API；`services.hpp` 提供 Clock/Logging/Services；`acquisition.hpp`、`storage.hpp` 定义对应 Factory/Instance 与角色值类型；`export.hpp` 只提供 C++→C adapter/export glue。C++ 文件不再使用宽泛的 `plugin.hpp`。文件名不携带发布版本；协议版本只保存在常量和运行时字段中。

C++ 公共命名空间固定为：

```cpp
namespace rodaq::plugin {
// ...
}
```

C++ API 直接复用项目 `rodaq::Status` 与 `rodaq::Result<T>`；类型使用 PascalCase，函数使用项目统一的 lowercase_snake_case。C++ authoring 层可以使用 `std::string_view`、`std::unique_ptr` 和 RAII，但 adapter 负责把这些对象完全终止在插件 DSO 内，RoDAQ 永远只观察 C ABI 类型。

**公共 C++ 基础对象。**

`config.hpp` 与 `services.hpp` 至少提供以下窄包装：

```cpp
namespace rodaq::plugin {

class Config final {
public:
    Config() = delete;

    [[nodiscard]] Result<ConfigType> type() const;
    [[nodiscard]] Result<bool> contains(std::string_view key) const;
    [[nodiscard]] Result<Config> member(std::string_view key) const;
    [[nodiscard]] Result<Config> element(std::uint32_t index) const;
    [[nodiscard]] Result<std::uint32_t> element_count() const;

    [[nodiscard]] Result<bool> as_bool() const;
    [[nodiscard]] Result<std::int64_t> as_i64() const;
    [[nodiscard]] Result<std::uint64_t> as_u64() const;
    [[nodiscard]] Result<double> as_f64() const;
    [[nodiscard]] Result<std::string_view> as_string() const;

private:
    friend class ExportAdapter;
    Config(const rodaq_config_api_t* api, rodaq_config_node_t node) noexcept;

    const rodaq_config_api_t* api_;
    rodaq_config_node_t node_;
};

class Deadline final {
public:
    [[nodiscard]] std::uint64_t monotonic_ns() const noexcept;
};

class Services final {
public:
    [[nodiscard]] Result<ClockSample> clock_sample() const;
    [[nodiscard]] bool log_enabled(LogLevel level, std::string_view category) const noexcept;
    void log(LogRecord record) const noexcept;
};

class AcquisitionServices final {
public:
    [[nodiscard]] Result<SubmitResult> submit(RecordAttempt record) const;
    [[nodiscard]] Result<DescriptorToken> propose_descriptor(
        DescriptorProposal proposal, Deadline deadline) const;
    [[nodiscard]] Result<void> report_source_event(SourceEvent event) const;
};

}  // namespace rodaq::plugin
```

这些 wrapper 是 borrowed façade，不取得 RoDAQ 侧数据所有权。`Config` 只能由 SDK `ExportAdapter` 通过 private constructor 从当前调用的 `config_api + node` 构造，插件作者不能自行制造任意 Config handle；其内部 node/string 不得跨当前配置调用保存。`Services`/`AcquisitionServices` 在 create 调用期间有效，create 失败后立即失效，create 成功后可由实例保存到 destroy 返回。wrapper 不隐藏底层错误，不通过 exception 表达正常失败。`Deadline` 只保存 RoDAQ monotonic absolute deadline，不提供隐式 `expired()`：插件需要判断到期时通过绑定的 `Services::clock_sample()` 取得同一时钟域样本后比较，禁止用插件自己的 `std::chrono::steady_clock` 猜测另一时钟域。

**Role Factory 与 Instance 分离。**

pre-instance 的配置校验、Capability、Resource Plan 与运行实例生命周期必须在 C++ API 中保持和 C ABI 相同的分离，不能为了写起来方便把所有方法塞进一个对象。Acquisition 形状固定为：

```cpp
namespace rodaq::plugin {

class Acquisition {
public:
    virtual ~Acquisition() noexcept = default;

    virtual Result<void> prepare(Deadline deadline) = 0;
    virtual Result<void> start(Deadline deadline) = 0;
    virtual Result<void> quiesce(Deadline deadline) = 0;
};

class AcquisitionFactory {
public:
    virtual ~AcquisitionFactory() noexcept = default;

    virtual Result<void> validate_config(Config config) const = 0;
    virtual Result<AcquisitionCapability> configured_capability(Config config) const = 0;
    virtual Result<AcquisitionResourcePlan> resource_plan(Config config) const = 0;
    virtual Result<std::unique_ptr<Acquisition>> create(
        Config config,
        const AcquisitionInstanceConfig& instance_config,
        Services services,
        AcquisitionServices acquisition_services) const = 0;
};

}  // namespace rodaq::plugin
```

Storage 使用完全相同的分层。`write_batch()` 是一个例外：C ABI 允许“操作失败 + 有效写入进度”同时成立，因此不能用互斥 alternative 的 `Result<StorageBatchResult>` 表达，必须使用显式 outcome：

```cpp
struct StorageWriteOutcome {
    Status status;
    StorageBatchResult batch_result;
};

class Storage {
public:
    virtual ~Storage() noexcept = default;

    virtual Result<void> prepare(Deadline deadline) = 0;
    virtual Result<void> prepare_descriptor(
        const StorageDescriptor& descriptor,
        Deadline deadline) = 0;
    virtual Result<void> open_segment(
        const SegmentSpec& segment,
        Deadline deadline) = 0;
    virtual StorageWriteOutcome write_batch(
        const StorageBatch& batch,
        Deadline deadline) = 0;
    virtual Result<SegmentSize> segment_size() = 0;
    virtual Result<SegmentSealResult> seal_segment(Deadline deadline) = 0;
    virtual Result<void> quiesce(Deadline deadline) = 0;
};

class StorageFactory {
public:
    virtual ~StorageFactory() noexcept = default;

    virtual Result<void> validate_config(Config config) const = 0;
    virtual Result<StorageCapability> configured_capability(Config config) const = 0;
    virtual Result<StorageResourcePlan> resource_plan(Config config) const = 0;
    virtual Result<std::unique_ptr<Storage>> create(
        Config config,
        const StorageInstanceConfig& instance_config,
        Services services) const = 0;
};
```

上面的类型形状是 C++ 开发接口契约；Record、Descriptor、Batch、Capability、Resource Plan 的具体字段仍分别以 Acquisition/Storage 文档为权威。C++ 层不得增加 C ABI 中不存在的生命周期状态或弱化 deadline、late-return、quiescence、resource accounting 语义。`StorageWriteOutcome.status` 一一映射 C ABI 函数返回码，`batch_result` 一一映射 `rodaq_storage_batch_result_t`；当 backend write 已开始时，即使 `status` 非 OK，`batch_result` 仍必须有效。`StorageBatch` 是 call-scoped non-owning view，`const&` 只避免复制 wrapper，不改变底层 records/payload 的借用期限。

**自动 C ABI Adapter。**

插件作者实现 Factory/Instance 后，只需要一个统一导出声明：

```cpp
#include <rodaq/plugin/export.hpp>

RODAQ_EXPORT_PLUGIN(MyAcquisitionFactory)
```

`RODAQ_EXPORT_PLUGIN` 不导出 C++ class symbol；它只在插件 DSO 内实例化 SDK adapter，并生成唯一的：

```text
rodaq_plugin_get_descriptor
```

adapter 负责：

- 构造 immutable C descriptor 与 Role operation table；
- C `Config API` → C++ `Config`；
- C per-call Config API + instance configuration + typed Plugin Services → C++ typed configuration/RAII wrapper；
- 普通 `Result<T>` → C result/output struct；Storage `StorageWriteOutcome` → 函数 result code + `BatchResult` 双通道；
- C++ exception containment；
- `std::bad_alloc` → `ResourceExhausted`；
- instance pointer 与 `std::unique_ptr` ownership 转换；
- quiesce/destroy 的最终 owner 证明；
- required output 清零、`struct_size`、enum/result validation。

宏本身只负责声明导出入口和绑定 Factory 类型，不承载业务逻辑；所有转换实现由 SDK 统一提供，避免每个插件复制一套 C trampoline。V1 adapter **不保存 DSO-lifetime Factory singleton**：每次 `validate_config/get_configured_capability/get_resource_plan/create` trampoline 都在调用栈内构造一个短生命周期 Factory façade，调用一个 `const` 方法后立即销毁；`create()` 返回的运行状态只存在于新建的 Acquisition/Storage instance。Factory 必须可无参数构造，构造/析构 `noexcept`-equivalent、bounded、无 I/O、无线程、无业务资源、无跨调用 mutable state。这样 C++ authoring 模型与没有 factory handle/context 的 C ABI 完全一致，也不违反 DSO-global mutable state policy。

`plugin_id`、role、plugin version、schema catalog、build identity 等 metadata 不在 C++ Factory 中重复手写。`rodaq_add_plugin()` 以同一组 CMake 参数生成 package manifest 和供 `export.hpp` 使用的 generated metadata，adapter 再据此构造 C descriptor；这样 manifest 与 runtime descriptor 只有一个构建期事实来源，避免插件作者在 CMake、JSON 和 C++ 三处维护相同身份信息。

**C++ ABI 边界。**

C++ authoring API 是**源码接口，不是跨 DSO ABI**。RoDAQ 不能 `dynamic_cast` 插件对象，不能接收 `std::unique_ptr<Acquisition>`，也不能调用插件类虚表；这些 C++ 对象只存在于插件 DSO 内，由 adapter 从稳定 C function table 调用。由此允许官方插件使用自然的 C++17 设计，同时不把 compiler、libstdc++、STL layout、RTTI 或 exception ABI 变成 RoDAQ 的二进制兼容承诺。

SDK adapter 应尽量 header-only 或静态链接进插件 artifact，避免插件运行时再依赖一个带 C++ ABI 的 `librodaq_plugin_sdk.so`。若未来确有共享实现库，也只能暴露 C ABI 或作为与应用严格同版本部署、且不属于插件兼容承诺的内部依赖。 C++ Instance/Factory destructor 明确 `noexcept`；若实现违反该契约导致 `std::terminate()`，`catch (...)` 无法恢复，按进程级插件缺陷处理。adapter 的 exception containment 只覆盖正常 C++ 调用边界上可捕获的异常，不能宣称隔离 terminate/segfault/UB。

**两条开发路径。**

- 官方和普通第三方插件：优先使用 C++ Factory/Instance API + `RODAQ_EXPORT_PLUGIN`；
- 需要其它语言绑定、极小运行时或自定义工具链的插件：可直接实现 C ABI，但必须通过同一 package/ABI conformance gate。

两条路径最终生成完全相同的 Plugin Package、manifest、descriptor 和 Role Protocol，因此 RoDAQ 不区分插件是由 C 还是 C++ 实现。

### 5.3 配置、Descriptor、Plugin Services 与实例

**只读配置 ABI。**

插件不直接接收 JSON library type，也不要求每个插件重复解析 JSON 文本。V1 固定使用 RoDAQ 提供的只读 Config API：

```c
typedef const struct rodaq_config_node* rodaq_config_node_t;
typedef uint32_t rodaq_config_type_t;

enum {
    RODAQ_CONFIG_NULL = 0,
    RODAQ_CONFIG_OBJECT = 1,
    RODAQ_CONFIG_ARRAY = 2,
    RODAQ_CONFIG_BOOL = 3,
    RODAQ_CONFIG_I64 = 4,
    RODAQ_CONFIG_U64 = 5,
    RODAQ_CONFIG_F64 = 6,
    RODAQ_CONFIG_STRING = 7
};

typedef struct rodaq_config_api {
    rodaq_abi_header_t header;

    rodaq_result_code_t (*get_type)(
        rodaq_config_node_t node,
        rodaq_config_type_t* type);

    rodaq_result_code_t (*object_get)(
        rodaq_config_node_t object,
        rodaq_string_t key,
        uint8_t* found,
        rodaq_config_node_t* member);

    rodaq_result_code_t (*array_size)(
        rodaq_config_node_t array,
        uint32_t* element_count);

    rodaq_result_code_t (*array_get)(
        rodaq_config_node_t array,
        uint32_t index,
        rodaq_config_node_t* element);

    rodaq_result_code_t (*get_bool)(
        rodaq_config_node_t node,
        uint8_t* value);

    rodaq_result_code_t (*get_i64)(
        rodaq_config_node_t node,
        int64_t* value);

    rodaq_result_code_t (*get_u64)(
        rodaq_config_node_t node,
        uint64_t* value);

    rodaq_result_code_t (*get_f64)(
        rodaq_config_node_t node,
        double* value);

    rodaq_result_code_t (*get_string)(
        rodaq_config_node_t node,
        rodaq_string_t* value);
} rodaq_config_api_t;
```

Config API 的行为也属于 ABI：

- `rodaq_config_node_t` 只在**当前接收该 config node 的 ABI 调用期间**有效，包括 validate/capability/resource-plan/create；plugin 不得把 node/string 交给后台线程或跨调用保存；
- Config API 只读、bounded，不执行网络/文件 I/O，也不反调插件；同一 ABI 调用内可重复读取；
- `object_get()` 在 key 不存在时返回 `OK + found=0 + member=NULL`，存在时返回 `OK + found=1`；非 object、非法 key 或空结果指针返回 InvalidArgument；
- `array_get()` 越界返回 InvalidArgument 并把 `element` 置空；`array_size()` 返回 `element_count`，且配置边界保证它可表示为 `uint32_t`；
- `get_bool()` 的 `value` 只允许 `0/1`；所有 `get_*` 类型不匹配返回 InvalidArgument，失败时对应结果值不可使用；
- `get_string()` 返回的 string value 同样只在当前 ABI 调用期间 borrowed；
- schema validation 已由 RoDAQ 执行，plugin 再做无副作用 semantic validation。

`rodaq_config_api_t::header.version_major/minor` 使用已协商的 Common Plugin ABI version，V1 required prefix 中所有 getter 都必须非 NULL；头文件定义 `RODAQ_CONFIG_API_BASE_SIZE`。Config API 本身不携带 instance state，也不允许插件保存其 node/string 到当前 ABI 调用之外。Role `create()` 与 validate/capability/plan 一样显式接收 `config_api + config`，而不是从 `PluginServices` 隐式取得配置访问能力。

这样可以保持严格 JSON 作为外部格式，同时不把 JSON 库和 parser 复制进每个插件 ABI。

**Plugin Descriptor 结构。**

V1 descriptor required prefix 固定为：

```c
struct rodaq_plugin_descriptor {
    rodaq_abi_header_t header;              /* Common Plugin ABI */

    rodaq_string_t plugin_id;
    rodaq_string_t plugin_version;          /* SemVer text */
    rodaq_string_t build_id;
    uint32_t role;
    uint32_t reserved0;

    uint64_t required_rodaq_features_0;
    uint64_t required_rodaq_features_1;

    uint16_t role_protocol_major;
    uint16_t role_protocol_minor;
    uint32_t role_operations_size;
    const void* role_operations;

    rodaq_digest256_t config_schema_catalog_digest;

    rodaq_result_code_t (*query_extension)(
        rodaq_string_t extension_id,
        uint16_t requested_major,
        uint16_t requested_minor,
        const void** extension_table,
        uint32_t* extension_table_size,
        rodaq_error_buffer_t* error);
};
```

Descriptor 和 operation/extension table 都是 library-lifetime immutable。`role_operations` 是公共 descriptor 中**刻意保留的 discriminated opaque pointer**：唯一判别字段是 `role`，Loader 在注册阶段立即把它转换为对应 `rodaq_acquisition_operations_t` 或 `rodaq_storage_operations_t` 并完成 version/size/function validation，业务代码不得继续持有未经类型化的 `void*`。这里的类型擦除用于避免 `plugin.h` 反向依赖所有 Role headers，与被禁止的 generic `role_services/context` 不同。RoDAQ 同时检查 role protocol major/minor、`role_operations_size` 与 table 自身 header，二者矛盾视为 Contract Violation。配置 schema 内容来自已在 `dlopen` 前验证的 package schema catalog；descriptor 只携带 catalog digest 用于 cross-check，避免在 DSO 与 sidecar 两处维护两份可漂移 schema bytes。

**Plugin Services。**

RoDAQ 向插件注入的能力按职责拆成 typed service table，不提供把所有能力和状态聚合进单一 opaque context 的总对象，也不使用 `void* role_services` 做运行时服务类型擦除。公共定义位于 `abi/services.h`。每种 service table 都拥有自己的 typed opaque handle；插件只能把 handle 原样传回对应 table 的函数，不能解引用、转换或跨 service 混用。

```c
typedef uint32_t rodaq_log_level_t;
typedef struct rodaq_clock_sample rodaq_clock_sample_t;
typedef struct rodaq_log_record rodaq_log_record_t;

enum {
    RODAQ_LOG_TRACE = 0,
    RODAQ_LOG_DEBUG = 1,
    RODAQ_LOG_INFO = 2,
    RODAQ_LOG_WARN = 3,
    RODAQ_LOG_ERROR = 4,
    RODAQ_LOG_CRITICAL = 5
};

struct rodaq_clock_sample {
    rodaq_abi_header_t header;
    uint64_t monotonic_ns;
    int64_t realtime_ns;
    rodaq_system_boot_id_t system_boot_id;
    uint64_t clock_epoch_id;
};

struct rodaq_log_record {
    rodaq_abi_header_t header;
    rodaq_log_level_t level;
    rodaq_string_t category;
    rodaq_string_t message;
};

typedef struct rodaq_clock_services_handle* rodaq_clock_services_handle_t;
typedef struct rodaq_logging_services_handle* rodaq_logging_services_handle_t;

typedef struct rodaq_clock_services {
    rodaq_abi_header_t header;
    rodaq_clock_services_handle_t handle;

    rodaq_result_code_t (*get_sample)(
        rodaq_clock_services_handle_t handle,
        rodaq_clock_sample_t* sample);
} rodaq_clock_services_t;

typedef struct rodaq_logging_services {
    rodaq_abi_header_t header;
    rodaq_logging_services_handle_t handle;

    uint8_t (*enabled)(
        rodaq_logging_services_handle_t handle,
        rodaq_log_level_t level,
        rodaq_string_t category);

    void (*write)(
        rodaq_logging_services_handle_t handle,
        const rodaq_log_record_t* record);
} rodaq_logging_services_t;

typedef struct rodaq_plugin_services {
    rodaq_abi_header_t header;
    uint64_t feature_bits_0;
    uint64_t feature_bits_1;

    const rodaq_clock_services_t* clock;
    const rodaq_logging_services_t* logging;
} rodaq_plugin_services_t;
```

`rodaq_plugin_services_t` 只是不可变 runtime service directory，不携带统一 opaque state，也不包含 Config API。Clock 与 Logging 各自拥有独立 typed handle，避免把所有实现状态压入一个 `service_context`。未来确有新的跨角色公共能力时，必须先证明其无法由现有 Result/ErrorBuffer、Logging 或 role-specific interface 表达，再通过独立 typed service table 与 Common ABI 向后兼容 tail/feature bit 协商；不得预留没有明确用途的 service。

V1 中 `PluginServices`、Clock、Logging table 的 `header.version_major/minor` 都表示**已协商的 Common Plugin ABI**，不再创造独立 Service Protocol 版本域。Common ABI `1.0` 的两个基础 service pointer 均为 required、不得为 NULL；各 table 的 handle 与 base prefix 内所有 function pointer 也必须非 NULL。头文件提供 `RODAQ_PLUGIN_SERVICES_BASE_SIZE`、`RODAQ_CLOCK_SERVICES_BASE_SIZE`、`RODAQ_LOGGING_SERVICES_BASE_SIZE`。Loader/SDK adapter 在 create 前统一验证 required prefix、header version 和 required function pointer。

`feature_bits_0/1` 只表示**可选 Common ABI feature**，基础 Clock/Logging 不重复占 bit。Descriptor 的 `required_rodaq_features_*` 必须是当前实例 `PluginServices.feature_bits_*` 与 Common baseline 的子集；某个 optional feature 只有在 `feature bit + negotiated Common minor + required struct_size/function pointer` 同时满足时才可调用。`struct_size` 只证明内存字段可访问，feature bit 只证明语义获准，二者不能互相替代。Bootstrap Foundation 不参与 feature negotiation。

公共 clock/log 类型必须是固定宽度、无动态所有权的 ABI 结构。Clock 携带 monotonic/realtime、boot identity 与 epoch；Logging record 携带 level、category、message。字符串只在调用期间 borrowed，RoDAQ 需要保留时在自身有界存储中复制。

Logging 是插件向 RoDAQ 提供运行记录的唯一通用信息通道，面向 operator/developer，允许按预算 drop，不参与控制逻辑。正式调用结果仍必须通过 result code 与 ErrorBuffer 表达；不得要求 RoDAQ 解析日志文本判断调用成功、驱动状态或恢复动作。

公共 ABI 固定 `log category <= 128 bytes`、`log message <= 4096 bytes`，均要求有效 UTF-8 且不含嵌入 NUL；C++ authoring wrapper 在调用前执行同样检查。超限或 malformed 的 best-effort log 由 RoDAQ 丢弃并增加有界 contract counter。

插件不得创建独立的进程级 logging backend、sink、rotation、logging thread pool，也不得修改 RoDAQ logging backend。RoDAQ 在创建 Logging Services 时已经绑定 `plugin_id/role/task/instance` 等稳定上下文；插件只提交 level/category/message 等插件事实，时间戳和实例关联由 RoDAQ 补齐，避免各插件重复或伪造公共上下文。

所有 service callbacks 都必须逐函数冻结 owner/lifetime、允许线程域、并发、阻塞/分配上限、是否可能反调插件以及最大工作量。公共基线：Clock `get_sample()` thread-safe、bounded、无外部 I/O、不得反调插件；Logging `enabled()/write()` thread-safe、bounded，允许按预算 drop，且不得反调插件。

`rodaq_plugin_services_t`、其子 table/handle 以及 role-specific service table 在整个 `create()` 调用期间都有效。若 create 失败，插件必须在返回前停止使用且不得保存任何 service pointer/handle；若 create 成功，插件可以把所需 table/handle 保存到该 role instance，并依赖它们一直有效到 `destroy()` 返回。Config API 则是 per-call protocol：`config_api`、config node、配置数组和临时字符串只在当前 validate/capability/plan/create ABI 调用期间有效，插件必须在 create 返回前复制需要跨调用保存的普通配置值。只有 instance 已 Quiescent 且 destroy 返回后，RoDAQ 才能释放成功实例绑定的 callback target。

Role-specific callback 不放入公共 `PluginServices`。Acquisition 的 Record/Descriptor/Source callback 由 `rodaq_acquisition_services_t` 明确承载；Storage 当前没有必须的反向 callback，因此不创建空的 `StorageServices` 占位类型。V1 不向插件提供任意 filesystem、Task controller、Pipeline private object、global registry 或 mutable system container。

**Role Instance。**

Common Plugin ABI 不定义一个可在角色之间互换的 `rodaq_plugin_instance_t`。每个 Role Protocol 使用自己的 opaque instance 类型，例如：

```c
typedef struct rodaq_acquisition_instance* rodaq_acquisition_instance_t;
typedef struct rodaq_storage_instance* rodaq_storage_instance_t;
```

这样 Acquisition instance 不能误传给 Storage operation，role ownership 在 C ABI 层也保持强类型。实例布局完全私有于插件，RoDAQ 只保存 opaque pointer。create 的公共语义为：

```text
validated config + per-call Config API
+ immutable instance configuration/resource grant
+ Plugin Services / role services
+ expected_environment_fingerprint
→ role-specific instance
```

`create()` 成功是 instance ownership 从插件局部准备态交给 RoDAQ 的唯一边界；成功时 `instance` 必须非空，失败时必须保持空且插件自行清理尚未发布的 partial state。create 不启动未在 plan 中计费的后台资源，也不进行 role-specific 外部开放；它必须重新核对 `expected_environment_fingerprint`，防止 capability/resource-plan 与真正创建实例之间发生 TOCTOU。

destroy：

- 只允许在公共 quiescence 已证明完成后；
- 不返回业务 flush 成功；
- 不执行首次外部 shutdown/flush/seal/join；
- 不调用 RoDAQ callback；
- 必须 noexcept-equivalent 且有界完成；
- `destroy()` 是 `void` 且没有可靠错误通道，因此所有可失败清理必须在 `quiesce()` 完成并报告；destroy 实现本身不得抛异常。SDK C++ wrapper 在最外层仍防御性 `catch (...)` 以阻止异常越过 C ABI，但该情况属于插件实现缺陷并由 conformance 判失败，不能依赖 destroy 阶段再上报业务诊断。

### 5.4 Capability、Resource Plan 与 Extension

**Capability 调用。**

Plugin 能力必须明确区分三类事实，禁止用一个 `Capability` 对象同时承载静态支持、配置结果和运行状态：

1. **Static Protocol Support**：由 descriptor、Role Protocol operation table 与 Extension Query 表达，不另外创建 `DeclaredCapability` ABI 对象；
2. **Configured Capability**：由已验证配置与显式稳定环境事实计算出的实例前能力，作为 admission 输入；
3. **Runtime State**：连接状态、已发现 Source、当前吞吐、队列深度、last error 等运行事实，只进入 Acquisition/Storage Snapshot，不属于 Capability。

Capability 必须在**创建运行实例之前**完成，作为 Prepare 资源准入输入。角色 operation table 的 `validate_config / get_configured_capability / get_resource_plan` 都是 library-level pre-instance 调用：RoDAQ 可以为不同 Task 并发调用它们，插件必须线程安全且不得保存 config node；这些调用不得启动后台线程、建立业务连接、打开业务文件、修改全局状态或留下需要 quiesce 的资源。若某个能力必须通过真正连接设备/网络才能确定，它不是 pre-instance capability，应在 `prepare()` 后作为 Role Runtime State 暴露，而不是偷偷做探测。

```text
Static Protocol Support
+ validated config
+ explicit stable environment facts
→ Configured Capability
```

Configured Capability 与 Resource Plan 一起参与 Effective Pipeline Limits 计算；只有准入成功后才调用 create 创建实际运行实例。任何会改变 capability/resource plan 的稳定环境事实（例如实际 RMW、backend feature set、系统库能力）必须进入 capability/plan 的 fingerprint/identity 输出并参与 `effective_config_id`，不能作为未记录的隐藏输入。瞬时连接状态、当前设备在线与否、当前负载不进入 Configured Capability。

角色 capability 与 resource-plan 结构都必须包含同一 Common ABI `rodaq_environment_fingerprint_t`（固定 32 字节不透明摘要）。

它与 `rodaq_digest256_t` 字节宽度相同但语义不同，正式 ABI 使用独立 struct，防止内容摘要和环境摘要误传。fingerprint 算法固定为 SHA-256：输入以 ASCII domain separator `rodaq.environment\0` 开始，随后追加 `uint16 big-endian format_revision = 1`，再把插件声明的环境事实按 ASCII key 字典序排序，每项编码为 `uint16 big-endian key_length + key bytes + uint32 big-endian value_length + canonical value bytes`。key 必须使用插件命名空间且不重复；整数 canonical value 使用固定宽度 big-endian，字符串使用无 NUL 的 UTF-8，枚举使用发布后固定的数值或 ASCII token。

每个后端必须在其设计/manifest qualification metadata 中列出**影响 capability/resource-plan/create 有效性的完整环境事实集**并形成 golden test，例如 RMW 实现/版本、backend feature set、关键运行库能力等；任何这类事实变化必须改变 fingerprint。瞬时负载、CPU 使用率、当前空闲内存、secret plaintext 等不得进入 fingerprint。配置身份、plugin artifact/build identity 由 RoDAQ 在 Effective Configuration 中独立保存，不要求插件通过 Config API 重新构造一份配置 digest。Capability 与 Resource Plan 对同一次解析必须返回完全相同 fingerprint；create 按相同事实集重新计算并与 `expected_environment_fingerprint` 比较。

Capability struct 中的 variable-length 列表使用 caller-owned bounded output 或“两次调用：query count/required bytes → fill”，不返回插件堆容器。

**Resource Plan 调用。**

Resource Plan 的输入必须是已经通过 schema + semantic validation 的配置，并且**不得要求运行实例已经创建**。确定性以 `(plugin artifact/build identity, canonical config, explicit environment facts)` 为输入：相同输入必须产生语义相同的 capability、plan 与 `environment_fingerprint`；若 backend/RMW/系统库等环境变化影响能力或资源，fingerprint 必须变化并参与 `effective_config_id`。

Resource Plan 不允许通过“先创建完整后台线程再测实际占用”生成。标准顺序固定为：

```text
schema validation
→ plugin semantic validation
→ configured capability
→ resource plan
→ fingerprint equality check
→ Core/Profile admission
→ create(expected_environment_fingerprint)
→ role prepare
```

`create()` 环境复核失败时必须重新走 capability/plan/admission，不能在原 grant 上动态扩容。任何角色 ABI 都必须保持这一顺序。

**Extension ABI。**

Extension ID 使用与 `plugin_id` 相同的稳定 ASCII namespace 规则和 128-byte 上限，例如：

```text
rodaq.acquisition.payload_retain
rodaq.storage.verification
```

query 返回 extension header + versioned operation table，并遵循 §3.1 的 major/minor/feature-floor 协商。调用方只在对应 library lifetime 内保存 table pointer；配置是否启用某 extension 由 configured capability 决定，不能从 query 成功本身推断。

### 5.5 Loader、异常边界与线程安全规则

**装载器内部组成。**

V1 loader 逻辑对象固定为：

这些对象位于 `rodaq::plugin`，类型名不再重复 `Plugin` 前缀：

| 对象 | 职责 |
| --- | --- |
| `Manifest` | 严格解析/校验 `manifest.json`、schema catalog、artifact/dependency facts，不触碰 DSO |
| `Package` | canonical package root、artifact/schema 路径、owner/permission/digest 与 target facts |
| `Compatibility` | Deployment allow-list、RoDAQ feature floor、role protocol、依赖共存支持矩阵 |
| `SharedLibrary` | `dlopen/dlsym/dladdr/dlclose` 的窄 RAII；不解释业务 capability |
| `ContractValidator` | bootstrap/descriptor/table/manifest cross-check、symbol provenance、required prefix/feature 验证 |
| `Catalog` | 保存唯一且已完整验证的 `plugin_id → CatalogEntry` immutable entry |
| `InstanceFactory` | config semantic validation、capability/plan、admission 后创建 role instance |
| `AcquisitionHandle` / `StorageHandle` | C ABI 到 Core `Status/Result` 的窄 role wrapper 与调用 instrumentation |

`Catalog` 不扫描文件系统、不创建 Task；`SharedLibrary` 不解析 manifest/config；`InstanceFactory` 不负责 DSO discovery。发现同 `plugin_id` 的 package 冲突必须在 Catalog 注册前解决。

**Linux Loader Policy。**

V1 固定 `RTLD_NOW | RTLD_LOCAL`，不使用 `RTLD_GLOBAL/RTLD_DEEPBIND/dlmopen`。artifact 使用 package admission 得到的 canonical absolute path；Task 不提供路径。`dlsym` 后必须验证 bootstrap symbol provenance；所有失败都记录 bounded loader failure context。通过注册的 DSO 不在进程运行期间 `dlclose()`；只允许在 descriptor 校验失败且从未创建 instance、从未发布 function/table pointer、没有 constructor 留下受管资源的加载失败路径关闭句柄。若 constructor 已导致不可证明的进程级副作用，应按 contract violation/进程恢复处理，而不是假定 `dlclose()` 能逆转。

**符号与 ELF Hardening。**

插件编译 `hidden` by default，主 DSO RoDAQ export allow-list 只包含当前 bootstrap symbol。`rodaq_add_plugin()` 生成/使用 linker version script（或等价机制），CI/post-build gate 通过 `readelf/nm` 检查 exported dynamic symbol、RUNPATH/NEEDED、target architecture 和意外 C++ symbol。Bootstrap symbol provenance 运行期用 `dladdr()` 等价验证；不把依赖库偶然暴露的同名 symbol 当作插件入口。

**Exception Boundary。**

每个 C ABI entry 都采用统一 wrapper：

```cpp
try {
    return implementation_call(...);
} catch (const std::bad_alloc&) {
    return RODAQ_RESULT_RESOURCE_EXHAUSTED;
} catch (const std::exception& e) {
    write_bounded_error_detail(e.what());
    return RODAQ_RESULT_INTERNAL;
} catch (...) {
    write_bounded_error_detail("unknown exception");
    return RODAQ_RESULT_INTERNAL;
}
```

异常分类可以按项目策略更细，但任何 C++ exception 都不得穿越 C ABI。

**Threading 声明。**

Operation table 和 RoDAQ callback 文档必须逐函数标记：

```text
same-instance serialization
cross-instance concurrency
may run concurrently with role callback
may block / bounded-work class
operation deadline / RoDAQ watchdog semantics
borrowed inputs lifetime
outputs valid on each result class
late-return ownership
allowed RoDAQ callback / reentrancy
```

不能只在总说明写一句“thread-safe”。没有逐函数声明的 mutable function 采用最保守规则：同一实例串行、不得重入；library-level immutable query 则必须明确满足多实例并发读取。

**ABI Static Assertions。**

C++ RoDAQ-side wrapper 和插件 SDK test 对关键 ABI 类型执行：

```cpp
static_assert(std::is_standard_layout<T>::value, ...);
static_assert(std::is_trivially_copyable<T>::value, ...);
```

并验证：

- field offset、`sizeof`、`alignof` 与每个 `*_BASE_SIZE`；
- amd64/arm64 各自的 golden layout，两个目标的语义字段顺序/宽度必须一致；
- enum width strategy；
- pointer/null behavior；
- canary tail 不被越界写；
- concrete C++ Factory 满足 `std::is_nothrow_default_constructible_v<Factory>` 与 `std::is_nothrow_destructible_v<Factory>`，Instance/Factory destructor 满足 noexcept 契约。

C compiler test 负责证明头文件不偷偷依赖 C++。

**Role Wrapper。**

Core C++ 不应在业务代码到处直接调用 function pointer。

分别提供内部 wrapper：

```text
AcquisitionHandle
StorageHandle
```

`Handle` 表示 Core 持有的、已经通过 Catalog/Factory 创建并绑定到一个 C ABI instance 的调用句柄，不表示插件实现类本身。wrapper 负责：

- C ABI → `Status/Result` 的白名单映射，unknown result/enum 变成 Contract Violation；
- struct-size/version/feature-floor 初始化与校验；
- caller-owned error buffer；
- `work_id/instance/generation` owner token；
- deadline + RoDAQ watchdog + late-return instrumentation；
- same-instance serialization guard；
- resource/HWM/latency metrics；
- quiesce/destroy 前 owner proof。

Pipeline 只依赖这些项目 wrapper 的窄接口，而不是原始 C table。

**Test Seam。**

Plugin framework 测试必须生成/安装真实 package 与 DSO fixture，而不只是进程内手工 function table。Fixture 至少包括：unsupported bootstrap major、repeated bootstrap probe、malformed bootstrap response、old ABI minor、short/long struct、outer-size/header-size mismatch、descriptor/table version mismatch、NULL required service/function、wrong service/Common version、wrong role/protocol、RoDAQ feature floor/service realization mismatch、duplicate id、manifest/artifact mismatch、entry symbol 位于 dependency、wrong architecture、bad RUNPATH/dependency、exception implementation、prepare/start fail、RoDAQ-watchdog late return、permanent block、multi-instance race、callback after failed create/quiesce/destroy、payload release after Acquisition destroy、unknown result/enum、resource-plan overflow/HWM violation。Storage fixture 还必须覆盖“non-OK + valid BatchResult”、seal output capacity 在副作用前不足、backend 违反 file/path admission 上界三类边界。破坏进程内存的 hostile fixture 单独用 subprocess 测试。

ABI Freeze gate 不是“Markdown 审查通过”即结束：必须先把 `bootstrap.h/types.h/config.h/services.h/plugin.h/acquisition.h/storage.h` 按本文生成真实 header，逐个作为 translation unit 第一个项目头在 C11/C17/C++17 `-Werror` 编译；再生成 amd64/arm64 layout snapshot（`sizeof/alignof/offsetof/BASE_SIZE`）纳入版本控制。随后用一份直接 C 插件和一份 C++ Factory/Instance 插件执行同一 conformance vector，证明 status/output/ownership/minor-tail/late-return 语义等价后，才允许标记 ABI Frozen。

**代码组织约束。**

推荐逻辑文件组：

```text
include/rodaq/plugin/
  abi/
    bootstrap.h
    types.h
    config.h
    services.h
    plugin.h
    acquisition.h
    storage.h
  config.hpp
  services.hpp
  acquisition.hpp
  storage.hpp
  export.hpp

src/plugin/
  manifest.*
  package.*
  catalog.*
  shared_library.*
  contract_validator.*
  compatibility.*
  instance_factory.*
  acquisition_handle.*
  storage_handle.*

cmake/
  RoDAQPlugin.cmake
```

目录已经表达 `plugin` 领域，因此内部文件不再重复 `plugin_` 前缀。`manifest/package/catalog/contract_validator/compatibility` 都只承担一种明确职责；`shared_library` 保留完整词组，因为它表示 DSO handle RAII，而不是泛化的 `library`；`instance_factory` 明确表示“准入后创建 role instance”，避免宽泛的 `factory`；`acquisition_handle/storage_handle` 是 Core 侧已加载 C ABI instance 的窄包装，不与插件作者实现类混淆。

C ABI headers、C++ authoring headers 与 Core-side wrapper 分层，但都归属于语义明确的 `plugin` 模块，不使用 `sdk/`、`host/`、`common/`、`detail/` 这类宽泛目录。公共 plugin 目录不得引入具体 ROS 2/MCAP 头。`RoDAQPlugin.cmake` 是插件作者的统一构建入口，C/C++ 示例和源码树外 consumer test 都必须使用它。

**实现顺序。** 为避免先写 Loader 后反复改接口，开发顺序固定为：① 冻结 `abi/` 下 C headers、常量、命名规则和 layout fixture；② 落地 `include/rodaq/plugin/` 下 C++ authoring API，并用 fake Factory 验证 C++→C adapter；③ 实现 `manifest.schema.json`、Manifest/Package 与 package limits；④ 实现 SharedLibrary、ContractValidator、Compatibility、Catalog 和真实 DSO fixtures；⑤ 实现 Config/Plugin Services、AcquisitionHandle/StorageHandle、InstanceFactory 和 timeout/quiesce owner model；⑥ 完成 `rodaq_add_plugin()`、源码树外 C/C++ examples、ABI/export/package gates；⑦ 最后接入 ROS 2 Acquisition 和 MCAP Storage，并用同一 conformance harness 验证。前一阶段的 ABI/package gate 未通过时不得提前把后端业务代码作为“事实标准”反向修改公共契约。

### 5.6 Secret Handoff

可移植 Task/Plugin 配置只保存 credential reference，不保存 secret material。RoDAQ 在形成 Effective Recording Configuration 时解析引用并得到：

```text
credential logical name
credential version/fingerprint
secret items
```

公共 ABI 使用只读、调用期借用结构：

```c
typedef struct rodaq_secret_item {
    rodaq_string_t name;
    rodaq_bytes_t value;
} rodaq_secret_item_t;

typedef struct rodaq_credential_fingerprint {
    uint8_t bytes[32];
} rodaq_credential_fingerprint_t;

typedef struct rodaq_secret_bundle {
    rodaq_abi_header_t header;
    const rodaq_secret_item_t* items;
    uint32_t item_count;
    rodaq_credential_fingerprint_t credential_fingerprint;
} rodaq_secret_bundle_t;
```

规则：

- `rodaq_secret_bundle_t::header.version_major/minor` 写当前已协商 Common Plugin ABI version，V1 定义 `RODAQ_SECRET_BUNDLE_BASE_SIZE`；
- `items == NULL` 仅在 `item_count == 0` 时合法，item count/单项/总字节都受 Deployment 硬上限；
- `items/value` 只在 create 调用期间有效；
- plugin 若运行期/reconnect 仍需要凭据，必须在 create 内复制到自己的受控内存或交给已验证的 backend credential object；
- secret bytes 不进入普通日志、ErrorBuffer、Task export、Recovery Record、Segment Manifest、environment fingerprint；
- `effective_config_id` 只包含 credential reference 的稳定 version/fingerprint，不包含 secret plaintext；
- destroy/quiescent cleanup 对 plugin 自己保存的敏感缓冲执行能力范围内的 best-effort zeroization；不能宣称第三方库内部复制一定已被擦除，相关行为进入 backend qualification；
- secret item 数量和单项/总字节数由 Deployment 设置硬上限；
- V1 不允许插件自行根据 Task 参数打开任意 secret file path；文件/密钥库访问由 RoDAQ/Deployment 层完成。

`rodaq_secret_bundle_t` 的完整定义属于 `abi/plugin.h`，供各 Role instance config 通过只读指针引用。Acquisition/Storage 的 `instance_config` 都包含可空的 `const rodaq_secret_bundle_t*`。不需要凭据时传空；需要但引用无法解析时在实例创建前 Prepare 失败。

## 6. 不变量与完成条件

### 6.1 核心不变量

1. Stable bootstrap 始终使用固定 `rodaq_plugin_get_descriptor`；Bootstrap Foundation 不依赖 Common ABI 类型，Common ABI major 通过纯、可重复的 bootstrap probe 协商，已发布 bootstrap required prefix 永不改变。
2. Common ABI、Role Protocol、Extension、Config Schema、Plugin SemVer 各有独立版本域，不能互相替代。
3. 未通过 manifest/path/permission/digest/target/allow-list 校验的 artifact 不得进入 `dlopen()`。
4. Runtime descriptor 必须与 package manifest 身份一致；bootstrap symbol 必须由候选主 DSO 自身定义。
5. 稳定 ABI 不暴露 STL、JSON library、RTTI、exception 或 backend C++ 类型；所有 C 头可独立编译。
6. RoDAQ feature floor、required prefix 和 struct-size 三者共同决定字段/能力可用性；未声明能力不能由 RoDAQ 猜测。
7. Capability 与 Resource Plan 在实例创建前完成，使用同一 environment fingerprint；create 必须再次核对该 fingerprint。
8. Resource Plan 是有限硬上界；RoDAQ 使用 checked arithmetic，实际 HWM 超界属于 Contract Violation。
9. 一旦 create 成功，任何后续成功/失败路径都必须经过幂等 quiesce 后才能 destroy；create 失败不发布半实例。
10. RoDAQ watchdog timeout 不等于 ABI call 返回，更不等于 quiescence；late-return 前相关 owner/resource 不提前释放。
11. 同一 DSO 的不同 instance 默认可并发；插件不能依赖未声明的 DSO-wide serialization 或隐藏 process-global mutable state。
12. 所有跨 ABI owner/lifetime/release、逐函数 threading/reentrancy/deadline/late-return 语义显式。
13. library handle 长于所有 descriptor/table/function pointer、instance 与 callback；已注册插件 V1 不热卸载。
14. Plugin log text 与 ErrorBuffer message 不作为程序逻辑；unknown result/enum/contradictory output fail-closed 为 Contract Violation。
15. 同进程模型不承诺隔离非法指针、wild write、UB 或 process-global crash；此类故障只能通过测试子进程和外部 supervisor 恢复。
16. 未经 joint dependency qualification 的插件组合不能因 C ABI 稳定而自动视为可共存。

### 6.2 V1 完成条件

Plugin 公共框架只有在以下条件全部满足后才能进入 ABI Freeze/完整后端开发：

- `abi/bootstrap.h / abi/types.h / abi/config.h / abi/services.h / abi/plugin.h / abi/acquisition.h / abi/storage.h` 在 C11/C17 与 C++17、amd64/arm64 上独立编译，ABI baseline、数值和 required-prefix fixture 已入库；C ABI 公开 typedef 全部使用 `rodaq_*_t`，struct tag 不使用 `_t`，文件/导出符号/函数/类型均不编码发布版本；
- `config.hpp / services.hpp / acquisition.hpp / storage.hpp / export.hpp` 在源码树外以 C++17 独立消费；最小 Acquisition/Storage C++ 插件只实现 Factory/Instance 和 `RODAQ_EXPORT_PLUGIN`，不手写 C function table；
- 同一行为分别通过直接 C ABI fixture 与 C++ adapter fixture 执行，Result、生命周期、所有权、timeout/late-return 和输出结构语义一致；
- `rodaq_add_plugin()` 能在源码树外构建最小 C 与 C++ Acquisition/Storage package，并自动完成 manifest/schema/digest、visibility/export 和 RUNPATH gate；
- pre-dlopen package admission、duplicate-id fail-closed、artifact/schema digest、wrong target、path/symlink/permission 拒绝全部通过；
- Bootstrap Foundation 独立编译、unsupported/multi-major probe、old/new Common minor、RoDAQ feature floor/service realization、Role Protocol 与 Extension negotiation 的双向兼容测试通过；
- symbol provenance、export allow-list 与首发依赖组合的真实 `dlopen/dlsym` 测试通过；
- 多 schema version、Draft 7 bounded validation、unsupported old schema 与禁止 remote/file `$ref` 测试通过；
- capability/resource-plan fingerprint、checked arithmetic、create-time TOCTOU recheck 和 HWM violation 测试通过；
- create/prepare/start/open 任意失败、repeated quiesce、RoDAQ-watchdog late return、永久阻塞和 Quiescent destroy 的 owner 证明通过；
- 多 Task/多 instance 并发、callback/quiesce race、禁止重入和 callback-after-quiesce 的 contract tests 通过；
- unknown result/enum、malformed **value**、short/tail struct、ErrorBuffer truncation 和 plugin C++ exception 均 fail-closed；故意 invalid pointer/segfault/wild-write fixture 在子进程验证为 process-fatal，不宣称 RoDAQ 同进程隔离；
- ROS 2 Acquisition + MCAP Storage 联合依赖资格通过，未支持组合会在 Deployment/loader admission 前拒绝；
- ASan/UBSan、关键并发 TSan，以及多实例长时 create→prepare→operate→quiesce→destroy 循环中 RSS/thread/FD/TLS/内部 registry 无未解释增长。
