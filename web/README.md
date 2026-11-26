# RoDAQ Web

RoDAQ V1 local operator console built with **Vue 3**, **TypeScript**, and **Vite 8.3.x** (Node.js 26.x LTS for build/CI). Production builds emit immutable hashed assets and `web_manifest.json` under `web/dist/`, served same-origin by the `rodaq` Drogon HTTP server alongside `/api/`.

## Architecture & Boundaries

- Interactive visualization and control layer over the authoritative `/api/` contract (`server/api/openapi.yaml`); never maintains a second Task state machine or predicts server state after HTTP 202.
- Explicit desktop browser target matrix: `chrome111`, `edge111`, `firefox114`, `safari16.4`.
- Browser session uses `HttpOnly; SameSite=Strict` cookie + in-memory `RoDAQ-CSRF-Token` recovered via `GET /api/session`; raw credentials and CSRF tokens are never written to `localStorage` or `sessionStorage`.
- Principal-scoped bounded `PendingMutation` records in `sessionStorage` support idempotent recovery across reloads/timeouts without cross-principal leakage or replay.
- Artifact downloads use authenticated `HEAD` preflight followed by same-origin browser navigation (never buffering large artifacts into JS memory).

## Build & Verification

```bash
cd web
npm ci
npm run typecheck
npm run test:unit
npm run build
npm run test:e2e
```
