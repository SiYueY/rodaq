import type { DecimalString, OperationKind, Scope } from '../../api/generated';
import { requiredScopeForOperation } from '../../api/types';

/**
 * Bounded, principal-scoped PendingMutation store in sessionStorage (Web §3.5, WEB-E2E-019/020).
 * - Storage key is strictly versioned prefix + Idempotency-Key (never task/route key),
 *   so multiple concurrent mutations on the same task never overwrite each other.
 * - Enforces hard count and byte limits; when full, refuses new mutations instead of evicting.
 * - Never stores credentials, session IDs, or CSRF tokens.
 */

export const PENDING_MUTATION_PREFIX = 'rodaq.pending_mutation.v1.';
export const MAX_PENDING_MUTATIONS = 8;
export const MAX_PENDING_TOTAL_BYTES = 256 * 1024;

export interface PendingMutation {
  version: 1;
  method: 'POST' | 'PUT';
  path: string;
  idempotencyKey: string;
  originPrincipal: string;
  requiredScope: Scope;
  operationKind: OperationKind | 'create';
  taskId: string | null;
  controlGeneration: DecimalString | null;
  bodyUtf8: string;
  bodyDigest: string;
  createdAt: string;
  operationId: string | null;
}

export type PendingMutationDecision =
  | { action: 'eligible_replay'; record: PendingMutation }
  | { action: 'eligible_poll_operation'; record: PendingMutation }
  | { action: 'blocked_foreign_principal'; idempotencyKey: string }
  | { action: 'blocked_insufficient_scope'; idempotencyKey: string }
  | { action: 'blocked_expired_ttl'; idempotencyKey: string }
  | { action: 'blocked_corrupt_or_missing_principal'; storageKey: string }
  | { action: 'blocked_not_ready'; idempotencyKey: string };

export function computeLocalBodyDigest(bodyUtf8: string): string {
  // Deterministic 64-bit FNV-1a hex checksum purely for local sessionStorage tamper detection
  // (explicitly NOT Core request_digest, per Web §3.5).
  let h1 = 0x811c9dc5;
  let h2 = 0x01000193;
  for (let i = 0; i < bodyUtf8.length; i += 1) {
    const c = bodyUtf8.charCodeAt(i);
    h1 ^= c;
    h1 = Math.imul(h1, 0x01000193) >>> 0;
    h2 ^= c + i;
    h2 = Math.imul(h2, 0x85ebca6b) >>> 0;
  }
  return `${h1.toString(16).padStart(8, '0')}${h2.toString(16).padStart(8, '0')}`;
}

export function generateIdempotencyKey(): string {
  if (typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function') {
    return crypto.randomUUID();
  }
  const bytes = new Uint8Array(16);
  if (typeof crypto !== 'undefined' && typeof crypto.getRandomValues === 'function') {
    crypto.getRandomValues(bytes);
  } else {
    for (let i = 0; i < bytes.length; i += 1) {
      bytes[i] = Math.floor(Math.random() * 256);
    }
  }
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}

function getStorage(storage?: Storage): Storage | null {
  if (storage) {
    return storage;
  }
  if (typeof window !== 'undefined' && window.sessionStorage) {
    return window.sessionStorage;
  }
  return null;
}

function listPendingKeys(store: Storage): string[] {
  const keys: string[] = [];
  for (let i = 0; i < store.length; i += 1) {
    const k = store.key(i);
    if (k && k.startsWith(PENDING_MUTATION_PREFIX)) {
      keys.push(k);
    }
  }
  return keys.sort();
}

function parseRecord(raw: string | null): PendingMutation | null {
  if (!raw) {
    return null;
  }
  try {
    const obj = JSON.parse(raw) as Partial<PendingMutation>;
    if (
      obj.version !== 1 ||
      (obj.method !== 'POST' && obj.method !== 'PUT') ||
      typeof obj.path !== 'string' ||
      typeof obj.idempotencyKey !== 'string' ||
      obj.idempotencyKey.length === 0 ||
      typeof obj.originPrincipal !== 'string' ||
      obj.originPrincipal.length === 0 ||
      typeof obj.bodyUtf8 !== 'string' ||
      typeof obj.bodyDigest !== 'string' ||
      typeof obj.createdAt !== 'string'
    ) {
      return null;
    }
    if (computeLocalBodyDigest(obj.bodyUtf8) !== obj.bodyDigest) {
      return null;
    }
    const scope: Scope =
      obj.requiredScope === 'admin' || obj.requiredScope === 'control' || obj.requiredScope === 'read'
        ? obj.requiredScope
        : obj.operationKind && obj.operationKind !== 'create'
          ? requiredScopeForOperation(obj.operationKind)
          : 'control';
    return {
      version: 1,
      method: obj.method,
      path: obj.path,
      idempotencyKey: obj.idempotencyKey,
      originPrincipal: obj.originPrincipal,
      requiredScope: scope,
      operationKind: obj.operationKind ?? 'create',
      taskId: typeof obj.taskId === 'string' ? obj.taskId : null,
      controlGeneration: typeof obj.controlGeneration === 'string' ? obj.controlGeneration : null,
      bodyUtf8: obj.bodyUtf8,
      bodyDigest: obj.bodyDigest,
      createdAt: obj.createdAt,
      operationId: typeof obj.operationId === 'string' ? obj.operationId : null,
    };
  } catch {
    return null;
  }
}

export function savePendingMutation(
  record: Omit<PendingMutation, 'version' | 'bodyDigest'>,
  storage?: Storage,
): { ok: true; record: PendingMutation } | { ok: false; reason: string } {
  const store = getStorage(storage);
  if (!store) {
    return { ok: false, reason: 'sessionStorage unavailable' };
  }
  if (!record.originPrincipal) {
    return { ok: false, reason: 'originPrincipal is required for PendingMutation' };
  }
  const full: PendingMutation = {
    ...record,
    version: 1,
    bodyDigest: computeLocalBodyDigest(record.bodyUtf8),
  };
  const storageKey = PENDING_MUTATION_PREFIX + full.idempotencyKey;
  const serialized = JSON.stringify(full);
  const existingKeys = listPendingKeys(store);
  const isUpdate = existingKeys.includes(storageKey);

  if (!isUpdate && existingKeys.length >= MAX_PENDING_MUTATIONS) {
    return {
      ok: false,
      reason: `PendingMutation capacity full (${MAX_PENDING_MUTATIONS}); settle existing mutations before starting a new one`,
    };
  }

  let totalBytes = serialized.length;
  for (const k of existingKeys) {
    if (k !== storageKey) {
      totalBytes += (store.getItem(k) ?? '').length;
    }
  }
  if (totalBytes > MAX_PENDING_TOTAL_BYTES) {
    return {
      ok: false,
      reason: `PendingMutation byte budget (${MAX_PENDING_TOTAL_BYTES}) exceeded`,
    };
  }

  store.setItem(storageKey, serialized);
  return { ok: true, record: full };
}

export function updatePendingMutationOperationId(
  idempotencyKey: string,
  operationId: string,
  storage?: Storage,
): void {
  const store = getStorage(storage);
  if (!store) {
    return;
  }
  const key = PENDING_MUTATION_PREFIX + idempotencyKey;
  const parsed = parseRecord(store.getItem(key));
  if (!parsed) {
    return;
  }
  parsed.operationId = operationId;
  store.setItem(key, JSON.stringify(parsed));
}

export function deletePendingMutation(idempotencyKey: string, storage?: Storage): void {
  const store = getStorage(storage);
  if (!store) {
    return;
  }
  store.removeItem(PENDING_MUTATION_PREFIX + idempotencyKey);
}

export function clearForeignOrCorruptPendingMutations(
  currentPrincipal: string,
  storage?: Storage,
): number {
  const store = getStorage(storage);
  if (!store) {
    return 0;
  }
  let removed = 0;
  for (const key of listPendingKeys(store)) {
    const parsed = parseRecord(store.getItem(key));
    if (!parsed || parsed.originPrincipal !== currentPrincipal) {
      store.removeItem(key);
      removed += 1;
    }
  }
  return removed;
}

export function evaluatePendingMutations(params: {
  currentPrincipal: string | null;
  currentScopes: readonly Scope[];
  serverReady: boolean;
  apiCompatible: boolean;
  idempotencyRetentionMs: number;
  nowMs?: number;
  storage?: Storage;
}): PendingMutationDecision[] {
  const store = getStorage(params.storage);
  if (!store) {
    return [];
  }
  const nowMs = params.nowMs ?? Date.now();
  const decisions: PendingMutationDecision[] = [];

  for (const key of listPendingKeys(store)) {
    const parsed = parseRecord(store.getItem(key));
    if (!parsed) {
      decisions.push({ action: 'blocked_corrupt_or_missing_principal', storageKey: key });
      continue;
    }
    if (!params.currentPrincipal || !params.serverReady || !params.apiCompatible) {
      decisions.push({ action: 'blocked_not_ready', idempotencyKey: parsed.idempotencyKey });
      continue;
    }
    // Exact byte-for-byte principal match required (WEB-E2E-020).
    if (parsed.originPrincipal !== params.currentPrincipal) {
      decisions.push({
        action: 'blocked_foreign_principal',
        idempotencyKey: parsed.idempotencyKey,
      });
      continue;
    }
    if (!params.currentScopes.includes(parsed.requiredScope)) {
      decisions.push({
        action: 'blocked_insufficient_scope',
        idempotencyKey: parsed.idempotencyKey,
      });
      continue;
    }
    const createdMs = Date.parse(parsed.createdAt);
    if (Number.isNaN(createdMs) || nowMs - createdMs > params.idempotencyRetentionMs) {
      decisions.push({
        action: 'blocked_expired_ttl',
        idempotencyKey: parsed.idempotencyKey,
      });
      continue;
    }
    if (parsed.operationId) {
      decisions.push({ action: 'eligible_poll_operation', record: parsed });
    } else {
      decisions.push({ action: 'eligible_replay', record: parsed });
    }
  }
  return decisions;
}
