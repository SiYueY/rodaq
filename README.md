# RoDAQ

RoDAQ（Robot Data Acquisition）面向工业机器人数据采集、录制与管理。

当前正在重新收敛设计，**Notion 为设计文档的唯一当前基线**；仓库中的旧设计草稿、插件实现及对应测试已从 `next` 分支移除，后续以审核通过的新设计为依据分阶段重新实现。

- [当前 Notion 设计文档](https://verbose-vulture-f51.notion.site/RoDAQ-417da2b2a80f81c68d97cffe766805b3)
- [服务端模块](server/README.md) · [Web 前端模块](web/README.md)
- [C++17 代码规范](docs/coding_guidelines/cpp_coding_guidelines.md)
- [仓库协作约定](AGENTS.md)
- [Apache License 2.0](LICENSE)

当前保留最小 C++17 工程基础（Result、Status、Logging、构建和基础测试），**不代表插件等业务模块已实现**。
