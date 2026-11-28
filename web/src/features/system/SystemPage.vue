<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { CLIENT_TIMEOUTS } from '../../api/client';
import type { SystemData } from '../../api/generated';
import { usePolling } from '../../composables/usePolling';
import { useSessionStore } from '../../composables/useSession';
import { formatIecBytes } from '../../format/decimal';
import { formatDurationMs, formatTimestampUtc } from '../../format/time';
import StatusBadge from '../../components/StatusBadge.vue';
import FreshnessBadge from '../../components/FreshnessBadge.vue';
import ErrorPanel from '../../components/ErrorPanel.vue';
import DataTable, { type TableColumn } from '../../components/DataTable.vue';

const session = useSessionStore();
const nowMs = ref(Date.now());
let clockTimer: ReturnType<typeof globalThis.setInterval> | null = null;

const systemPoll = usePolling<SystemData>({
  resourceKey: 'system.runtime',
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

const system = computed(() => systemPoll.state.data ?? session.systemSnapshot);
const caps = computed(() => session.capabilities);

const volumeCols: TableColumn[] = [
  { key: 'name', label: 'Volume' },
  { key: 'pressure', label: 'Pressure' },
  { key: 'free', label: 'Free Bytes', numeric: true },
  { key: 'total', label: 'Total Bytes', numeric: true },
];

const backendCols: TableColumn[] = [
  { key: 'plugin', label: 'Plugin ID' },
  { key: 'version', label: 'Exact Version' },
  { key: 'role', label: 'Role' },
  { key: 'installed', label: 'Installed' },
  { key: 'allowed', label: 'Allowed' },
  { key: 'support', label: 'Support Status' },
  { key: 'qualification', label: 'Qualification Status' },
];

const profileCols: TableColumn[] = [
  { key: 'name', label: 'Profile' },
  { key: 'seg', label: 'Max Segment Physical Bytes', numeric: true },
  { key: 'streams', label: 'Max Streams / Pipeline', numeric: true },
];

onMounted(() => {
  systemPoll.start();
  if (session.lifecycle === 'AuthenticatedReady' && !session.capabilities) {
    void session.refreshCapabilities();
  }
  clockTimer = globalThis.setInterval(() => {
    nowMs.value = Date.now();
  }, 1_000);
});

onBeforeUnmount(() => {
  systemPoll.stop();
  if (clockTimer !== null) {
    globalThis.clearInterval(clockTimer);
  }
});
</script>

<template>
  <section>
    <div class="page-header">
      <h1>System &amp; Capabilities</h1>
      <div class="row-actions">
        <button type="button" class="btn" @click="systemPoll.refreshNow()">Refresh Runtime</button>
        <button
          type="button"
          class="btn"
          :disabled="session.lifecycle !== 'AuthenticatedReady'"
          @click="session.refreshCapabilities()"
        >
          Refresh Capabilities
        </button>
      </div>
    </div>

    <ErrorPanel v-if="systemPoll.state.error" :error="systemPoll.state.error" title="System Runtime Error" />

    <article class="card">
      <h2 class="card-title">Runtime Health</h2>
      <div v-if="!system" class="skeleton" aria-hidden="true" />
      <div v-else class="grid-2">
        <div>
          <div class="muted">Server / Core</div>
          <div class="row-actions" style="margin-top: 6px">
            <StatusBadge
              :label="system.server_phase"
              :tone="system.server_phase === 'ready' ? 'success' : 'warning'"
            />
            <StatusBadge
              :label="system.core_ready ? 'core_ready' : 'core_not_ready'"
              :tone="system.core_ready ? 'success' : 'warning'"
            />
            <FreshnessBadge
              :freshness="systemPoll.state.phase === 'stale' ? 'stale' : system.freshness.state"
              :server-age-ms="system.freshness.age_ms"
              :received-at-ms="systemPoll.state.receivedAt"
              :now-ms="nowMs"
            />
          </div>
          <div class="muted mono" style="margin-top: 6px">
            Captured: {{ formatTimestampUtc(system.captured_at) }}
          </div>
        </div>
        <div>
          <div class="muted">Process Resources &amp; Backlog</div>
          <div style="margin-top: 6px">
            CPU Cores: <strong class="mono">{{ system.resources.cpu_cores.toFixed(2) }}</strong>
            · RSS: <strong class="mono">{{ formatIecBytes(system.resources.rss_bytes) }}</strong>
          </div>
          <div>
            Threads: <strong class="mono">{{ system.resources.threads }}</strong>
            · FDs: <strong class="mono">{{ system.resources.open_fds }}</strong>
            · Downloads: <strong class="mono">{{ system.backlog.active_downloads }}</strong>
            · Pending Commits: <strong class="mono">{{ system.backlog.commit_items }}</strong>
          </div>
        </div>
      </div>
    </article>

    <article class="card">
      <h2 class="card-title">Managed Volumes</h2>
      <DataTable
        :columns="volumeCols"
        :rows="system?.volumes ?? []"
        :row-key="(v) => v.name"
        empty-text="No volume status reported"
      >
        <template #row="{ row }">
          <td><strong>{{ row.name }}</strong></td>
          <td>
            <StatusBadge
              :label="row.pressure"
              :tone="row.pressure === 'normal' ? 'success' : row.pressure === 'low' ? 'warning' : 'danger'"
            />
          </td>
          <td class="num">{{ formatIecBytes(row.free_bytes) }}</td>
          <td class="num">{{ formatIecBytes(row.total_bytes) }}</td>
        </template>
      </DataTable>
    </article>

    <article class="card">
      <div class="row-actions" style="justify-content: space-between; margin-bottom: 8px">
        <h2 class="card-title" style="margin: 0">Backend Capabilities</h2>
        <span v-if="caps" class="mono muted">App {{ caps.application_version }}</span>
      </div>
      <p class="muted" style="margin: 0 0 10px 0; font-size: 0.85rem">
        Installed, Allowed, Support Status, and Qualification Status are reported independently.
        Only backends with <strong>qualified</strong> status are production-qualified; deferred
        backends are never offered in Task creation.
      </p>
      <DataTable
        :columns="backendCols"
        :rows="caps?.backends ?? []"
        :row-key="(b) => `${b.plugin_id}@${b.plugin_version}`"
        empty-text="Capabilities unavailable"
      >
        <template #row="{ row }">
          <td class="mono">{{ row.plugin_id }}</td>
          <td class="mono">{{ row.plugin_version }}</td>
          <td>{{ row.role }}</td>
          <td>
            <StatusBadge :label="row.installed ? 'Installed' : 'Not Installed'" :tone="row.installed ? 'info' : 'neutral'" />
          </td>
          <td>
            <StatusBadge :label="row.allowed ? 'Allowed' : 'Denied'" :tone="row.allowed ? 'success' : 'neutral'" />
          </td>
          <td>
            <StatusBadge
              :label="row.support_status"
              :tone="row.support_status === 'required' ? 'info' : row.support_status === 'qualified_candidate' ? 'warning' : 'neutral'"
            />
          </td>
          <td>
            <StatusBadge
              :label="row.qualification_status"
              :tone="row.qualification_status === 'qualified' ? 'success' : 'warning'"
            />
          </td>
        </template>
      </DataTable>
    </article>

    <article v-if="caps" class="card">
      <h2 class="card-title">Resource Profiles &amp; API Limits</h2>
      <DataTable
        :columns="profileCols"
        :rows="caps.resource_profiles"
        :row-key="(p) => p.name"
      >
        <template #row="{ row }">
          <td><strong>{{ row.name }}</strong></td>
          <td class="num">{{ formatIecBytes(row.max_segment_physical_bytes) }}</td>
          <td class="num">{{ row.max_streams_per_pipeline }}</td>
        </template>
      </DataTable>
      <div class="muted mono" style="margin-top: 10px">
        max_request_body_bytes={{ caps.api_limits.max_request_body_bytes }} ·
        max_page_size={{ caps.api_limits.max_page_size }} ·
        polling_floor={{ formatDurationMs(caps.api_limits.recommended_polling_floor_ms) }}
      </div>
    </article>
  </section>
</template>
