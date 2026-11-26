<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { CLIENT_TIMEOUTS } from '../../api/client';
import type { ArtifactData, ArtifactSummaryData, PageData } from '../../api/generated';
import { useRequest } from '../../composables/useRequest';
import { useSessionStore } from '../../composables/useSession';
import { formatIecBytes } from '../../format/decimal';
import { formatDuration, formatTimestampUtc } from '../../format/time';
import StatusBadge from '../../components/StatusBadge.vue';
import ErrorPanel from '../../components/ErrorPanel.vue';
import DataTable, { type TableColumn } from '../../components/DataTable.vue';
import ArtifactDetailPanel from './ArtifactDetailPanel.vue';

const props = defineProps<{
  fixedTaskId?: string;
}>();

const session = useSessionStore();

const taskIdFilter = ref(props.fixedTaskId ?? '');
const pipelineIdFilter = ref('');
const formatFilter = ref('');
const timeMode = ref<'committed_wall' | 'recording_local'>('committed_wall');
const fromFilter = ref('');
const toFilter = ref('');

const cursorStack = ref<(string | undefined)[]>([undefined]);
const currentCursor = computed(() => cursorStack.value[cursorStack.value.length - 1]);
const selectedArtifactId = ref<string | null>(null);

const listReq = useRequest<PageData<ArtifactSummaryData>>((signal) => {
  const query: Record<string, string | undefined> = {
    cursor: currentCursor.value,
    limit: '50',
    task_id: props.fixedTaskId ?? (taskIdFilter.value.trim() || undefined),
    pipeline_id: pipelineIdFilter.value.trim() || undefined,
    format: formatFilter.value.trim() || undefined,
  };
  if (timeMode.value === 'committed_wall') {
    query.committed_after = fromFilter.value.trim() || undefined;
    query.committed_before = toFilter.value.trim() || undefined;
  } else {
    query.recording_begin_ns = fromFilter.value.trim() || undefined;
    query.recording_end_ns = toFilter.value.trim() || undefined;
  }
  return session.api.request<PageData<ArtifactSummaryData>>({
    method: 'GET',
    path: '/api/artifacts',
    query,
    timeoutMs: CLIENT_TIMEOUTS.query,
    signal,
  });
});

const detailReq = useRequest<ArtifactData>((signal) => {
  if (!selectedArtifactId.value) {
    return Promise.reject(new Error('No artifact selected'));
  }
  return session.api.request<ArtifactData>({
    method: 'GET',
    path: `/api/artifacts/${encodeURIComponent(selectedArtifactId.value)}`,
    timeoutMs: CLIENT_TIMEOUTS.query,
    signal,
  });
});

const columns: TableColumn[] = [
  { key: 'id', label: 'Artifact ID' },
  { key: 'task', label: 'Task / Pipeline' },
  { key: 'committed', label: 'Committed At (UTC)' },
  { key: 'range', label: 'Recording Range (Task-Local)' },
  { key: 'format', label: 'Format' },
  { key: 'logical', label: 'Logical Bytes', numeric: true },
  { key: 'physical', label: 'Physical Bytes', numeric: true },
  { key: 'impact', label: 'Data Impact' },
];

function applyFilters(): void {
  // Filter change resets cursor chain per Web §4.6.
  cursorStack.value = [undefined];
  selectedArtifactId.value = null;
  void listReq.run();
}

function selectArtifact(id: string): void {
  selectedArtifactId.value = id;
  void detailReq.run();
}

function nextPage(): void {
  const next = listReq.state.data?.next_cursor;
  if (next) {
    cursorStack.value = [...cursorStack.value, next];
    void listReq.run();
  }
}

function prevPage(): void {
  if (cursorStack.value.length > 1) {
    cursorStack.value = cursorStack.value.slice(0, -1);
    void listReq.run();
  }
}

watch(
  () => props.fixedTaskId,
  (next) => {
    taskIdFilter.value = next ?? '';
    applyFilters();
  },
);

onMounted(() => {
  void listReq.run();
});

onBeforeUnmount(() => {
  listReq.abort();
  detailReq.abort();
});
</script>

<template>
  <section>
    <div v-if="!fixedTaskId" class="page-header">
      <h1>Committed Artifacts</h1>
      <button type="button" class="btn" @click="listReq.refresh()">Refresh</button>
    </div>

    <article class="card">
      <form class="grid-4" @submit.prevent="applyFilters">
        <div v-if="!fixedTaskId" class="form-field" style="margin-bottom: 0">
          <label for="art-task-filter">Task ID</label>
          <input id="art-task-filter" v-model="taskIdFilter" type="text" placeholder="Optional Task UUID" />
        </div>
        <div class="form-field" style="margin-bottom: 0">
          <label for="art-pipe-filter">Pipeline ID</label>
          <input id="art-pipe-filter" v-model="pipelineIdFilter" type="text" placeholder="Decimal ID" />
        </div>
        <div class="form-field" style="margin-bottom: 0">
          <label for="art-fmt-filter">Format</label>
          <input id="art-fmt-filter" v-model="formatFilter" type="text" placeholder="mcap / fake" />
        </div>
        <div class="form-field" style="margin-bottom: 0">
          <label for="art-time-mode">Time Filter Semantics</label>
          <select id="art-time-mode" v-model="timeMode">
            <option value="committed_wall">Wall-Clock Committed (RFC 3339)</option>
            <option value="recording_local">Task-Local Recording Duration (ns)</option>
          </select>
        </div>
        <div class="form-field" style="margin-bottom: 0">
          <label for="art-from-filter">{{ timeMode === 'committed_wall' ? 'Committed After (RFC 3339)' : 'Recording Begin (ns)' }}</label>
          <input id="art-from-filter" v-model="fromFilter" type="text" />
        </div>
        <div class="form-field" style="margin-bottom: 0">
          <label for="art-to-filter">{{ timeMode === 'committed_wall' ? 'Committed Before (RFC 3339)' : 'Recording End (ns)' }}</label>
          <input id="art-to-filter" v-model="toFilter" type="text" />
        </div>
        <div class="row-actions" style="align-self: end">
          <button type="submit" class="btn btn-primary">Query</button>
          <button type="button" class="btn" @click="listReq.refresh()">Refresh</button>
        </div>
      </form>
    </article>

    <ErrorPanel v-if="listReq.state.error" :error="listReq.state.error" title="Artifact Query Error" />

    <article class="card">
      <div class="row-actions" style="justify-content: space-between; margin-bottom: 10px">
        <span class="muted" style="font-size: 0.85rem">
          Committed artifacts only (staging/quarantined segments never appear in normal list).
        </span>
        <div class="row-actions">
          <button type="button" class="btn" :disabled="cursorStack.length <= 1" @click="prevPage">
            Previous Page
          </button>
          <button
            type="button"
            class="btn"
            :disabled="!listReq.state.data?.has_more"
            @click="nextPage"
          >
            Next Page
          </button>
        </div>
      </div>

      <DataTable
        :columns="columns"
        :rows="listReq.state.data?.items ?? []"
        :row-key="(a) => a.artifact_id"
        empty-text="No committed artifacts match the current filter."
      >
        <template #row="{ row }">
          <td>
            <button
              type="button"
              class="btn mono"
              style="padding: 2px 8px"
              @click="selectArtifact(row.artifact_id)"
            >
              {{ row.artifact_id }}
            </button>
          </td>
          <td class="mono">{{ row.task_id.slice(0, 8) }}… / p.{{ row.pipeline_id }}</td>
          <td class="mono">{{ formatTimestampUtc(row.committed_at) }}</td>
          <td class="mono">
            {{ formatDuration(row.recording_range.begin) }} → {{ formatDuration(row.recording_range.end) }}
          </td>
          <td>{{ row.format }}</td>
          <td class="num">{{ formatIecBytes(row.logical_bytes) }}</td>
          <td class="num">{{ formatIecBytes(row.physical_bytes) }}</td>
          <td>
            <StatusBadge
              :label="row.data_impact"
              :tone="row.data_impact === 'none' ? 'neutral' : 'warning'"
            />
          </td>
        </template>
      </DataTable>
    </article>

    <ErrorPanel v-if="detailReq.state.error" :error="detailReq.state.error" title="Artifact Detail Error" />
    <ArtifactDetailPanel v-if="selectedArtifactId && detailReq.state.data" :artifact="detailReq.state.data" />
  </section>
</template>
