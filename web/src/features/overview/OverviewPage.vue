<script setup lang="ts">
import { onMounted } from 'vue'
import { apiClient, useSession } from '../../composables/useSession'
import { usePolling } from '../../composables/usePolling'
import { formatBytesDecimal, formatDuration, formatRateHz } from '../../format/format'
import type { Page, SystemData, TaskSummaryData } from '../../api/types'

const { state: sessionState } = useSession()

const systemPoll = usePolling<SystemData>({
  fetcher: (signal) => apiClient.request<SystemData>({ method: 'GET', path: '/api/system', signal }),
  foregroundMs: 2000,
  hiddenMs: 10000,
  maxBackoffMs: 15000
})

const tasksPoll = usePolling<Page<TaskSummaryData>>({
  fetcher: (signal) =>
    apiClient.request<Page<TaskSummaryData>>({
      method: 'GET',
      path: '/api/tasks',
      query: { limit: '50' },
      signal
    }),
  foregroundMs: 1000,
  hiddenMs: 5000,
  maxBackoffMs: 10000,
  enabled: () => sessionState.lifecycle === 'AuthenticatedReady'
})

onMounted(() => {
  systemPoll.start()
  tasksPoll.start()
})
</script>

<template>
  <main class="main">
    <div class="row" style="margin-bottom: 16px">
      <h1 style="margin: 0">Overview</h1>
      <span class="spacer" />
      <button class="btn" @click="systemPoll.refreshNow(); tasksPoll.refreshNow()">Refresh</button>
    </div>

    <div class="grid-2">
      <section class="card">
        <h2 style="margin-top: 0">Service Health</h2>
        <div v-if="systemPoll.state.data">
          <div class="row" style="margin-bottom: 8px">
            <span class="badge" :class="systemPoll.state.data.server_phase === 'ready' ? 'badge-ok' : 'badge-warn'">
              Phase: {{ systemPoll.state.data.server_phase }}
            </span>
            <span class="badge" :class="systemPoll.state.data.core_ready ? 'badge-ok' : 'badge-warn'">
              Core: {{ systemPoll.state.data.core_ready ? 'Ready' : 'Not Ready' }}
            </span>
            <span class="badge badge-info">Freshness: {{ systemPoll.state.data.freshness.state }}</span>
          </div>
          <div class="mono" style="color: var(--muted)">
            RSS: {{ formatBytesDecimal(systemPoll.state.data.resources.rss_bytes) }} ·
            Threads: {{ systemPoll.state.data.resources.threads }} ·
            Open FDs: {{ systemPoll.state.data.resources.open_fds }}
          </div>
          <div class="mono" style="color: var(--muted); margin-top: 4px">
            Commit Backlog: {{ systemPoll.state.data.backlog.commit_items }} items ({{ formatBytesDecimal(systemPoll.state.data.backlog.commit_bytes) }}) ·
            Active Downloads: {{ systemPoll.state.data.backlog.active_downloads }}
          </div>
        </div>
        <div v-else-if="systemPoll.state.error" class="error-box">
          {{ systemPoll.state.error.message }}
        </div>
        <div v-else style="color: var(--muted)">Loading system snapshot…</div>
      </section>

      <section class="card">
        <h2 style="margin-top: 0">Volumes</h2>
        <table v-if="systemPoll.state.data?.volumes.length">
          <thead>
            <tr>
              <th>Volume</th>
              <th class="num">Free</th>
              <th class="num">Total</th>
              <th>Pressure</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="v in systemPoll.state.data.volumes" :key="v.name">
              <td>{{ v.name }}</td>
              <td class="num mono">{{ formatBytesDecimal(v.free_bytes) }}</td>
              <td class="num mono">{{ formatBytesDecimal(v.total_bytes) }}</td>
              <td>
                <span class="badge" :class="v.pressure === 'normal' ? 'badge-ok' : 'badge-danger'">
                  {{ v.pressure }}
                </span>
              </td>
            </tr>
          </tbody>
        </table>
        <div v-else style="color: var(--muted)">No volume facts yet.</div>
      </section>
    </div>

    <section class="card">
      <div class="row" style="margin-bottom: 10px">
        <h2 style="margin: 0">Tasks</h2>
        <span class="spacer" />
        <router-link to="/tasks" class="btn">All Tasks</router-link>
      </div>
      <div v-if="sessionState.lifecycle === 'AuthenticatedRecovering'" class="badge badge-warn">
        Server is recovering — task controls are temporarily disabled.
      </div>
      <table v-else-if="tasksPoll.state.data && tasksPoll.state.data.items.length">
        <thead>
          <tr>
            <th>Task</th>
            <th>Desired</th>
            <th>Execution</th>
            <th>Data Impact</th>
            <th class="num">Accepted Rate</th>
            <th class="num">Commit Lag</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="t in tasksPoll.state.data.items" :key="t.task_id">
            <td>
              <router-link :to="`/tasks/${t.task_id}`" class="mono">{{ t.name || t.task_id }}</router-link>
            </td>
            <td><span class="badge badge-info">{{ t.desired_state }}</span></td>
            <td>
              <span class="badge" :class="t.execution_condition === 'active' || t.execution_condition === 'idle' ? 'badge-ok' : 'badge-warn'">
                {{ t.execution_condition }}
              </span>
            </td>
            <td>
              <span class="badge" :class="t.data_impact === 'none' ? 'badge-ok' : 'badge-danger'">
                {{ t.data_impact }}
              </span>
            </td>
            <td class="num mono">{{ formatRateHz(t.runtime_summary.accepted_records_per_second) }}</td>
            <td class="num mono">{{ formatDuration(t.runtime_summary.commit_lag) }}</td>
          </tr>
        </tbody>
      </table>
      <div v-else-if="tasksPoll.state.data" style="color: var(--muted)">
        Service is ready. No tasks registered.
      </div>
      <div v-else style="color: var(--muted)">Loading tasks…</div>
    </section>
  </main>
</template>
