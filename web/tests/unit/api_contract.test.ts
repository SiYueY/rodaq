import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it, vi } from 'vitest';
import { parse as parseYaml } from 'yaml';
import { CLIENT_TIMEOUTS, createApiClient } from '../../src/api/client';
import { CLOSED_ENUM_VALUES, type SystemData } from '../../src/api/generated';
import { ApiFailure } from '../../src/api/errors';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const OPENAPI_PATH = path.resolve(HERE, '../../../server/api/openapi.yaml');

describe('OpenAPI 3.1 contract parity & Bearer API version compatibility', () => {
  it('matches server/api/openapi.yaml 3.1.0 closed enums, uses /api/* paths only, and has no /api/session', () => {
    const raw = fs.readFileSync(OPENAPI_PATH, 'utf8');
    const doc = parseYaml(raw) as {
      openapi: string;
      paths: Record<string, unknown>;
      components: { schemas: Record<string, { enum?: string[] }> };
    };
    expect(doc.openapi).toBe('3.1.0');

    const paths = Object.keys(doc.paths);
    expect(paths.length).toBeGreaterThanOrEqual(11);
    expect(paths).not.toContain('/api/session');
    for (const p of paths) {
      expect(p.startsWith('/api/')).toBe(true);
      expect(p.includes('/api/v1/')).toBe(false);
    }

    for (const [schemaName, expectedValues] of Object.entries(CLOSED_ENUM_VALUES)) {
      const schema = doc.components.schemas[schemaName];
      expect(schema, `Missing schema ${schemaName} in OpenAPI`).toBeDefined();
      expect(schema.enum).toEqual([...expectedValues]);
    }
  });

  it('sends memory Bearer token without Cookie/CSRF, tolerates minor version, and fails closed on major mismatch', async () => {
    let incompatibleCount = 0;
    let unauthCount = 0;

    const mockFetch = vi.fn<typeof fetch>();
    const client = createApiClient(
      {
        getBearerToken: () => 'test-memory-bearer-token-0123456789abcdef',
        onUnauthenticated: () => {
          unauthCount += 1;
        },
        onApiIncompatible: () => {
          incompatibleCount += 1;
        },
      },
      mockFetch,
    );

    // 1. Minor 1.2 with extra optional field succeeds cleanly.
    mockFetch.mockResolvedValueOnce(
      new Response(
        JSON.stringify({
          api_version: { major: 1, minor: 2 },
          request_id: 'req-1',
          data: {
            server_phase: 'ready',
            core_ready: true,
            resources: { cpu_cores: 0.5, rss_bytes: 1024, threads: 4, open_fds: 20 },
            volumes: [],
            backlog: { commit_items: 0, commit_bytes: 0, active_downloads: 0 },
            captured_at: '2026-11-27T08:00:00Z',
            snapshot_sequence: '1',
            freshness: { state: 'fresh', age_ms: 0 },
            future_optional_field: { ignored: true },
          },
        }),
        {
          status: 200,
          headers: {
            'Content-Type': 'application/json',
            'RoDAQ-API-Version': '1.2',
            'RoDAQ-Request-ID': 'req-1',
          },
        },
      ),
    );
    const sys = await client.request<SystemData>({
      method: 'GET',
      path: '/api/system',
      timeoutMs: CLIENT_TIMEOUTS.sessionOrSystem,
    });
    expect(sys.server_phase).toBe('ready');
    expect(incompatibleCount).toBe(0);
    const firstCall = mockFetch.mock.calls[0];
    const firstHeaders = firstCall[1]?.headers as Record<string, string>;
    expect(firstHeaders['Authorization']).toBe('Bearer test-memory-bearer-token-0123456789abcdef');
    expect(firstHeaders['RoDAQ-API-Version']).toBe('1.0');
    expect(firstHeaders['RoDAQ-CSRF-Token']).toBeUndefined();
    expect(firstCall[1]?.credentials).toBe('omit');

    // 2. Major 2.0 triggers onApiIncompatible and fails closed.
    mockFetch.mockResolvedValueOnce(
      new Response(
        JSON.stringify({
          api_version: { major: 2, minor: 0 },
          request_id: 'req-2',
          data: { server_phase: 'ready' },
        }),
        {
          status: 200,
          headers: {
            'Content-Type': 'application/json',
            'RoDAQ-API-Version': '2.0',
          },
        },
      ),
    );
    await expect(
      client.request({
        method: 'GET',
        path: '/api/system',
        timeoutMs: CLIENT_TIMEOUTS.sessionOrSystem,
      }),
    ).rejects.toMatchObject({ kind: 'api_incompatible' });
    expect(incompatibleCount).toBe(1);

    // 3. 401 triggers onUnauthenticated hook.
    mockFetch.mockResolvedValueOnce(
      new Response(
        JSON.stringify({
          api_version: { major: 1, minor: 0 },
          request_id: 'req-401',
          error: {
            code: 'unauthenticated',
            message: 'invalid bearer',
            retryability: 'never',
            failure_domain: 'request',
            data_impact: 'none',
            details: [],
          },
        }),
        {
          status: 401,
          headers: {
            'Content-Type': 'application/json',
            'RoDAQ-API-Version': '1.0',
          },
        },
      ),
    );
    await expect(
      client.request({
        method: 'GET',
        path: '/api/tasks',
        timeoutMs: CLIENT_TIMEOUTS.query,
      }),
    ).rejects.toBeInstanceOf(ApiFailure);
    expect(unauthCount).toBe(1);

    // 4. Mutation sends Bearer + Idempotency-Key + API version and never CSRF.
    mockFetch.mockResolvedValueOnce(
      new Response(
        JSON.stringify({
          api_version: { major: 1, minor: 0 },
          request_id: 'req-mut',
          data: { task_id: 't1', operation_id: 'op1', state: 'accepted' },
        }),
        {
          status: 202,
          headers: {
            'Content-Type': 'application/json',
            'RoDAQ-API-Version': '1.0',
          },
        },
      ),
    );
    await client.request({
      method: 'POST',
      path: '/api/tasks/t1/operations',
      rawJsonBody: '{"kind":"start","parameters":{}}',
      idempotencyKey: 'idem-1',
      controlGeneration: '2',
      timeoutMs: CLIENT_TIMEOUTS.mutation,
    });
    const lastCall = mockFetch.mock.calls[mockFetch.mock.calls.length - 1];
    const lastHeaders = lastCall[1]?.headers as Record<string, string>;
    expect(lastHeaders['Authorization']).toBe('Bearer test-memory-bearer-token-0123456789abcdef');
    expect(lastHeaders['Idempotency-Key']).toBe('idem-1');
    expect(lastHeaders['If-Control-Generation']).toBe('2');
    expect(lastHeaders['RoDAQ-CSRF-Token']).toBeUndefined();
  });
});
