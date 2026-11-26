import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it, vi } from 'vitest';
import { parse as parseYaml } from 'yaml';
import { CLIENT_TIMEOUTS, createApiClient } from '../../src/api/client';
import { CLOSED_ENUM_VALUES, type SessionData } from '../../src/api/generated';
import { ApiFailure } from '../../src/api/errors';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const OPENAPI_PATH = path.resolve(HERE, '../../../server/api/openapi.yaml');

describe('OpenAPI 3.1 contract parity & API version compatibility (WEB-E2E-014)', () => {
  it('matches server/api/openapi.yaml 3.1.0 closed enums and uses /api/* paths only', () => {
    const raw = fs.readFileSync(OPENAPI_PATH, 'utf8');
    const doc = parseYaml(raw) as {
      openapi: string;
      paths: Record<string, unknown>;
      components: { schemas: Record<string, { enum?: string[] }> };
    };
    expect(doc.openapi).toBe('3.1.0');

    const paths = Object.keys(doc.paths);
    expect(paths.length).toBeGreaterThanOrEqual(12);
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

  it('accepts compatible minor API version with unknown optional fields and fails closed on major mismatch', async () => {
    let incompatibleCount = 0;
    let unauthCount = 0;

    const mockFetch = vi.fn<typeof fetch>();
    const client = createApiClient(
      {
        getCsrfToken: () => 'csrf-mem-token',
        onUnauthenticated: () => {
          unauthCount += 1;
        },
        onApiIncompatible: () => {
          incompatibleCount += 1;
        },
      },
      mockFetch,
    );

    // 1. Minor 1.2 with unknown extra optional field succeeds cleanly.
    mockFetch.mockResolvedValueOnce(
      new Response(
        JSON.stringify({
          api_version: { major: 1, minor: 2 },
          request_id: 'req-1',
          data: {
            principal: 'op1',
            scopes: ['read', 'control'],
            api_version: { major: 1, minor: 2 },
            csrf_token: 'csrf-2',
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
    const session = await client.request<SessionData>({
      method: 'GET',
      path: '/api/session',
      timeoutMs: CLIENT_TIMEOUTS.sessionOrSystem,
    });
    expect(session.principal).toBe('op1');
    expect(incompatibleCount).toBe(0);

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
            message: 'expired',
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

    // 4. Raw UTF-8 import preserves duplicate keys verbatim on the wire with CSRF & Idempotency-Key.
    const dupBody = '{"format_version":"1.0","format_version":"1.0"}';
    mockFetch.mockResolvedValueOnce(
      new Response(
        JSON.stringify({
          api_version: { major: 1, minor: 0 },
          request_id: 'req-dup',
          error: {
            code: 'invalid_json',
            message: 'duplicate key',
            retryability: 'never',
            failure_domain: 'request',
            data_impact: 'none',
            details: [],
          },
        }),
        {
          status: 400,
          headers: {
            'Content-Type': 'application/json',
            'RoDAQ-API-Version': '1.0',
          },
        },
      ),
    );
    await expect(
      client.request({
        method: 'POST',
        path: '/api/tasks',
        rawJsonBody: dupBody,
        idempotencyKey: 'idem-dup-1',
        timeoutMs: CLIENT_TIMEOUTS.mutation,
      }),
    ).rejects.toMatchObject({ httpStatus: 400 });
    const lastCall = mockFetch.mock.calls[mockFetch.mock.calls.length - 1];
    expect(lastCall[1]?.body).toBe(dupBody);
    const headers = lastCall[1]?.headers as Record<string, string>;
    expect(headers['RoDAQ-CSRF-Token']).toBe('csrf-mem-token');
    expect(headers['Idempotency-Key']).toBe('idem-dup-1');
    expect(headers['RoDAQ-API-Version']).toBe('1.0');
  });
});
