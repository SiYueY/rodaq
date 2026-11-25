# RoDAQ Web Console (V1)

Vue 3 + TypeScript + Vite single-page operator console served same-origin by the RoDAQ Drogon Server.

- **API Contract**: Consumes `/api/` with `RoDAQ-API-Version: 1.0` and HttpOnly session cookie + memory-only CSRF token.
- **Browser Target**: Explicitly pinned to `chrome111`, `edge111`, `firefox114`, `safari16.4`.
- **Build**: `npm ci && npm run build` produces `dist/` and `dist/web_manifest.json` (`api_major: 1`).
- **Test**: `npm test` runs Vitest unit and contract tests.
