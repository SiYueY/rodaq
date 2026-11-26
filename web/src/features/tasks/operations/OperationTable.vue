<script setup lang="ts">
import { ref } from 'vue';
import { CLIENT_TIMEOUTS } from '../../../api/client';
import type { OperationData, OperationPageData } from '../../../api/generated';
import { ApiFailure } from '../../../api/errors';
import { useSessionStore } from '../../../composables/useSession';
import { formatTimestampUtc } from '../../../format/time';
import StatusBadge from '../../../components/StatusBadge.vue';
import ErrorPanel from '../../../components/ErrorPanel.vue';

const props = defineProps<{
  taskId: string;
  page: OperationPageData | null;
  hasPrev: boolean;
  error: ApiFailure | null;
}>();

const emit = defineEmits<{
  prevPage: [];
  nextPage: [];
  refresh: [];
}>();

const session = useSessionStore();
const lookupId = ref('');
const lookupResult = ref<OperationData | null>(null);
const lookupNotFound = ref(false);
const lookupError = ref<ApiFailure | null>(null);
const expandedOpId = ref<string | null>(null);

function stateTone(s: string): 'neutral' | 'info' | 'success' | 'warning' | 'danger' {
  if (s === 'succeeded') return 'success';
  if (s === 'accepted' || s === 'in_progress') return 'info';
  if (s === 'partially_succeeded' || s === 'superseded') return 'warning';
  if (s === 'failed') return 'danger';
  return 'neutral';
}

async function runLookup(): Promise<void> {
  const id = lookupId.value.trim();
  if (!id) {
    return;
  }
  lookupResult.value = null;
  lookupNotFound.value = false;
  lookupError.value = null;
  try {
    lookupResult.value = await session.api.request<OperationData>({
      method: 'GET',
      path: `/api/tasks/${encodeURIComponent(props.taskId)}/operations/${encodeURIComponent(id)}`,
      timeoutMs: CLIENT_TIMEOUTS.query,
    });
  } catch (err) {
    if (err instanceof ApiFailure && err.httpStatus === 404) {
      lookupNotFound.value = true;
    } else if (err instanceof ApiFailure) {
      lookupError.value = err;
    }
  }
}
</script>

<template>
  <article class="card">
    <div class="row-actions" style="justify-content: space-between; margin-bottom: 12px">
      <h2 class="card-title" style="margin: 0">Operation History</h2>
      <div class="row-actions">
        <button type="button" class="btn" :disabled="!hasPrev" @click="emit('prevPage')">
          Previous Page
        </button>
        <button type="button" class="btn" :disabled="!page?.has_more" @click="emit('nextPage')">
          Next Page
        </button>
        <button type="button" class="btn" @click="emit('refresh')">Refresh</button>
      </div>
    </div>

    <form class="row-actions" style="margin-bottom: 12px" @submit.prevent="runLookup">
      <label for="op-lookup-input" class="muted" style="font-size: 0.85rem">Lookup Operation ID:</label>
      <input
        id="op-lookup-input"
        v-model="lookupId"
        type="text"
        placeholder="UUID"
        style="padding: 4px 8px; border: 1px solid var(--border-strong); border-radius: 4px; width: 280px"
      />
      <button type="submit" class="btn">Lookup</button>
    </form>

    <div v-if="lookupNotFound" class="banner banner-info" role="status">
      <span>
        <strong>Not found or expired:</strong> Operation ID is not in retained history. This does not
        prove the operation failed or never existed; inspect current Task generation, Desired/Execution
        state, and committed Artifacts above.
      </span>
    </div>
    <ErrorPanel v-if="lookupError" :error="lookupError" title="Operation Lookup Error" />
    <div v-if="lookupResult" class="banner banner-info">
      <span>
        Lookup <code class="mono">{{ lookupResult.operation_id }}</code>:
        <strong>{{ lookupResult.kind }}</strong> ·
        <StatusBadge :label="lookupResult.state" :tone="stateTone(lookupResult.state)" /> ·
        gen <code class="mono">{{ lookupResult.control_generation }}</code>
      </span>
    </div>

    <ErrorPanel v-if="error" :error="error" title="Operation History Error" />

    <div v-if="page?.history_truncated" class="banner banner-warning" role="status">
      <span>
        Older operation history exceeds the retained window (oldest retained control generation:
        <code class="mono">{{ page.oldest_retained_control_generation ?? '—' }}</code>).
      </span>
    </div>

    <div class="table-wrap">
      <table class="data-table">
        <thead>
          <tr>
            <th scope="col">Kind</th>
            <th scope="col">Operation ID</th>
            <th scope="col">Accepted</th>
            <th scope="col" class="num">Gen</th>
            <th scope="col">State</th>
            <th scope="col">Terminal</th>
            <th scope="col">Retryability</th>
            <th scope="col">Domain</th>
            <th scope="col">Data Impact</th>
          </tr>
        </thead>
        <tbody>
          <tr v-if="!page || page.items.length === 0">
            <td colspan="9" class="muted">No operations recorded.</td>
          </tr>
          <template v-for="op in page?.items ?? []" :key="op.operation_id">
            <tr>
              <td><strong>{{ op.kind }}</strong></td>
              <td>
                <button
                  type="button"
                  class="btn mono"
                  style="padding: 2px 6px"
                  @click="expandedOpId = expandedOpId === op.operation_id ? null : op.operation_id"
                >
                  {{ op.operation_id }}
                </button>
              </td>
              <td class="mono">{{ formatTimestampUtc(op.accepted_at) }}</td>
              <td class="num">{{ op.control_generation }}</td>
              <td><StatusBadge :label="op.state" :tone="stateTone(op.state)" /></td>
              <td class="mono">{{ formatTimestampUtc(op.terminal_at) }}</td>
              <td>{{ op.retryability ?? '—' }}</td>
              <td>{{ op.failure_domain ?? '—' }}</td>
              <td>
                <StatusBadge
                  :label="op.data_impact"
                  :tone="op.data_impact === 'none' ? 'neutral' : 'warning'"
                />
              </td>
            </tr>
            <tr v-if="expandedOpId === op.operation_id">
              <td colspan="9" style="background: var(--bg-subtle)">
                <strong>Diagnostic message:</strong> {{ op.message ?? 'None' }}
              </td>
            </tr>
          </template>
        </tbody>
      </table>
    </div>
  </article>
</template>
