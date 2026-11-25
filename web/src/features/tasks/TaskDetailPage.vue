<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { apiClient, useSession } from '../../composables/useSession'
import { usePolling } from '../../composables/usePolling'
import {
  computeLocalBodyDigest,
  createPendingMutationStore
} from '../../composables/usePendingMutations'
import { formatBytesDecimal, formatDuration, formatStreamRateLabel } from '../../format/format'
import type {
  ArtifactSummaryData,
  LifecycleOperationKind,
  MutationAcceptedData,
  OperationPage,
  Page,
  TaskData,
  TaskRuntimeData
} from '../../api/types'

const route = useRoute()
const router = useRouter()
const { state: sessionState, hasScope } = useSession()
const pendingStore = createPendingMutationStore()

const taskId = computed(() => String(route.params.taskId))
const activeTab = ref<'overview' | 'streams' | 'operations' | 'artifacts'>('overview')
const opError = ref<string | null>(null)
const pendingOpId = ref<string | null>(null)
const confirmAction = ref<'reset' | 'retire' | null>(null)

const taskPoll = usePolling<TaskData>({
  fetcher: (signal) => apiClient.request<TaskData>({ method: 'GET', path: `/api/tasks/${taskId.value}`, signal }),
  foregroundMs: 1000,
  hiddenMs: 5000,
  maxBackoffMs: 10000
})

const runtimePoll = usePolling<TaskRuntimeData>({
  fetcher: (signal) =>
    apiClient.request<TaskRuntimeData>({ method: 'GET', path: `/api/tasks/${taskId.value}/runtime`, signal }),
  foregroundMs: 1000,
  hiddenMs: 5000,
  maxBackoffMs: 10000
})

const opsPoll = usePolling<OperationPage>({
  fetcher: (signal) =>
    apiClient.request<OperationPage>({
      method: 'GET',
      path: `/api/tasks/${taskId.value}/operations`,
      query: { limit: '50' },
      signal
    }),
  foregroundMs: 2000,
  hiddenMs: 10000,
  maxBackoffMs: 15000
})

const artifactsPoll = usePolling<Page<ArtifactSummaryData>>({
  fetcher: (signal) =>
    apiClient.request<Page<ArtifactSummaryData>>({
      method: 'GET',
      path: '/api/artifacts',
      query: { task_id: taskId.value, limit: '50' },
      signal
    }),
  foregroundMs: 5000,
  hiddenMs: 15000,
  maxBackoffMs: 15000
})

async function sendLifecycle(kind: LifecycleOperationKind) {
  if (!sessionState.session || !taskPoll.state.data) return
  opError.value = null
  confirmAction.value = null
  const idemKey = crypto.randomUUID()
  const path = `/api/tasks/${taskId.value}/operations`
  const bodyUtf8 = JSON.stringify({ kind, parameters: {} })
  const requiredScope = kind === 'reset' || kind === 'retire' ? 'admin' : 'control'

  const saved = pendingStore.save({
    version: 1,
    method: 'POST',
    path,
    idempotencyKey: idemKey,
    originPrincipal: sessionState.session.principal,
    requiredScope,
    controlGeneration: taskPoll.state.data.control_generation,
    bodyUtf8,
    bodyDigest: computeLocalBodyDigest(bodyUtf8),
    createdAt: new Date().toISOString(),
    operationId: null
  })
  if (!saved) {
    opError.value = 'Pending mutation limit reached.'
    return
  }

  try {
    const accepted = await apiClient.request<MutationAcceptedData>({
      method: 'POST',
      path,
      bodyText: bodyUtf8,
      idempotencyKey: idemKey,
      controlGeneration: taskPoll.state.data.control_generation
    })
    pendingOpId.value = accepted.operation_id
    pendingStore.remove(idemKey)
    if (kind === 'retire') {
      await router.push('/tasks')
      return
    }
    taskPoll.refreshNow()
    opsPoll.refreshNow()
  } catch (err) {
    opError.value = err instanceof Error ? err.message : 'Operation rejected'
  }
}

async function exportConfig() {
  try {
    const raw = await apiClient.request<string>({
      method: 'GET',
      path: `/api/tasks/${taskId.value}/config`,
      rawResponse: true
    })
    const blob = new Blob([raw], { type: 'application/json' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = `task-${taskId.value}.json`
    a.click()
    URL.revokeObjectURL(url)
  } catch (err) {
    opError.value = err instanceof Error ? err.message : 'Export failed'
  }
}

onMounted(() => {
  taskPoll.start()
  runtimePoll.start()
  opsPoll.start()
  artifactsPoll.start()
})
</script>

<template>
  <main class="main">
    <div v-if="taskPoll.state.data" class="card">
      <div class="row" style="margin-bottom: 10px">
        <h1 style="margin: 0">{{ taskPoll.state.data.name || taskPoll.state.data.task_id }}</h1>
        <span class="mono" style="color: var(--muted)">gen {{ taskPoll.state.data.control_generation }}</span>
        <span class="spacer" />
        <button class="btn" @click="exportConfig">Export Config</button>
        <router-link
          v-if="hasScope('control') && taskPoll.state.data.desired_state === 'stopped'"
          :to="`/tasks/${taskId}/config`"
          class="btn"
        >
          Configure
        </router-link>
      </div>

      <!-- Four Orthogonal Facts (Web §1.1) -->
      <div class="row" style="margin-bottom: 14px">
        <span class="badge badge-info">Desired: {{ taskPoll.state.data.desired_state }}</span>
        <span class="badge" :class="taskPoll.state.data.execution_condition === 'active' || taskPoll.state.data.execution_condition === 'idle' ? 'badge-ok' : 'badge-warn'">
          Execution: {{ taskPoll.state.data.execution_condition }}
        </span>
        <span class="badge" :class="taskPoll.state.data.data_impact === 'none' ? 'badge-ok' : 'badge-danger'">
          Data Impact: {{ taskPoll.state.data.data_impact }}
        </span>
        <span class="badge">Freshness: {{ taskPoll.state.data.freshness.state }}</span>
        <span v-if="pendingOpId" class="badge badge-info">Accepted Op: {{ pendingOpId.slice(0, 12) }}…</span>
      </div>

      <!-- Controls -->
      <div v-if="hasScope('control')" class="row">
        <button
          class="btn btn-primary"
          :disabled="taskPoll.state.data.desired_state !== 'stopped'"
          @click="sendLifecycle('start')"
        >
          Start
        </button>
        <button
          class="btn"
          :disabled="taskPoll.state.data.desired_state !== 'running'"
          @click="sendLifecycle('pause')"
        >
          Pause
        </button>
        <button
          class="btn"
          :disabled="taskPoll.state.data.desired_state !== 'paused'"
          @click="sendLifecycle('resume')"
        >
          Resume
        </button>
        <button
          class="btn btn-danger"
          :disabled="taskPoll.state.data.desired_state === 'stopped'"
          @click="sendLifecycle('stop')"
        >
          Stop
        </button>
        <span class="spacer" />
        <button
          v-if="hasScope('admin')"
          class="btn btn-danger"
          :disabled="taskPoll.state.data.desired_state !== 'stopped' || taskPoll.state.data.execution_condition !== 'idle'"
          @click="confirmAction = 'reset'"
        >
          Reset Task
        </button>
        <button
          v-if="hasScope('admin')"
          class="btn btn-danger"
          :disabled="taskPoll.state.data.desired_state !== 'stopped' || taskPoll.state.data.execution_condition !== 'idle'"
          @click="confirmAction = 'retire'"
        >
          Retire Task
        </button>
      </div>
    </div>

    <div v-if="confirmAction" class="card error-box" role="dialog" aria-modal="true">
      <h3 style="margin-top: 0">
        Confirm {{ confirmAction === 'reset' ? 'Reset Task' : 'Retire Task' }}
      </h3>
      <p v-if="confirmAction === 'reset'">
        Reset will permanently delete all committed artifacts for task <code>{{ taskId }}</code> while
        preserving monotonic StreamIdentity high-water marks.
      </p>
      <p v-else>
        Retire is irreversible and permanently removes task <code>{{ taskId }}</code> once no live
        artifacts remain.
      </p>
      <div class="row">
        <button class="btn btn-danger" @click="sendLifecycle(confirmAction)">
          {{ confirmAction === 'reset' ? 'Reset Task' : 'Retire Task' }}
        </button>
        <button class="btn" @click="confirmAction = null">Cancel</button>
      </div>
    </div>

    <div v-if="opError" class="error-box" role="alert">{{ opError }}</div>

    <div class="tabs" role="tablist">
      <button class="tab" :class="{ active: activeTab === 'overview' }" @click="activeTab = 'overview'">Overview</button>
      <button class="tab" :class="{ active: activeTab === 'streams' }" @click="activeTab = 'streams'">Streams</button>
      <button class="tab" :class="{ active: activeTab === 'operations' }" @click="activeTab = 'operations'">Operations</button>
      <button class="tab" :class="{ active: activeTab === 'artifacts' }" @click="activeTab = 'artifacts'">Artifacts</button>
    </div>

    <section v-if="activeTab === 'overview'" class="card">
      <h2 style="margin-top: 0">Pipelines Summary</h2>
      <table v-if="runtimePoll.state.data?.pipelines.length">
        <thead>
          <tr>
            <th>Pipeline</th>
            <th>Condition</th>
            <th>Gate</th>
            <th class="num">Accepted</th>
            <th class="num">Queue Records</th>
            <th class="num">Pending Segments</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="p in runtimePoll.state.data.pipelines" :key="p.pipeline_id">
            <td class="mono">{{ p.pipeline_id }} ({{ p.required ? 'required' : 'optional' }})</td>
            <td><span class="badge badge-info">{{ p.condition }}</span></td>
            <td>{{ p.gate_state }}</td>
            <td class="num mono">{{ p.admission.accepted_records }}</td>
            <td class="num mono">{{ p.queue.records }}</td>
            <td class="num mono">{{ p.commit.pending_segments }}</td>
          </tr>
        </tbody>
      </table>
      <div v-else style="color: var(--muted)">No runtime snapshot available.</div>
    </section>

    <section v-if="activeTab === 'streams'" class="card">
      <h2 style="margin-top: 0">Stream Runtime</h2>
      <div v-for="p in runtimePoll.state.data?.pipelines || []" :key="p.pipeline_id" style="margin-bottom: 16px">
        <h3>Pipeline {{ p.pipeline_id }} · Gate: {{ p.gate_state }} · Queue: {{ p.queue.records }} rec</h3>
        <table>
          <thead>
            <tr>
              <th>Stream</th>
              <th>Entry Ready</th>
              <th>Source Ready</th>
              <th>Rate</th>
              <th class="num">Received</th>
              <th class="num">Accepted</th>
              <th class="num">Rejected</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="s in p.streams" :key="s.stream_id">
              <td class="mono">{{ s.name || s.stream_key }}</td>
              <td>{{ s.entry_ready ? 'yes' : 'no' }}</td>
              <td>{{ s.source_ready ? 'yes' : 'no' }}</td>
              <td class="mono">{{ formatStreamRateLabel(s) }}</td>
              <td class="num mono">{{ s.received.records }}</td>
              <td class="num mono">{{ s.admission.accepted_records }}</td>
              <td class="num mono">{{ s.admission.rejected_records }}</td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <section v-if="activeTab === 'operations'" class="card">
      <div v-if="opsPoll.state.data?.history_truncated" class="badge badge-warn" style="margin-bottom: 10px">
        Older operation history has been truncated beyond retained window.
      </div>
      <table v-if="opsPoll.state.data?.items.length">
        <thead>
          <tr>
            <th>Kind</th>
            <th>Operation ID</th>
            <th>Gen</th>
            <th>State</th>
            <th>Data Impact</th>
            <th>Accepted At</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="op in opsPoll.state.data.items" :key="op.operation_id">
            <td>{{ op.kind }}</td>
            <td class="mono">{{ op.operation_id }}</td>
            <td class="mono">{{ op.control_generation }}</td>
            <td><span class="badge badge-info">{{ op.state }}</span></td>
            <td>{{ op.data_impact }}</td>
            <td class="mono">{{ op.accepted_at }}</td>
          </tr>
        </tbody>
      </table>
      <div v-else style="color: var(--muted)">No operations recorded.</div>
    </section>

    <section v-if="activeTab === 'artifacts'" class="card">
      <table v-if="artifactsPoll.state.data?.items.length">
        <thead>
          <tr>
            <th>Artifact ID</th>
            <th>Committed At</th>
            <th>Format</th>
            <th class="num">Physical Size</th>
            <th>Recording Range</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="a in artifactsPoll.state.data.items" :key="a.artifact_id">
            <td class="mono">{{ a.artifact_id }}</td>
            <td class="mono">{{ a.committed_at }}</td>
            <td>{{ a.format }}</td>
            <td class="num mono">{{ formatBytesDecimal(a.physical_bytes) }}</td>
            <td class="mono">
              {{ formatDuration(a.recording_range.begin) }} → {{ formatDuration(a.recording_range.end) }}
            </td>
          </tr>
        </tbody>
      </table>
      <div v-else style="color: var(--muted)">No committed artifacts for this task.</div>
    </section>
  </main>
</template>
