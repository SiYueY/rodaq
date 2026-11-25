# RoDAQ Server (V1)

Drogon 1.9.14 HTTP/JSON API server and static web console host for RoDAQ V1.

- **API Root**: `/api/` with `RoDAQ-API-Version: 1.0` negotiation (never `/api/v1/`).
- **OpenAPI 3.1 Projection**: `src/api/openapi.yaml`.
- **Core Boundary**: Links exclusively against public `rodaq::daq` service facades (`TaskService`, `SystemService`, `ArtifactService`).
- **Build**: Configure CMake with `-DRODAQ_BUILD_SERVER=ON` and `-DCMAKE_PREFIX_PATH=<drogon-1.9.14-prefix>`.
