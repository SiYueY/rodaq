import { ref } from 'vue';
import { CLIENT_TIMEOUTS } from '../../api/client';
import type {
  DecimalString,
  MutationAcceptedData,
  OperationData,
  OperationKind,
  TaskCreateData,
} from '../../api/generated';
import { isTerminalOperationState, requiredScopeForOperation } from '../../api/types';
import { ApiFailure } from '../../api/errors';
import { useSessionStore } from '../../composables/useSession';
import {
  deletePendingMutation,
  generateIdempotencyKey,
  savePendingMutation,
  updatePendingMutationOperationId,
  type PendingMutation,
} from './pendingMutations';

export function useTaskMutation() {
  const session = useSessionStore();
  const submitting = ref(false);
  const lastError = ref<ApiFailure | null>(null);
  const lastAccepted = ref<MutationAcceptedData | null>(null);

  async function submitCreateTask(rawJsonBody: string): Promise<TaskCreateData> {
    if (!session.identity) {
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
    const persisted = savePendingMutation({
      method: 'POST',
      path: '/api/tasks',
      idempotencyKey: key,
      originPrincipal: session.identity.principal,
      requiredScope: 'control',
      operationKind: 'create',
      taskId: null,
      controlGeneration: null,
      bodyUtf8: rawJsonBody,
      createdAt: new Date().toISOString(),
      operationId: null,
    });
    if (!persisted.ok) {
      const err = new ApiFailure({
        message: persisted.reason,
        httpStatus: null,
        requestId: null,
        retryAfterMs: null,
        apiError: null,
        kind: 'validation',
      });
      lastError.value = err;
      throw err;
    }
    submitting.value = true;
    lastError.value = null;
    try {
      const created = await session.api.request<TaskCreateData>({
        method: 'POST',
        path: '/api/tasks',
        rawJsonBody,
        idempotencyKey: key,
        timeoutMs: CLIENT_TIMEOUTS.mutation,
      });
      deletePendingMutation(key);
      return created;
    } catch (err) {
      if (err instanceof ApiFailure) {
        lastError.value = err;
        // Client/server definite 4xx (not 504/network) settles the pending record.
        if (err.httpStatus !== null && err.httpStatus >= 400 && err.httpStatus < 500 && err.httpStatus !== 401) {
          deletePendingMutation(key);
        }
      }
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
    if (!session.identity) {
      throw new Error('Not authenticated');
    }
    const key = generateIdempotencyKey();
    const path = `/api/tasks/${encodeURIComponent(taskId)}/config`;
    const persisted = savePendingMutation({
      method: 'PUT',
      path,
      idempotencyKey: key,
      originPrincipal: session.identity.principal,
      requiredScope: 'control',
      operationKind: 'configure',
      taskId,
      controlGeneration,
      bodyUtf8: rawJsonBody,
      createdAt: new Date().toISOString(),
      operationId: null,
    });
    if (!persisted.ok) {
      throw new ApiFailure({
        message: persisted.reason,
        httpStatus: null,
        requestId: null,
        retryAfterMs: null,
        apiError: null,
        kind: 'validation',
      });
    }
    submitting.value = true;
    lastError.value = null;
    try {
      const accepted = await session.api.request<MutationAcceptedData>({
        method: 'PUT',
        path,
        rawJsonBody,
        idempotencyKey: key,
        controlGeneration,
        timeoutMs: CLIENT_TIMEOUTS.mutation,
      });
      updatePendingMutationOperationId(key, accepted.operation_id);
      lastAccepted.value = accepted;
      return accepted;
    } catch (err) {
      if (err instanceof ApiFailure) {
        lastError.value = err;
        if (err.httpStatus !== null && err.httpStatus >= 400 && err.httpStatus < 500 && err.httpStatus !== 401) {
          deletePendingMutation(key);
        }
      }
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
    if (!session.identity) {
      throw new Error('Not authenticated');
    }
    const key = generateIdempotencyKey();
    const path = `/api/tasks/${encodeURIComponent(taskId)}/operations`;
    const bodyUtf8 = JSON.stringify({ kind, parameters: {} });
    const persisted = savePendingMutation({
      method: 'POST',
      path,
      idempotencyKey: key,
      originPrincipal: session.identity.principal,
      requiredScope: requiredScopeForOperation(kind),
      operationKind: kind,
      taskId,
      controlGeneration,
      bodyUtf8,
      createdAt: new Date().toISOString(),
      operationId: null,
    });
    if (!persisted.ok) {
      throw new ApiFailure({
        message: persisted.reason,
        httpStatus: null,
        requestId: null,
        retryAfterMs: null,
        apiError: null,
        kind: 'validation',
      });
    }
    submitting.value = true;
    lastError.value = null;
    try {
      const accepted = await session.api.request<MutationAcceptedData>({
        method: 'POST',
        path,
        rawJsonBody: bodyUtf8,
        idempotencyKey: key,
        controlGeneration,
        timeoutMs: CLIENT_TIMEOUTS.mutation,
      });
      updatePendingMutationOperationId(key, accepted.operation_id);
      lastAccepted.value = accepted;
      return accepted;
    } catch (err) {
      if (err instanceof ApiFailure) {
        lastError.value = err;
        if (err.httpStatus !== null && err.httpStatus >= 400 && err.httpStatus < 500 && err.httpStatus !== 401) {
          deletePendingMutation(key);
        }
      }
      throw err;
    } finally {
      submitting.value = false;
    }
  }

  async function replayPendingMutation(record: PendingMutation): Promise<void> {
    if (record.operationId && record.taskId) {
      try {
        const op = await session.api.request<OperationData>({
          method: 'GET',
          path: `/api/tasks/${encodeURIComponent(record.taskId)}/operations/${encodeURIComponent(record.operationId)}`,
          timeoutMs: CLIENT_TIMEOUTS.query,
        });
        if (isTerminalOperationState(op.state)) {
          deletePendingMutation(record.idempotencyKey);
        }
      } catch (err) {
        if (err instanceof ApiFailure && err.httpStatus === 404) {
          deletePendingMutation(record.idempotencyKey);
        }
      }
      return;
    }

    try {
      if (record.operationKind === 'create') {
        await session.api.request<TaskCreateData>({
          method: record.method,
          path: record.path,
          rawJsonBody: record.bodyUtf8,
          idempotencyKey: record.idempotencyKey,
          timeoutMs: CLIENT_TIMEOUTS.mutation,
        });
        deletePendingMutation(record.idempotencyKey);
      } else {
        const accepted = await session.api.request<MutationAcceptedData>({
          method: record.method,
          path: record.path,
          rawJsonBody: record.bodyUtf8,
          idempotencyKey: record.idempotencyKey,
          controlGeneration: record.controlGeneration,
          timeoutMs: CLIENT_TIMEOUTS.mutation,
        });
        updatePendingMutationOperationId(record.idempotencyKey, accepted.operation_id);
      }
    } catch (err) {
      if (
        err instanceof ApiFailure &&
        err.httpStatus !== null &&
        err.httpStatus >= 400 &&
        err.httpStatus < 500 &&
        err.httpStatus !== 401
      ) {
        deletePendingMutation(record.idempotencyKey);
      }
    }
  }

  return {
    submitting,
    lastError,
    lastAccepted,
    submitCreateTask,
    submitConfigureTask,
    submitOperation,
    replayPendingMutation,
  };
}
