# RoDAQ Web

Web 模块位于仓库根目录，采用原生 HTML、CSS 和 JavaScript，提供录制任务控制、状态观测和产物查询。

生产包将本目录的静态资源交由 [RoDAQ Server](../server/README.md) 从本机回环地址提供。浏览器以同源相对路径调用 `/api/v1/`，与本机自动化使用同一任务控制和状态语义。

当前模块契约以 [Notion 上的 RoDAQ 设计文档](https://verbose-vulture-f51.notion.site/RoDAQ-417da2b2a80f81c68d97cffe766805b3) 为准；现有描述属于目标范围，不代表已完成实现。
