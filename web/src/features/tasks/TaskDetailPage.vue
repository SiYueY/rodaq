<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { RouterLink, useRoute, useRouter } from 'vue-router';
import { CLIENT_TIMEOUTS } from '../../api/client';
import type {
  OperationData,
  OperationKind,
  OperationPageData,
  TaskData,
  TaskRuntimeData,
} from '../../api/generated';
import { isTerminalOperationState } from '../../api/types';
import { usePolling } from '../../composables/usePolling';
import { useRequest } from '../../composables/useRequest';
import { useSessionStore } from '../../composables/useSession';
import { formatTimestampUtc } from '../../format/time';
import { deletePendingMutation } from './pendingMutations';
import { useTaskMutation } from './useTaskMutation';
import StatusBadge from '../../components/StatusBadge.vue';
import FreshnessBadge from '../../components/FreshnessBadge.vue';
import ErrorPanel from '../../components/ErrorPanel.vue';
import TaskControlPanel from './TaskControlPanel.vue';
import PipelineRuntimePanel from './runtime/PipelineRuntimePanel.vue';
import StreamRuntimeTable from './runtime/StreamRuntimeTable.vue';
import OperationTable from './operations/OperationTable.vue';
import ArtifactsPage from '../artifacts/ArtifactsPage.vue';

type TabKey = 'overview' | 'streams' | 'operations' | 'configuration' | 'artifacts';

const route = useRoute();
const router = useRouter();
const session = useSessionStore();
const mutation = useTaskMutation();

const taskId = computed(() => String(route.params.taskId ?? ''));
const activeTab = computed<TabKey>(() => {
  const q = String(route.query.tab ?? 'overview');
  if (q === 'streams' || q === 'operations' || q === 'configuration' || q === 'artifacts') {
    return q;
  }
  return 'overview';
});

const nowMs = ref(Date.now());
const opCursorStack = ref<(string | undefined)[]>([undefined]);
const opCursor = computed(() => opCursorStack.value[opCursorStack.value.length - 1]);
const activePendingOpId = ref<string | null>(null);
const activePendingOpKey = ref<string | null>(null);

let clockTimer: ReturnType<typeof globalThis.setInterval> | null = null;

const taskPoll = usePolling<TaskData>({
  resourceKey: 'task.detail',
  enabled: () => session.lifecycle === 'AuthenticatedReady' && taskId.value.length > 0,
  fetcher: (signal) =>
    session.api.request<TaskData>({
      method: 'GET',
      path: `/api/tasks/${encodeURIComponent(taskId.value)}`,
      timeoutMs: CLIENT_TIMEOUTS.query,
      signal,
    }),
  foregroundMs: 1_000,
  hiddenMs: 5_000,
  maxBackoffMs: 10_000,
  getFloorMs: () => session.pollingFloorMs,
});

const runtimePoll = usePolling<TaskRuntimeData>({
  resourceKey: 'task.runtime',
  enabled: () => session.lifecycle === 'AuthenticatedReady' && taskId.value.length > 0,
  fetcher: (signal) =>
    session.api.request<TaskRuntimeData>({
      method: 'GET',
      path: `/api/tasks/${encodeURIComponent(taskId.value)}/runtime`,
      timeoutMs: CLIENT_TIMEOUTS.query,
      signal,
    }),
  foregroundMs: 1_000,
  hiddenMs: 5_000,
  maxBackoffMs: 10_000,
  getFloorMs: () => session.pollingFloorMs,
});

const trackedOpId = computed(
  () => activePendingOpId.value ?? taskPoll.state.data?.current_operation_id ?? null,
);

const currentOpPoll = usePolling<OperationData>({
  resourceKey: 'task.current_op',
  enabled: () => session.lifecycle === 'AuthenticatedReady' && trackedOpId.value !== null,
  fetcher: async (signal) => {
    const op = await session.api.request<OperationData>({
      method: 'GET',
      path: `/api/tasks/${encodeURIComponent(taskId.value)}/operations/${encodeURIComponent(trackedOpId.value!)}`,
      timeoutMs: CLIENT_TIMEOUTS.query,
      signal,
    });
    if (isTerminalOperationState(op.state)) {
      if (activePendingOpKey.value) {
        deletePendingMutation(activePendingOpKey.value);
        activePendingOpKey.value = null;
      }
      if (activePendingOpId.value === op.operation_id) {
        activePendingOpId.value = null;
      }
      taskPoll.refreshNow();
      runtimePoll.refreshNow();
      if (activeTab.value === 'operations') {
        void opsReq.refresh();
      }
    }
    return op;
  },
  foregroundMs: 500,
  hiddenMs: 5_000,
  maxBackoffMs: 10_000,
  getFloorMs: () => session.pollingFloorMs,
});

const opsReq = useRequest<OperationPageData>((signal) =>
  session.api.request<OperationPageData>({
    method: 'GET',
    path: `/api/tasks/${encodeURIComponent(taskId.value)}/operations`,
    query: { cursor: opCursor.value, limit: '50' },
    timeoutMs: CLIENT_TIMEOUTS.query,
    signal,
  }),
);

const configReq = useRequest((signal) =>
  session.api.requestRawConfig(`/api/tasks/${encodeURIComponent(taskId.value)}/config`, signal),
);

const isTaskNotFound = computed(
  () => taskPoll.state.error?.httpStatus === 404 && taskPoll.state.data === null,
);

const task = computed(() => taskPoll.state.data);
const runtime = computed(() => runtimePoll.state.data);

function setTab(tab: TabKey): void {
  void router.replace({ query: { ...route.query, tab } });
  if (tab === 'operations') {
    void opsReq.run();
  } else if (tab === 'configuration') {
    void configReq.run();
  }
}

async function handleOperate(kind: OperationKind): Promise<void> {
  if (!task.value) {
    return;
  }
  try {
    const accepted = await mutation.submitOperation(
      task.value.task_id,
      kind,
      task.value.control_generation,
    );
    // HTTP 202 NEVER overwrites server Task state — only tracks accepted Operation ID!
    activePendingOpId.value = accepted.operation_id;
    currentOpPoll.refreshNow();
    taskPoll.refreshNow();
    if (activeTab.value === 'operations') {
      void opsReq.refresh();
    }
  } catch {
    // Error displayed via mutation.lastError.
  }
}

function downloadConfigExport(): void {
  if (!configReq.state.data || !task.value) {
    return;
  }
  const blob = new Blob([configReq.state.data.rawJson], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `${task.value.name || task.value.task_id}.task.json`;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

function nextOpsPage(): void {
  const next = opsReq.state.data?.next_cursor;
  if (next) {
    opCursorStack.value = [...opCursorStack.value, next];
    void opsReq.run();
  }
}

function prevOpsPage(): void {
  if (opCursorStack.value.length > 1) {
    opCursorStack.value = opCursorStack.value.slice(0, -1);
    void opsReq.run();
  }
}

watch(taskId, () => {
  taskPoll.stop();
  runtimePoll.stop();
  currentOpPoll.stop();
  opsReq.abort();
  configReq.abort();
  activePendingOpId.value = null;
  opCursorStack.value = [undefined];
  taskPoll.start();
  runtimePoll.start();
  currentOpPoll.start();
});

onMounted(() => {
  taskPoll.start();
  runtimePoll.start();
  currentOpPoll.start();
  if (activeTab.value === 'operations') {
    void opsReq.run();
  } else if (activeTab.value === 'configuration') {
    void configReq.run();
  }
  clockTimer = globalThis.setInterval(() => {
    nowMs.value = Date.now();
  }, 1_000);
});

onBeforeUnmount(() => {
  taskPoll.stop();
  runtimePoll.stop();
  currentOpPoll.stop();
  opsReq.abort();
  configReq.abort();
  if (clockTimer !== null) {
    globalThis.clearInterval(clockTimer);
  }
});
</script>

<template>
  <section v-if="isTaskNotFound" class="card" role="alert">
    <h1>Task Not Found (404)</h1>
    <p>Task <code class="mono">{{ taskId }}</code> does not exist or has been retired.</p>
    <RouterLink to="/tasks" class="btn">Back to Tasks</RouterLink>
  </section>

  <section v-else>
    <div class="page-header">
      <div>
        <h1>{{ task?.name || taskId }}</h1>
        <div class="mono muted">{{ taskId }}</div>
      </div>
      <div class="row-actions">
        <RouterLink to="/tasks" class="btn">All Tasks</RouterLink>
        <button type="button" class="btn" @click="taskPoll.refreshNow(); runtimePoll.refreshNow()">
          Refresh
        </button>
      </div>
    </div>

    <ErrorPanel v-if="taskPoll.state.error" :error="taskPoll.state.error" title="Task Snapshot Error" />
    <ErrorPanel v-if="mutation.lastError.value" :error="mutation.lastError.value" title="Control Operation Error" />

    <!-- TaskHeader: Four Orthogonal Facts (Web §1.1, §2.3) -->
    <article v-if="task" class="card" data-testid="task-header">
      <div class="grid-4">
        <div>
          <div class="muted" style="font-size: 0.8rem">Desired State</div>
          <div style="margin-top: 4px">
            <StatusBadge :label="task.desired_state" tone="info" />
          </div>
        </div>
        <div>
          <div class="muted" style="font-size: 0.8rem">Execution Condition</div>
          <div style="margin-top: 4px">
            <StatusBadge
              :label="task.execution_condition"
              :tone="task.execution_condition === 'active' ? 'success' : task.execution_condition === 'failed' ? 'danger' : 'warning'"
            />
          </div>
        </div>
        <div>
          <div class="muted" style="font-size: 0.8rem">Current Operation</div>
          <div style="margin-top: 4px">
            <template v-if="currentOpPoll.state.data">
              <StatusBadge
                :label="`${currentOpPoll.state.data.kind}: ${currentOpPoll.state.data.state}`"
                :tone="currentOpPoll.state.data.state === 'succeeded' ? 'success' : currentOpPoll.state.data.state === 'failed' ? 'danger' : 'info'"
              />
            </template>
            <template v-else-if="trackedOpId">
              <StatusBadge label="accepted / tracking" tone="info" />
            </template>
            <span v-else class="muted">None active</span>
          </div>
        </div>
        <div>
          <div class="muted" style="font-size: 0.8rem">Data Impact &amp; Freshness</div>
          <div class="row-actions" style="margin-top: 4px">
            <StatusBadge
              :label="task.data_impact"
              :tone="task.data_impact === 'none' ? 'neutral' : 'danger'"
            />
            <FreshnessBadge
              :freshness="taskPoll.state.phase === 'stale' ? 'stale' : task.freshness.state"
              :server-age="task.freshness.age"
              :received-at-ms="taskPoll.state.receivedAt"
              :now-ms="nowMs"
            />
          </div>
        </div>
      </div>
      <div class="muted mono" style="margin-top: 10px">
        config_version={{ task.config_version.slice(0, 16) }} ·
        control_generation={{ task.control_generation }} ·
        captured={{ formatTimestampUtc(task.captured_at) }}
      </div>
    </article>

    <TaskControlPanel
      v-if="task"
      :task="task"
      :can-control="session.hasScope('control')"
      :can-admin="session.hasScope('admin')"
      :busy="mutation.submitting.value"
      @operate="handleOperate"
    />

    <nav class="tabs" role="tablist" aria-label="Task detail sections">
      <button
        type="button"
        role="tab"
        :aria-selected="activeTab === 'overview'"
        class="tab-btn"
        :class="{ active: activeTab === 'overview' }"
        @click="setTab('overview')"
      >
        Overview
      </button>
      <button
        type="button"
        role="tab"
        :aria-selected="activeTab === 'streams'"
        class="tab-btn"
        :class="{ active: activeTab === 'streams' }"
        @click="setTab('streams')"
      >
        Streams
      </button>
      <button
        type="button"
        role="tab"
        :aria-selected="activeTab === 'operations'"
        class="tab-btn"
        :class="{ active: activeTab === 'operations' }"
        @click="setTab('operations')"
      >
        Operations
      </button>
      <button
        type="button"
        role="tab"
        :aria-selected="activeTab === 'configuration'"
        class="tab-btn"
        :class="{ active: activeTab === 'configuration' }"
        @click="setTab('configuration')"
      >
        Configuration
      </button>
      <button
        type="button"
        role="tab"
        :aria-selected="activeTab === 'artifacts'"
        class="tab-btn"
        :class="{ active: activeTab === 'artifacts' }"
        @click="setTab('artifacts')"
      >
        Artifacts
      </button>
    </nav>

    <ErrorPanel
      v-if="runtimePoll.state.error && (activeTab === 'overview' || activeTab === 'streams')"
      :error="runtimePoll.state.error"
      title="Runtime Snapshot Error (Header Preserved)"
    />

    <template v-if="activeTab === 'overview'">
      <PipelineRuntimePanel
        :pipelines="runtime?.pipelines ?? []"
        :stale="runtimePoll.state.phase === 'stale' || runtime?.freshness.state === 'stale'"
      />
    </template>

    <template v-else-if="activeTab === 'streams'">
      <PipelineRuntimePanel
        :pipelines="runtime?.pipelines ?? []"
        :stale="runtimePoll.state.phase === 'stale' || runtime?.freshness.state === 'stale'"
      />
      <StreamRuntimeTable
        :pipelines="runtime?.pipelines ?? []"
        :stale="runtimePoll.state.phase === 'stale' || runtime?.freshness.state === 'stale'"
      />
    </template>

    <template v-else-if="activeTab === 'operations'">
      <OperationTable
        :task-id="taskId"
        :page="opsReq.state.data"
        :has-prev="opCursorStack.length > 1"
        :error="opsReq.state.error"
        @prev-page="prevOpsPage"
        @next-page="nextOpsPage"
        @refresh="opsReq.refresh()"
      />
    </template>

    <template v-else-if="activeTab === 'configuration'">
      <article class="card">
        <div class="row-actions" style="justify-content: space-between; margin-bottom: 10px">
          <h2 class="card-title" style="margin: 0">Active Portable Task Configuration</h2>
          <div class="row-actions">
            <button
              type="button"
              class="btn"
              :disabled="!configReq.state.data"
              @click="downloadConfigExport"
            >
              Export JSON
            </button>
            <RouterLink
              v-if="session.hasScope('control') && task?.desired_state === 'stopped' && task?.execution_condition === 'idle'"
              :to="`/tasks/${encodeURIComponent(taskId)}/config`"
              class="btn btn-primary"
            >
              Edit / Configure
            </RouterLink>
          </div>
        </div>
        <ErrorPanel v-if="configReq.state.error" :error="configReq.state.error" title="Config Load Error" />
        <pre v-if="configReq.state.data" class="mono" style="overflow-x: auto; background: var(--bg-subtle); padding: 12px; border-radius: 4px">{{ configReq.state.data.rawJson }}</pre>
      </article>
    </template>

    <template v-else-if="activeTab === 'artifacts'">
      <ArtifactsPage :fixed-task-id="taskId" />
    </template>
  </section>
</template>
