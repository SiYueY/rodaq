# RoDAQ Web

Web 模块位于仓库根目录，采用原生 HTML、CSS 和 JavaScript，提供录制任务控制、状态观测和产物查询。

生产包将本目录的静态资源交由 [RoDAQ Server](../server/README.md) 从本机回环地址提供。浏览器以同源相对路径调用 `/api/v1/`，与本机自动化使用同一任务控制和状态语义。

模块契约见[系统设计说明书](../docs/design/architecture.md)和[技术选型说明书](../docs/design/technology_selection.md)。
