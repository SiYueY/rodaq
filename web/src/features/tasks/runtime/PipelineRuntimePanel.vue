<script setup lang="ts">
import type { PipelineRuntimeData } from '../../../api/generated';
import { formatDecimalCount, formatIecBytes, formatRatePerSec } from '../../../format/decimal';
import { formatDurationMs, formatTimestampUtc } from '../../../format/time';
import StatusBadge from '../../../components/StatusBadge.vue';

defineProps<{
  pipelines: readonly PipelineRuntimeData[];
  stale: boolean;
}>();
</script>

<template>
  <article class="card">
    <div class="row-actions" style="justify-content: space-between; margin-bottom: 10px">
      <h2 class="card-title" style="margin: 0">Pipelines (Receive → Admission → Queue/Writer/Storage → Commit)</h2>
      <StatusBadge v-if="stale" label="Snapshot Stale (rates not interpolated)" tone="warning" />
    </div>

    <div v-if="pipelines.length === 0" class="muted">No pipeline runtime published yet.</div>

    <div v-else class="table-wrap">
      <table class="data-table">
        <thead>
          <tr>
            <th scope="col">Pipeline</th>
            <th scope="col">Condition / Gate</th>
            <th scope="col" class="num">Recv Rate</th>
            <th scope="col" class="num">Accepted / Rejected</th>
            <th scope="col" class="num">Queue (Rec / Bytes / HWM / Age)</th>
            <th scope="col" class="num">Writer Logical</th>
            <th scope="col" class="num">Storage Physical</th>
            <th scope="col" class="num">Commit Lag / Pending</th>
            <th scope="col">Last Committed Artifact</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="p in pipelines" :key="p.pipeline_id">
            <td>
              <strong class="mono">p.{{ p.pipeline_id }}</strong>
              <StatusBadge :label="p.required ? 'Required' : 'Optional'" tone="neutral" />
            </td>
            <td>
              <StatusBadge
                :label="p.condition"
                :tone="p.condition === 'active' ? 'success' : p.condition === 'failed' ? 'danger' : 'warning'"
              />
              <StatusBadge
                :label="p.gate_state === 'open' ? 'Gate Open' : 'Gate Closed'"
                :tone="p.gate_state === 'open' ? 'info' : 'neutral'"
              />
            </td>
            <td class="num">{{ formatRatePerSec(p.received.records_per_second, 'rec/s') }}</td>
            <td class="num">
              {{ formatDecimalCount(p.admission.accepted_records) }} /
              {{ formatDecimalCount(p.admission.rejected_records) }}
            </td>
            <td class="num">
              {{ formatDecimalCount(p.queue.records) }} ·
              {{ formatIecBytes(p.queue.bytes) }} ·
              HWM {{ formatDecimalCount(p.queue.high_watermark_records) }} ·
              {{ formatDurationMs(p.queue.oldest_age_ms) }}
            </td>
            <td class="num">
              <div>{{ formatRatePerSec(p.writer.logical_bytes_per_second, 'B/s') }}</div>
              <div class="muted">{{ formatTimestampUtc(p.writer.last_progress_at) }}</div>
            </td>
            <td class="num">
              <div>{{ formatRatePerSec(p.storage.physical_bytes_per_second, 'B/s') }}</div>
              <div class="muted">{{ formatTimestampUtc(p.storage.last_progress_at) }}</div>
            </td>
            <td class="num">
              <div>{{ formatDurationMs(p.commit.lag_ms) }}</div>
              <div class="muted">
                {{
                  p.commit.pending_segments !== 0
                    ? `Sealed/Pending commit (${p.commit.pending_segments})`
                    : '0 pending'
                }}
              </div>
            </td>
            <td class="mono">
              {{ p.commit.last_committed_artifact_id ?? '—' }}
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  </article>
</template>
