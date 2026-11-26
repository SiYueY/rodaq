import { describe, expect, it } from 'vitest';
import {
  clearForeignOrCorruptPendingMutations,
  deletePendingMutation,
  evaluatePendingMutations,
  MAX_PENDING_MUTATIONS,
  PENDING_MUTATION_PREFIX,
  savePendingMutation,
  updatePendingMutationOperationId,
} from '../../src/features/tasks/pendingMutations';

function createMemoryStorage(): Storage {
  const map = new Map<string, string>();
  return {
    get length() {
      return map.size;
    },
    clear() {
      map.clear();
    },
    getItem(key: string) {
      return map.get(key) ?? null;
    },
    key(index: number) {
      return Array.from(map.keys())[index] ?? null;
    },
    removeItem(key: string) {
      map.delete(key);
    },
    setItem(key: string, value: string) {
      map.set(key, value);
    },
  };
}

describe('PendingMutation concurrency & capacity (WEB-E2E-019)', () => {
  it('isolates multiple concurrent keys on the same task and refuses when full without evicting', () => {
    const storage = createMemoryStorage();
    const nowIso = '2026-11-26T12:00:00Z';

    // Two concurrent mutations on the exact same taskId must not overwrite each other.
    const r1 = savePendingMutation(
      {
        method: 'POST',
        path: '/api/tasks/task-1/start',
        idempotencyKey: 'idem-task1-a',
        originPrincipal: 'operator-a',
        requiredScope: 'control',
        operationKind: 'start',
        taskId: 'task-1',
        controlGeneration: '1',
        bodyUtf8: '{}',
        createdAt: nowIso,
        operationId: null,
      },
      storage,
    );
    const r2 = savePendingMutation(
      {
        method: 'POST',
        path: '/api/tasks/task-1/stop',
        idempotencyKey: 'idem-task1-b',
        originPrincipal: 'operator-a',
        requiredScope: 'control',
        operationKind: 'stop',
        taskId: 'task-1',
        controlGeneration: '1',
        bodyUtf8: '{}',
        createdAt: nowIso,
        operationId: null,
      },
      storage,
    );
    expect(r1.ok).toBe(true);
    expect(r2.ok).toBe(true);
    expect(storage.length).toBe(2);

    // Fill to MAX_PENDING_MUTATIONS (8).
    for (let i = 2; i < MAX_PENDING_MUTATIONS; i += 1) {
      const res = savePendingMutation(
        {
          method: 'POST',
          path: `/api/tasks/task-${i}/start`,
          idempotencyKey: `idem-${i}`,
          originPrincipal: 'operator-a',
          requiredScope: 'control',
          operationKind: 'start',
          taskId: `task-${i}`,
          controlGeneration: '1',
          bodyUtf8: '{}',
          createdAt: nowIso,
          operationId: null,
        },
        storage,
      );
      expect(res.ok).toBe(true);
    }
    expect(storage.length).toBe(MAX_PENDING_MUTATIONS);

    // 9th mutation must be REFUSED without evicting existing entries.
    const overflow = savePendingMutation(
      {
        method: 'POST',
        path: '/api/tasks/task-overflow/start',
        idempotencyKey: 'idem-overflow',
        originPrincipal: 'operator-a',
        requiredScope: 'control',
        operationKind: 'start',
        taskId: 'task-overflow',
        controlGeneration: '1',
        bodyUtf8: '{}',
        createdAt: nowIso,
        operationId: null,
      },
      storage,
    );
    expect(overflow.ok).toBe(false);
    expect(storage.length).toBe(MAX_PENDING_MUTATIONS);
    expect(storage.getItem(PENDING_MUTATION_PREFIX + 'idem-task1-a')).not.toBeNull();
    expect(storage.getItem(PENDING_MUTATION_PREFIX + 'idem-task1-b')).not.toBeNull();

    // Attaching operationId transitions decision from replay -> poll_operation.
    updatePendingMutationOperationId('idem-task1-a', 'op-999', storage);
    const decisions = evaluatePendingMutations({
      currentPrincipal: 'operator-a',
      currentScopes: ['read', 'control'],
      serverReady: true,
      apiCompatible: true,
      idempotencyRetentionMs: 3600_000,
      nowMs: Date.parse('2026-11-26T12:05:00Z'),
      storage,
    });
    const d1 = decisions.find(
      (d) => 'record' in d && d.record.idempotencyKey === 'idem-task1-a',
    );
    expect(d1?.action).toBe('eligible_poll_operation');

    deletePendingMutation('idem-task1-a', storage);
    expect(storage.length).toBe(MAX_PENDING_MUTATIONS - 1);
  });
});

describe('PendingMutation principal/scope/TTL/tamper isolation (WEB-E2E-020)', () => {
  it('blocks foreign principal, scope downgrade, expired TTL, corrupt records, and Recovering mode', () => {
    const storage = createMemoryStorage();
    const createdAt = '2026-11-26T12:00:00Z';

    savePendingMutation(
      {
        method: 'POST',
        path: '/api/tasks/t1/reset',
        idempotencyKey: 'idem-admin-a',
        originPrincipal: 'admin-a',
        requiredScope: 'admin',
        operationKind: 'reset',
        taskId: 't1',
        controlGeneration: '4',
        bodyUtf8: '{"secret":"should-not-leak"}',
        createdAt,
        operationId: null,
      },
      storage,
    );

    // Inject corrupt record (missing originPrincipal) directly in sessionStorage.
    storage.setItem(
      PENDING_MUTATION_PREFIX + 'idem-corrupt',
      JSON.stringify({ version: 1, idempotencyKey: 'idem-corrupt', method: 'POST' }),
    );

    // 1. Same principal + admin scope + ready + within TTL => eligible_replay.
    const samePrincipal = evaluatePendingMutations({
      currentPrincipal: 'admin-a',
      currentScopes: ['read', 'control', 'admin'],
      serverReady: true,
      apiCompatible: true,
      idempotencyRetentionMs: 600_000,
      nowMs: Date.parse('2026-11-26T12:01:00Z'),
      storage,
    });
    expect(samePrincipal.find((d) => d.action === 'eligible_replay')).toBeDefined();
    expect(
      samePrincipal.find((d) => d.action === 'blocked_corrupt_or_missing_principal'),
    ).toBeDefined();

    // 2. Principal B logs in => blocked_foreign_principal (record/body NOT exposed!).
    const foreign = evaluatePendingMutations({
      currentPrincipal: 'admin-b',
      currentScopes: ['read', 'control', 'admin'],
      serverReady: true,
      apiCompatible: true,
      idempotencyRetentionMs: 600_000,
      nowMs: Date.parse('2026-11-26T12:01:00Z'),
      storage,
    });
    const foreignDecision = foreign.find((d) => d.action === 'blocked_foreign_principal');
    expect(foreignDecision).toBeDefined();
    expect('record' in (foreignDecision ?? {})).toBe(false);

    // 3. Same principal with downgraded scopes => blocked_insufficient_scope.
    const downgraded = evaluatePendingMutations({
      currentPrincipal: 'admin-a',
      currentScopes: ['read', 'control'],
      serverReady: true,
      apiCompatible: true,
      idempotencyRetentionMs: 600_000,
      nowMs: Date.parse('2026-11-26T12:01:00Z'),
      storage,
    });
    expect(downgraded.find((d) => d.action === 'blocked_insufficient_scope')).toBeDefined();

    // 4. Expired TTL => blocked_expired_ttl.
    const expired = evaluatePendingMutations({
      currentPrincipal: 'admin-a',
      currentScopes: ['read', 'control', 'admin'],
      serverReady: true,
      apiCompatible: true,
      idempotencyRetentionMs: 600_000,
      nowMs: Date.parse('2026-11-26T12:15:00Z'),
      storage,
    });
    expect(expired.find((d) => d.action === 'blocked_expired_ttl')).toBeDefined();

    // 5. Server Recovering => blocked_not_ready.
    const recovering = evaluatePendingMutations({
      currentPrincipal: 'admin-a',
      currentScopes: ['read', 'control', 'admin'],
      serverReady: false,
      apiCompatible: true,
      idempotencyRetentionMs: 600_000,
      nowMs: Date.parse('2026-11-26T12:01:00Z'),
      storage,
    });
    expect(recovering.find((d) => d.action === 'blocked_not_ready')).toBeDefined();

    // 6. Explicit local cleanup removes foreign + corrupt entries.
    const removed = clearForeignOrCorruptPendingMutations('admin-b', storage);
    expect(removed).toBe(2);
    expect(storage.length).toBe(0);
  });
});
