# RoDAQ Server

RoDAQ V1 local daemon and HTTP control/query service (`rodaq` executable), built on C++17, Drogon 1.9.14, and the public `rodaq::daq` C++ facade (`TaskService`, `ArtifactService`, `SystemService`, `DaqRuntime`).

## Architecture & Boundaries

- Exposes the versioned `/api/` HTTP/JSON contract defined in Notion **RoDAQ Server 模块设计** (`server/api/openapi.yaml` is the single machine-readable OpenAPI 3.1 projection).
- Links strictly against public `rodaq::daq` and Drogon 1.9.14; never includes private Core headers (`src/task/`, `src/pipeline/`, `src/artifact/`).
- Serves built Vue 3 static assets (`web/dist/`) on the same loopback origin with SPA fallback for non-`/api/` routes only.
- Uses bounded execution domains (`control`, `control_reserve`, `query`, `query_reserve`, `maintenance`) and independent `DownloadSession` streaming (`ArtifactReadHandle` → `DownloadRegistry` → Drogon async transport) with single-Range (`200`/`206`/`416`), `If-Range`, TCP backpressure, disconnect cancellation, and exactly-once `ReadLease` release.

## Build & Run

Configure with `-DRODAQ_BUILD_SERVER=ON` and `-DCMAKE_PREFIX_PATH=<drogon-prefix>`:

```bash
rodaq --deployment /etc/rodaq/deployment.json
```
