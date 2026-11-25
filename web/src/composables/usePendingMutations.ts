import type { DecimalString, Scope, ServerPhase } from '../api/types'

export const PENDING_PREFIX = 'rodaq.pending.v1:'
export const MAX_PENDING_RECORDS = 16

export interface PendingMutation {
  version: 1
  method: 'POST' | 'PUT'
  path: string
  idempotencyKey: string
  originPrincipal: string
  requiredScope: Scope
  controlGeneration: DecimalString | null
  bodyUtf8: string
  bodyDigest: string
  createdAt: string
  operationId: string | null
}

export interface PendingRecoveryDecision {
  eligible: PendingMutation[]
  foreignPrincipalCount: number
  blockedReason: string | null
}

// Lightweight integrity fingerprint to detect corrupted sessionStorage records (not Core digest).
export function computeLocalBodyDigest(bodyUtf8: string): string {
  let h = 2166136261 >>> 0
  for (let i = 0; i < bodyUtf8.length; i++) {
    h ^= bodyUtf8.charCodeAt(i)
    h = Math.imul(h, 16777619) >>> 0
  }
  return `fnv1a32:${h.toString(16).padStart(8, '0')}`
}

export function createPendingMutationStore(storage: Storage = window.sessionStorage) {
  function listAll(): PendingMutation[] {
    const out: PendingMutation[] = []
    for (let i = 0; i < storage.length; i++) {
      const k = storage.key(i)
      if (!k || !k.startsWith(PENDING_PREFIX)) continue
      try {
        const raw = storage.getItem(k)
        if (!raw) continue
        const parsed = JSON.parse(raw) as Partial<PendingMutation>
        if (
          parsed.version === 1 &&
          typeof parsed.idempotencyKey === 'string' &&
          typeof parsed.originPrincipal === 'string' &&
          parsed.originPrincipal.length > 0 &&
          typeof parsed.path === 'string' &&
          typeof parsed.bodyUtf8 === 'string' &&
          parsed.bodyDigest === computeLocalBodyDigest(parsed.bodyUtf8)
        ) {
          out.push(parsed as PendingMutation)
        }
      } catch {
        // Ignore corrupted record; never auto-replay.
      }
    }
    return out
  }

  function save(record: PendingMutation): boolean {
    const existing = listAll()
    const key = PENDING_PREFIX + record.idempotencyKey
    if (!storage.getItem(key) && existing.length >= MAX_PENDING_RECORDS) {
      // Web §3.5: reject new mutation when full; NEVER evict an unsettled record.
      return false
    }
    storage.setItem(key, JSON.stringify(record))
    return true
  }

  function remove(idempotencyKey: string): void {
    storage.removeItem(PENDING_PREFIX + idempotencyKey)
  }

  function clearForeignRecords(currentPrincipal: string): void {
    for (const rec of listAll()) {
      if (rec.originPrincipal !== currentPrincipal) {
        remove(rec.idempotencyKey)
      }
    }
  }

  // Web §3.5 + WEB-E2E-020: fail-closed principal, scope, phase, and TTL gate.
  function evaluateRecovery(params: {
    currentPrincipal: string | null
    currentScopes: Scope[]
    serverPhase: ServerPhase
    apiCompatible: boolean
    idempotencyRetentionMs: number
    nowMs?: number
  }): PendingRecoveryDecision {
    const all = listAll()
    if (!params.currentPrincipal || !params.apiCompatible || params.serverPhase !== 'ready') {
      return {
        eligible: [],
        foreignPrincipalCount: params.currentPrincipal
          ? all.filter((r) => r.originPrincipal !== params.currentPrincipal).length
          : 0,
        blockedReason: !params.currentPrincipal
          ? 'unauthenticated'
          : !params.apiCompatible
            ? 'api_incompatible'
            : 'server_not_ready'
      }
    }
    const now = params.nowMs ?? Date.now()
    const eligible: PendingMutation[] = []
    let foreignPrincipalCount = 0
    for (const rec of all) {
      if (rec.originPrincipal !== params.currentPrincipal) {
        foreignPrincipalCount++
        continue
      }
      if (!params.currentScopes.includes(rec.requiredScope)) {
        continue
      }
      const created = Date.parse(rec.createdAt)
      if (Number.isNaN(created) || now - created > params.idempotencyRetentionMs) {
        continue
      }
      eligible.push(rec)
    }
    return { eligible, foreignPrincipalCount, blockedReason: null }
  }

  return { listAll, save, remove, clearForeignRecords, evaluateRecovery }
}
