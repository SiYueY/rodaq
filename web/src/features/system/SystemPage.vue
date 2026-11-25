<script setup lang="ts">
import { onMounted } from 'vue'
import { apiClient, useSession } from '../../composables/useSession'
import { usePolling } from '../../composables/usePolling'
import { formatBytesDecimal } from '../../format/format'
import type { SystemData } from '../../api/types'

const { state: sessionState, refreshSystemAndCapabilities } = useSession()

const sysPoll = usePolling<SystemData>({
  fetcher: (signal) => apiClient.request<SystemData>({ method: 'GET', path: '/api/system', signal }),
  foregroundMs: 2000,
  hiddenMs: 10000,
  maxBackoffMs: 15000
})

onMounted(() => {
  sysPoll.start()
  void refreshSystemAndCapabilities()
})
</script>

<template>
  <main class="main">
    <div class="row" style="margin-bottom: 16px">
      <h1 style="margin: 0">System &amp; Capabilities</h1>
      <span class="spacer" />
      <button class="btn" @click="refreshSystemAndCapabilities(); sysPoll.refreshNow()">Refresh</button>
    </div>

    <section class="card" v-if="sysPoll.state.data">
      <h2 style="margin-top: 0">Runtime Diagnostics</h2>
      <div class="row" style="margin-bottom: 8px">
        <span class="badge badge-info">Server Phase: {{ sysPoll.state.data.server_phase }}</span>
        <span class="badge" :class="sysPoll.state.data.core_ready ? 'badge-ok' : 'badge-warn'">
          Core Ready: {{ sysPoll.state.data.core_ready ? 'yes' : 'no' }}
        </span>
        <span class="badge">Recovery Complete: {{ sysPoll.state.data.recovery.complete ? 'yes' : 'no' }}</span>
      </div>
      <p class="mono" style="color: var(--muted)">
        RSS: {{ formatBytesDecimal(sysPoll.state.data.resources.rss_bytes) }} ·
        Threads: {{ sysPoll.state.data.resources.threads }} ·
        FDs: {{ sysPoll.state.data.resources.open_fds }}
      </p>
    </section>

    <section class="card" v-if="sessionState.capabilities">
      <h2 style="margin-top: 0">
        Backend Capabilities (App v{{ sessionState.capabilities.application_version }})
      </h2>
      <table>
        <thead>
          <tr>
            <th>Plugin ID</th>
            <th>Version</th>
            <th>Role</th>
            <th>Installed</th>
            <th>Allowed</th>
            <th>Support Status</th>
            <th>Qualification</th>
          </tr>
        </thead>
        <tbody>
          <tr
            v-for="b in sessionState.capabilities.backends"
            :key="`${b.plugin_id}@${b.plugin_version}`"
          >
            <td class="mono">{{ b.plugin_id }}</td>
            <td class="mono">{{ b.plugin_version }}</td>
            <td>{{ b.role }}</td>
            <td>{{ b.installed ? 'yes' : 'no' }}</td>
            <td>{{ b.allowed ? 'yes' : 'no' }}</td>
            <td><span class="badge badge-info">{{ b.support_status }}</span></td>
            <td>
              <span class="badge" :class="b.qualification_status === 'qualified' ? 'badge-ok' : 'badge-warn'">
                {{ b.qualification_status }}
              </span>
            </td>
          </tr>
        </tbody>
      </table>
    </section>
  </main>
</template>
