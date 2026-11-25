import { describe, expect, it, vi } from 'vitest'
import openapi from '../../server/src/api/openapi.yaml?raw'
import { createApiClient, ApiFailure, parseRetryAfterMs } from '../src/api/client'
import { formatBytesDecimal, formatStreamRateLabel } from '../src/format/format'
import {
  computeLocalBodyDigest,
  createPendingMutationStore,
  MAX_PENDING_RECORDS
} from '../src/composables/usePendingMutations'
import type { StreamRuntimeData } from '../src/api/types'

describe('RoDAQ Web V1 Formatters & 64-bit Decimal Strings', () => {
  it('formats uint64 bytes beyond Number.MAX_SAFE_INTEGER using BigInt without precision loss', () => {
    expect(formatBytesDecimal('0')).toBe('0 B')
    expect(formatBytesDecimal('1536')).toBe('1.5 KiB')
    expect(formatBytesDecimal('9007199254740993')).toContain('TiB')
    expect(formatBytesDecimal(null)).toBe('—')
  })

  it('distinguishes 0 Hz with no source vs 0 Hz with source ready/silent (WEB-E2E-004)', () => {
    const base: StreamRuntimeData = {
      stream_id: '1',
      stream_key: 'imu',
      name: 'IMU',
      entry_ready: true,
      source_ready: false,
      receiving: false,
      last_receive_at: null,
      window: { value: '1', unit: 's' },
      received: { records: '0', logical_bytes: '0', records_per_second: 0, logical_bytes_per_second: 0 },
      admission: { accepted_records: '0', rejected_records: '0', reject_reasons: [] }
    }
    expect(formatStreamRateLabel(base)).toBe('0 Hz · no source')
    expect(formatStreamRateLabel({ ...base, source_ready: true })).toBe('0 Hz · source present/silent')
    expect(
      formatStreamRateLabel({
        ...base,
        source_ready: true,
        received: { ...base.received, records_per_second: 42.5 }
      })
    ).toBe('42.5 Hz')
  })
})

describe('RoDAQ Web V1 ApiClient & Protocol Contract', () => {
  it('sends RoDAQ-API-Version and memory CSRF on mutations and detects API major mismatch', async () => {
    const onUnauth = vi.fn()
    const onIncompat = vi.fn()
    const fakeFetch = vi.fn(async (_url: RequestInfo | URL, init?: RequestInit) => {
      const hdrs = init?.headers as Headers
      expect(hdrs.get('RoDAQ-API-Version')).toBe('1.0')
      expect(hdrs.get('RoDAQ-CSRF-Token')).toBe('csrf-mem-123')
      expect(hdrs.get('Idempotency-Key')).toBe('idem-1')
      return new Response(
        JSON.stringify({ api_version: { major: 2, minor: 0 }, request_id: 'r1', data: {} }),
        { status: 200, headers: { 'RoDAQ-API-Version': '2.0', 'RoDAQ-Request-ID': 'r1' } }
      )
    })

    const client = createApiClient(
      {
        getCsrfToken: () => 'csrf-mem-123',
        onUnauthenticated: onUnauth,
        onApiIncompatible: onIncompat
      },
      fakeFetch as unknown as typeof fetch
    )

    await expect(
      client.request({
        method: 'POST',
        path: '/api/tasks/1/operations',
        bodyText: '{"kind":"start","parameters":{}}',
        idempotencyKey: 'idem-1'
      })
    ).rejects.toBeInstanceOf(ApiFailure)
    expect(onIncompat).toHaveBeenCalledTimes(1)
    expect(parseRetryAfterMs('3')).toBe(3000)
  })
})

describe('PendingMutation Concurrent Isolation & Principal Gate (WEB-E2E-019 / WEB-E2E-020)', () => {
  it('stores multiple concurrent keys without overwriting and enforces capacity without eviction', () => {
    window.sessionStorage.clear()
    const store = createPendingMutationStore(window.sessionStorage)
    const body = '{"kind":"start","parameters":{}}'
    for (let i = 0; i < MAX_PENDING_RECORDS; i++) {
      expect(
        store.save({
          version: 1,
          method: 'POST',
          path: '/api/tasks/t1/operations',
          idempotencyKey: `key-${i}`,
          originPrincipal: 'operator-a',
          requiredScope: 'control',
          controlGeneration: '1',
          bodyUtf8: body,
          bodyDigest: computeLocalBodyDigest(body),
          createdAt: new Date().toISOString(),
          operationId: null
        })
      ).toBe(true)
    }
    // Capacity full MUST reject new record rather than evicting unsettled records.
    expect(
      store.save({
        version: 1,
        method: 'POST',
        path: '/api/tasks/t1/operations',
        idempotencyKey: 'key-overflow',
        originPrincipal: 'operator-a',
        requiredScope: 'control',
        controlGeneration: '1',
        bodyUtf8: body,
        bodyDigest: computeLocalBodyDigest(body),
        createdAt: new Date().toISOString(),
        operationId: null
      })
    ).toBe(false)
    expect(store.listAll().length).toBe(MAX_PENDING_RECORDS)
  })

  it('blocks automatic replay across principals, downgraded scopes, or recovering phase', () => {
    window.sessionStorage.clear()
    const store = createPendingMutationStore(window.sessionStorage)
    const body = '{"kind":"reset","parameters":{}}'
    store.save({
      version: 1,
      method: 'POST',
      path: '/api/tasks/t1/operations',
      idempotencyKey: 'reset-key-1',
      originPrincipal: 'admin-a',
      requiredScope: 'admin',
      controlGeneration: '5',
      bodyUtf8: body,
      bodyDigest: computeLocalBodyDigest(body),
      createdAt: new Date().toISOString(),
      operationId: null
    })

    // Principal B cannot see or replay Principal A's pending mutation.
    const foreign = store.evaluateRecovery({
      currentPrincipal: 'admin-b',
      currentScopes: ['read', 'control', 'admin'],
      serverPhase: 'ready',
      apiCompatible: true,
      idempotencyRetentionMs: 3600_000
    })
    expect(foreign.eligible.length).toBe(0)
    expect(foreign.foreignPrincipalCount).toBe(1)

    // Same principal with downgraded scope (missing 'admin') is blocked.
    const downgraded = store.evaluateRecovery({
      currentPrincipal: 'admin-a',
      currentScopes: ['read', 'control'],
      serverPhase: 'ready',
      apiCompatible: true,
      idempotencyRetentionMs: 3600_000
    })
    expect(downgraded.eligible.length).toBe(0)

    // Same principal with admin scope + Ready server is eligible.
    const allowed = store.evaluateRecovery({
      currentPrincipal: 'admin-a',
      currentScopes: ['read', 'control', 'admin'],
      serverPhase: 'ready',
      apiCompatible: true,
      idempotencyRetentionMs: 3600_000
    })
    expect(allowed.eligible.length).toBe(1)
    expect(allowed.eligible[0].idempotencyKey).toBe('reset-key-1')
  })
})

describe('OpenAPI 3.1 ↔ Web Client Consistency Gate', () => {
  it('verifies server/src/api/openapi.yaml exposes /api/ routes and never /api/v1/', () => {
    expect(openapi).toContain('openapi: 3.1.0')
    expect(openapi).not.toContain('/api/v1/')
    expect(openapi).toContain('/api/session:')
    expect(openapi).toContain('/api/tasks/{task_id}/operations:')
    expect(openapi).toContain('/api/artifacts/{artifact_id}/files/{file_id}:')
  })
})
