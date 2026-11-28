import { ref } from 'vue';
import { CLIENT_TIMEOUTS } from '../../api/client';
import type {
  DecimalString,
  MutationAcceptedData,
  OperationKind,
  TaskCreateData,
} from '../../api/generated';
import { ApiFailure } from '../../api/errors';
import { useSessionStore } from '../../composables/useSession';

export type MutationPhase = 'idle' | 'submitting' | 'accepted' | 'error' | 'unknown';

function generateIdempotencyKey(): string {
  if (typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function') {
    return crypto.randomUUID();
  }
  return `req-${Date.now()}-${Math.random().toString(16).slice(2)}`;
}

function isOutcomeUnknown(err: ApiFailure): boolean {
  return err.kind === 'network' || err.kind === 'timeout' || err.httpStatus === 504;
}

/**
 * Memory-only mutation composable (Web §3.5):
 * - Generates a fresh UUID Idempotency-Key per explicit user action.
 * - Never persists mutations to sessionStorage/localStorage and never auto-replays.
 * - Network/timeout/504 errors enter `unknown` phase so the UI refreshes authoritative Server state.
 */
export function useTaskMutation() {
  const session = useSessionStore();
  const phase = ref<MutationPhase>('idle');
  const submitting = ref(false);
  const lastError = ref<ApiFailure | null>(null);
  const lastAccepted = ref<MutationAcceptedData | null>(null);
  const outcomeUnknownMessage = ref<string | null>(null);

  function reset(): void {
    phase.value = 'idle';
    submitting.value = false;
    lastError.value = null;
    lastAccepted.value = null;
    outcomeUnknownMessage.value = null;
  }

  function recordFailure(err: unknown): void {
    if (err instanceof ApiFailure) {
      if (isOutcomeUnknown(err)) {
        phase.value = 'unknown';
        outcomeUnknownMessage.value =
          'Request outcome is unknown (timeout or network error). Authoritative Task state will be refreshed — do not blindly resend.';
        lastError.value = null;
      } else {
        phase.value = 'error';
        lastError.value = err;
        outcomeUnknownMessage.value = null;
      }
    } else {
      phase.value = 'unknown';
      outcomeUnknownMessage.value =
        'Request outcome is unknown. Refresh authoritative Task state before retrying.';
    }
  }

  async function submitCreateTask(rawJsonBody: string): Promise<TaskCreateData> {
    if (!session.authenticated) {
      throw new ApiFailure({
        message: 'Not authenticated',
        httpStatus: 401,
        requestId: null,
        retryAfterMs: null,
        apiError: null,
        kind: 'unauthenticated',
      });
    }
    const key = generateIdempotencyKey();
    phase.value = 'submitting';
    submitting.value = true;
    lastError.value = null;
    outcomeUnknownMessage.value = null;
    try {
      const created = await session.api.request<TaskCreateData>({
        method: 'POST',
        path: '/api/tasks',
        rawJsonBody,
        idempotencyKey: key,
        timeoutMs: CLIENT_TIMEOUTS.mutation,
      });
      phase.value = 'accepted';
      return created;
    } catch (err) {
      recordFailure(err);
      throw err;
    } finally {
      submitting.value = false;
    }
  }

  async function submitConfigureTask(
    taskId: string,
    rawJsonBody: string,
    controlGeneration: DecimalString | null,
  ): Promise<MutationAcceptedData> {
    if (!session.authenticated) {
      throw new Error('Not authenticated');
    }
    const key = generateIdempotencyKey();
    const path = `/api/tasks/${encodeURIComponent(taskId)}/config`;
    phase.value = 'submitting';
    submitting.value = true;
    lastError.value = null;
    outcomeUnknownMessage.value = null;
    try {
      const accepted = await session.api.request<MutationAcceptedData>({
        method: 'PUT',
        path,
        rawJsonBody,
        idempotencyKey: key,
        controlGeneration,
        timeoutMs: CLIENT_TIMEOUTS.mutation,
      });
      lastAccepted.value = accepted;
      phase.value = 'accepted';
      return accepted;
    } catch (err) {
      recordFailure(err);
      throw err;
    } finally {
      submitting.value = false;
    }
  }

  async function submitOperation(
    taskId: string,
    kind: OperationKind,
    controlGeneration: DecimalString | null,
  ): Promise<MutationAcceptedData> {
    if (!session.authenticated) {
      throw new Error('Not authenticated');
    }
    const key = generateIdempotencyKey();
    const path = `/api/tasks/${encodeURIComponent(taskId)}/operations`;
    const bodyUtf8 = JSON.stringify({ kind, parameters: {} });
    phase.value = 'submitting';
    submitting.value = true;
    lastError.value = null;
    outcomeUnknownMessage.value = null;
    try {
      const accepted = await session.api.request<MutationAcceptedData>({
        method: 'POST',
        path,
        rawJsonBody: bodyUtf8,
        idempotencyKey: key,
        controlGeneration,
        timeoutMs: CLIENT_TIMEOUTS.mutation,
      });
      lastAccepted.value = accepted;
      phase.value = 'accepted';
      return accepted;
    } catch (err) {
      recordFailure(err);
      throw err;
    } finally {
      submitting.value = false;
    }
  }

  return {
    phase,
    submitting,
    lastError,
    lastAccepted,
    outcomeUnknownMessage,
    reset,
    submitCreateTask,
    submitConfigureTask,
    submitOperation,
  };
}
