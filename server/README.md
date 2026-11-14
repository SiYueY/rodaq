# RoDAQ Server

应用服务端位于仓库根目录。它装配 `rodaq` 模块，管理守护进程生命周期。HTTP 层负责控制、查询和 Web 静态资源，Drogon 是接入验证候选。

生产包将 [Web 前端](../web/README.md) 的静态资源与服务端一起部署。页面和 `/api/v1/` 由同一本机回环地址提供，Web 和本机自动化共用 API。服务通过 `rodaq` 模块执行录制操作并统一管理任务配置。

当前模块契约以 [Notion 上的 RoDAQ 设计文档](https://verbose-vulture-f51.notion.site/RoDAQ-417da2b2a80f81c68d97cffe766805b3) 为准；现有描述属于目标范围，不代表已完成实现。
