<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { RouterLink, useRouter } from 'vue-router';
import { CLIENT_TIMEOUTS } from '../../api/client';
import type { PageData, TaskSummaryData } from '../../api/generated';
import { ApiFailure } from '../../api/errors';
import { usePolling } from '../../composables/usePolling';
import { useSessionStore } from '../../composables/useSession';
import { useTaskMutation } from './useTaskMutation';
import StatusBadge from '../../components/StatusBadge.vue';
import FreshnessBadge from '../../components/FreshnessBadge.vue';
import ErrorPanel from '../../components/ErrorPanel.vue';
import DataTable, { type TableColumn } from '../../components/DataTable.vue';

const session = useSessionStore();
const router = useRouter();
const mutation = useTaskMutation();

const nowMs = ref(Date.now());
const cursorStack = ref<(string | undefined)[]>([undefined]);
const currentCursor = computed(() => cursorStack.value[cursorStack.value.length - 1]);

const showImport = ref(false);
const importRawText = ref('');
const importError = ref<ApiFailure | null>(null);

let clockTimer: ReturnType<typeof globalThis.setInterval> | null = null;

const tasksPoll = usePolling<PageData<TaskSummaryData>>({
  resourceKey: 'tasks.list',
  enabled: () => session.lifecycle === 'AuthenticatedReady',
  fetcher: (signal) =>
    session.api.request<PageData<TaskSummaryData>>({
      method: 'GET',
      path: '/api/tasks',
      query: { cursor: currentCursor.value, limit: '50' },
      timeoutMs: CLIENT_TIMEOUTS.query,
      signal,
    }),
  foregroundMs: 2_000,
  hiddenMs: 10_000,
  maxBackoffMs: 15_000,
  getFloorMs: () => session.pollingFloorMs,
});

const columns: TableColumn[] = [
  { key: 'name', label: 'Task / ID' },
  { key: 'cfg', label: 'Config Version' },
  { key: 'desired', label: 'Desired' },
  { key: 'condition', label: 'Execution Condition' },
  { key: 'op', label: 'Current Operation' },
  { key: 'impact', label: 'Data Impact' },
  { key: 'freshness', label: 'Freshness' },
];

const rows = computed(() => tasksPoll.state.data?.items ?? []);

function onFileSelected(ev: Event): void {
  const input = ev.target as HTMLInputElement;
  const file = input.files?.[0];
  if (!file) {
    return;
  }
  const reader = new FileReader();
  reader.onload = () => {
    if (typeof reader.result === 'string') {
      importRawText.value = reader.result;
    }
  };
  reader.readAsText(file, 'utf-8');
}

async function submitRawImport(): Promise<void> {
  importError.value = null;
  if (!importRawText.value.trim()) {
    importError.value = new ApiFailure({
      message: 'Import JSON body is empty.',
      httpStatus: null,
      requestId: null,
      retryAfterMs: null,
      apiError: null,
      kind: 'validation',
    });
    return;
  }
  try {
    JSON.parse(importRawText.value);
  } catch {
    importError.value = new ApiFailure({
      message: 'Import body is not valid JSON.',
      httpStatus: null,
      requestId: null,
      retryAfterMs: null,
      apiError: null,
      kind: 'validation',
    });
    return;
  }
  try {
    const created = await mutation.submitCreateTask(importRawText.value);
    showImport.value = false;
    importRawText.value = '';
    await router.push(`/tasks/${encodeURIComponent(created.task_id)}`);
  } catch (err) {
    if (err instanceof ApiFailure) {
      importError.value = err;
    }
    tasksPoll.refreshNow();
  }
}

function nextPage(): void {
  const next = tasksPoll.state.data?.next_cursor;
  if (next) {
    cursorStack.value = [...cursorStack.value, next];
    tasksPoll.refreshNow();
  }
}

function prevPage(): void {
  if (cursorStack.value.length > 1) {
    cursorStack.value = cursorStack.value.slice(0, -1);
    tasksPoll.refreshNow();
  }
}

onMounted(() => {
  tasksPoll.start();
  clockTimer = globalThis.setInterval(() => {
    nowMs.value = Date.now();
  }, 1_000);
});

onBeforeUnmount(() => {
  tasksPoll.stop();
  if (clockTimer !== null) {
    globalThis.clearInterval(clockTimer);
  }
});
</script>

<template>
  <section>
    <div class="page-header">
      <h1>Tasks</h1>
      <div class="row-actions">
        <RouterLink to="/tasks/new" class="btn btn-primary">Create Task</RouterLink>
        <button type="button" class="btn" @click="showImport = !showImport">
          {{ showImport ? 'Close Import' : 'Import JSON' }}
        </button>
        <button type="button" class="btn" @click="tasksPoll.refreshNow()">Refresh</button>
      </div>
    </div>

    <article v-if="showImport" class="card">
      <h2 class="card-title">Import Portable TaskConfig JSON</h2>
      <p class="muted" style="margin: 0 0 10px 0; font-size: 0.85rem">
        Server validates Portable TaskConfig V1 fields authoritatively.
      </p>
      <ErrorPanel v-if="importError" :error="importError" title="Import Rejected" />
      <div v-if="mutation.outcomeUnknownMessage.value" class="banner banner-warning" role="status">
        <span>{{ mutation.outcomeUnknownMessage.value }}</span>
      </div>
      <div class="form-field">
        <label for="raw-import-file">Load JSON file</label>
        <input id="raw-import-file" type="file" accept=".json,application/json" @change="onFileSelected" />
      </div>
      <div class="form-field">
        <label for="raw-import-json">Portable TaskConfig JSON</label>
        <textarea
          id="raw-import-json"
          v-model="importRawText"
          class="mono"
          rows="10"
          spellcheck="false"
        />
      </div>
      <div class="row-actions">
        <button
          type="button"
          class="btn btn-primary"
          :disabled="mutation.submitting.value"
          @click="submitRawImport"
        >
          {{ mutation.submitting.value ? 'Importing…' : 'Submit Import' }}
        </button>
      </div>
    </article>

    <ErrorPanel v-if="tasksPoll.state.error" :error="tasksPoll.state.error" title="Failed to Load Tasks" />

    <article class="card">
      <div class="row-actions" style="justify-content: flex-end; margin-bottom: 10px">
        <button type="button" class="btn" :disabled="cursorStack.length <= 1" @click="prevPage">
          Previous Page
        </button>
        <button
          type="button"
          class="btn"
          :disabled="!tasksPoll.state.data?.has_more"
          @click="nextPage"
        >
          Next Page
        </button>
      </div>

      <DataTable
        :columns="columns"
        :rows="rows"
        :row-key="(r) => r.task_id"
        empty-text="No tasks exist yet."
      >
        <template #row="{ row }">
          <td>
            <RouterLink :to="`/tasks/${encodeURIComponent(row.task_id)}`">
              <strong>{{ row.name || row.task_id }}</strong>
            </RouterLink>
            <div class="mono muted">{{ row.task_id }}</div>
          </td>
          <td class="mono">{{ row.config_version.slice(0, 12) }}</td>
          <td><StatusBadge :label="row.desired_state" tone="info" /></td>
          <td>
            <StatusBadge
              :label="row.execution_condition"
              :tone="row.execution_condition === 'active' ? 'success' : row.execution_condition === 'failed' ? 'danger' : 'warning'"
            />
          </td>
          <td class="mono">{{ row.current_operation_id ?? '—' }}</td>
          <td>
            <StatusBadge
              :label="row.data_impact"
              :tone="row.data_impact === 'none' ? 'neutral' : 'danger'"
            />
          </td>
          <td>
            <FreshnessBadge
              :freshness="tasksPoll.state.phase === 'stale' ? 'stale' : row.freshness.state"
              :server-age-ms="row.freshness.age_ms"
              :received-at-ms="tasksPoll.state.receivedAt"
              :now-ms="nowMs"
            />
          </td>
        </template>
      </DataTable>
    </article>
  </section>
</template>
