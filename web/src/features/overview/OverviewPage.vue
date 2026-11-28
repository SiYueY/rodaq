<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { RouterLink } from 'vue-router';
import { CLIENT_TIMEOUTS } from '../../api/client';
import type { PageData, SystemData, TaskSummaryData } from '../../api/generated';
import { usePolling } from '../../composables/usePolling';
import { useSessionStore } from '../../composables/useSession';
import { formatIecBytes, formatRatePerSec } from '../../format/decimal';
import { formatDurationMs } from '../../format/time';
import StatusBadge from '../../components/StatusBadge.vue';
import FreshnessBadge from '../../components/FreshnessBadge.vue';
import ErrorPanel from '../../components/ErrorPanel.vue';
import DataTable, { type TableColumn } from '../../components/DataTable.vue';

const session = useSessionStore();
const nowMs = ref(Date.now());
const cursorStack = ref<(string | undefined)[]>([undefined]);
const currentCursor = computed(() => cursorStack.value[cursorStack.value.length - 1]);

let clockTimer: ReturnType<typeof globalThis.setInterval> | null = null;

const systemPoll = usePolling<SystemData>({
  resourceKey: 'overview.system',
  enabled: () => session.lifecycle === 'AuthenticatedReady',
  fetcher: async (signal) => {
    const sys = await session.api.request<SystemData>({
      method: 'GET',
      path: '/api/system',
      timeoutMs: CLIENT_TIMEOUTS.sessionOrSystem,
      signal,
    });
    session.updateServerPhaseFromSystem(sys);
    return sys;
  },
  foregroundMs: 2_000,
  hiddenMs: 10_000,
  maxBackoffMs: 15_000,
  getFloorMs: () => session.pollingFloorMs,
});

const tasksPoll = usePolling<PageData<TaskSummaryData>>({
  resourceKey: 'overview.tasks',
  enabled: () => session.lifecycle === 'AuthenticatedReady',
  fetcher: (signal) =>
    session.api.request<PageData<TaskSummaryData>>({
      method: 'GET',
      path: '/api/tasks',
      query: { cursor: currentCursor.value, limit: '50' },
      timeoutMs: CLIENT_TIMEOUTS.query,
      signal,
    }),
  foregroundMs: 1_000,
  hiddenMs: 5_000,
  maxBackoffMs: 10_000,
  getFloorMs: () => session.pollingFloorMs,
});

const taskColumns: TableColumn[] = [
  { key: 'name', label: 'Task' },
  { key: 'desired', label: 'Desired' },
  { key: 'condition', label: 'Execution Condition' },
  { key: 'op', label: 'Current Operation' },
  { key: 'streams', label: 'Streams (Recv/Total)', numeric: true },
  { key: 'rate', label: 'Accepted Rate', numeric: true },
  { key: 'lag', label: 'Commit Lag', numeric: true },
  { key: 'impact', label: 'Data Impact' },
  { key: 'freshness', label: 'Freshness' },
];

const tasks = computed(() => tasksPoll.state.data?.items ?? []);
const system = computed(() => systemPoll.state.data ?? session.systemSnapshot);

const issues = computed(() => {
  const list: { tone: 'warning' | 'danger'; text: string }[] = [];
  const sys = system.value;
  if (sys) {
    if (sys.deployment_error) {
      list.push({ tone: 'warning', text: `Deployment warning: ${sys.deployment_error}` });
    }
    for (const v of sys.volumes) {
      if (v.pressure !== 'normal') {
        list.push({
          tone: v.pressure === 'critical' || v.pressure === 'protected' ? 'danger' : 'warning',
          text: `Volume "${v.name}" pressure is ${v.pressure}`,
        });
      }
    }
  }
  for (const t of tasks.value) {
    if (t.data_impact !== 'none') {
      list.push({
        tone: 'danger',
        text: `Task ${t.name ?? t.task_id} reports Data Impact: ${t.data_impact}`,
      });
    }
  }
  return list;
});

function conditionTone(c: string): 'neutral' | 'info' | 'success' | 'warning' | 'danger' {
  if (c === 'active') return 'success';
  if (c === 'preparing' || c === 'draining') return 'info';
  if (c === 'degraded' || c === 'protected') return 'warning';
  if (c === 'failed') return 'danger';
  return 'neutral';
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
  systemPoll.start();
  tasksPoll.start();
  clockTimer = globalThis.setInterval(() => {
    nowMs.value = Date.now();
  }, 1_000);
});

onBeforeUnmount(() => {
  systemPoll.stop();
  tasksPoll.stop();
  if (clockTimer !== null) {
    globalThis.clearInterval(clockTimer);
  }
});
</script>

<template>
  <section>
    <div class="page-header">
      <h1>Overview</h1>
      <div class="row-actions">
        <RouterLink to="/tasks/new" class="btn btn-primary">Create Task</RouterLink>
        <button type="button" class="btn" @click="systemPoll.refreshNow(); tasksPoll.refreshNow()">
          Refresh
        </button>
      </div>
    </div>

    <ErrorPanel v-if="systemPoll.state.error" :error="systemPoll.state.error" title="System Status Error" />
    <ErrorPanel v-if="tasksPoll.state.error" :error="tasksPoll.state.error" title="Task Summary Error" />

    <div class="grid-3">
      <article class="card">
        <h2 class="card-title">Service Status</h2>
        <div v-if="!system && systemPoll.state.phase === 'loading'" class="skeleton" aria-hidden="true" />
        <template v-else-if="system">
          <div class="row-actions" style="margin-bottom: 8px">
            <StatusBadge
              :label="`Phase: ${system.server_phase}`"
              :tone="system.server_phase === 'ready' ? 'success' : 'warning'"
            />
            <StatusBadge
              :label="system.core_ready ? 'Core Ready' : 'Core Not Ready'"
              :tone="system.core_ready ? 'success' : 'warning'"
            />
            <FreshnessBadge
              :freshness="systemPoll.state.phase === 'stale' ? 'stale' : system.freshness.state"
              :server-age-ms="system.freshness.age_ms"
              :received-at-ms="systemPoll.state.receivedAt"
              :now-ms="nowMs"
            />
          </div>
        </template>
      </article>

      <article class="card">
        <div class="row-actions" style="justify-content: space-between; margin-bottom: 8px">
          <h2 class="card-title" style="margin: 0">Volume Pressure</h2>
          <RouterLink to="/system" class="mono">Details</RouterLink>
        </div>
        <div v-if="!system" class="skeleton" aria-hidden="true" />
        <div v-else-if="system.volumes.length === 0" class="muted">No volumes reported</div>
        <div v-else>
          <div
            v-for="v in system.volumes"
            :key="v.name"
            class="row-actions"
            style="justify-content: space-between; margin-bottom: 6px"
          >
            <span>
              <strong>{{ v.name }}</strong>
              <span class="muted"> · Free {{ formatIecBytes(v.free_bytes) }} / {{ formatIecBytes(v.total_bytes) }}</span>
            </span>
            <StatusBadge
              :label="v.pressure"
              :tone="v.pressure === 'normal' ? 'success' : v.pressure === 'low' ? 'warning' : 'danger'"
            />
          </div>
        </div>
      </article>

      <article class="card">
        <h2 class="card-title">Process &amp; Backlog</h2>
        <div v-if="!system" class="skeleton" aria-hidden="true" />
        <div v-else style="font-size: 0.875rem">
          <div>
            RSS: <strong class="mono">{{ formatIecBytes(system.resources.rss_bytes) }}</strong>
            · Threads: <strong class="mono">{{ system.resources.threads }}</strong>
            · FDs: <strong class="mono">{{ system.resources.open_fds }}</strong>
          </div>
          <div style="margin-top: 6px">
            Downloads: <strong class="mono">{{ system.backlog.active_downloads }}</strong>
            · Commits: <strong class="mono">{{ system.backlog.commit_items }}</strong>
            · Commit Bytes: <strong class="mono">{{ formatIecBytes(system.backlog.commit_bytes) }}</strong>
          </div>
        </div>
      </article>
    </div>

    <article v-if="issues.length > 0" class="card">
      <h2 class="card-title">Active Issues</h2>
      <ul style="margin: 0; padding-left: 18px">
        <li v-for="(item, idx) in issues" :key="idx" style="margin-bottom: 4px">
          <StatusBadge :label="item.tone === 'danger' ? 'Alert' : 'Warning'" :tone="item.tone" />
          {{ item.text }}
        </li>
      </ul>
    </article>

    <article class="card">
      <div class="row-actions" style="justify-content: space-between; margin-bottom: 12px">
        <h2 class="card-title" style="margin: 0">Tasks</h2>
        <div class="row-actions">
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
      </div>

      <div v-if="tasksPoll.state.phase === 'loading' && !tasksPoll.state.data" class="skeleton" aria-hidden="true" />
      <div v-else-if="tasks.length === 0" class="muted" data-testid="zero-task-state">
        Service is healthy. No tasks are configured yet.
      </div>
      <DataTable
        v-else
        :columns="taskColumns"
        :rows="tasks"
        :row-key="(r) => r.task_id"
        caption="Configured RoDAQ tasks"
      >
        <template #row="{ row }">
          <td>
            <RouterLink :to="`/tasks/${encodeURIComponent(row.task_id)}`">
              <strong>{{ row.name || row.task_id }}</strong>
            </RouterLink>
            <div class="mono muted">{{ row.task_id }}</div>
          </td>
          <td><StatusBadge :label="row.desired_state" tone="info" /></td>
          <td>
            <StatusBadge
              :label="row.execution_condition"
              :tone="conditionTone(row.execution_condition)"
            />
          </td>
          <td class="mono">{{ row.current_operation_id ?? '—' }}</td>
          <td class="num">
            {{ row.runtime_summary.receiving_streams }} / {{ row.runtime_summary.total_streams }}
          </td>
          <td class="num">
            {{ formatRatePerSec(row.runtime_summary.accepted_records_per_second, 'rec/s') }}
          </td>
          <td class="num">{{ formatDurationMs(row.runtime_summary.commit_lag_ms) }}</td>
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
