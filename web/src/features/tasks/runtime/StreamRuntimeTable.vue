<script lang="ts">
import type { StreamRuntimeData } from '../../../api/generated';
import { formatRateHz } from '../../../format/decimal';

export function formatZeroHzLabel(stream: StreamRuntimeData): {
  label: string;
  tone: 'success' | 'warning' | 'neutral';
} {
  const rate = stream.received.records_per_second;
  if (rate === null || rate === undefined || !Number.isFinite(rate)) {
    return { label: '—', tone: 'neutral' };
  }
  if (rate === 0) {
    if (!stream.source_ready) {
      return { label: '0 Hz · no source', tone: 'warning' };
    }
    return { label: '0 Hz · source present/silent', tone: 'neutral' };
  }
  return { label: formatRateHz(rate), tone: 'success' };
}
</script>

<script setup lang="ts">
import { computed, ref } from 'vue';
import type { PipelineRuntimeData } from '../../../api/generated';
import { formatDecimalCount, formatIecBytes } from '../../../format/decimal';
import { formatTimestampUtc } from '../../../format/time';
import StatusBadge from '../../../components/StatusBadge.vue';

interface FlatStreamRow {
  pipelineId: string;
  stream: StreamRuntimeData;
}

const props = defineProps<{
  pipelines: readonly PipelineRuntimeData[];
  stale: boolean;
}>();

const sortBy = ref<'stable' | 'name' | 'rate'>('stable');
const windowOffset = ref(0);
const WINDOW_SIZE = 200;

const flatRows = computed<FlatStreamRow[]>(() => {
  const rows: FlatStreamRow[] = [];
  for (const p of props.pipelines) {
    for (const s of p.streams) {
      rows.push({ pipelineId: p.pipeline_id, stream: s });
    }
  }
  if (sortBy.value === 'name') {
    return [...rows].sort((a, b) =>
      (a.stream.name ?? a.stream.stream_key).localeCompare(b.stream.name ?? b.stream.stream_key),
    );
  }
  if (sortBy.value === 'rate') {
    return [...rows].sort(
      (a, b) => b.stream.received.records_per_second - a.stream.received.records_per_second,
    );
  }
  return rows;
});

const needsWindowing = computed(() => flatRows.value.length > WINDOW_SIZE);
const visibleRows = computed(() => {
  if (!needsWindowing.value) {
    return flatRows.value;
  }
  const start = Math.min(windowOffset.value, Math.max(0, flatRows.value.length - WINDOW_SIZE));
  return flatRows.value.slice(start, start + WINDOW_SIZE);
});
</script>

<template>
  <article class="card">
    <div class="row-actions" style="justify-content: space-between; margin-bottom: 10px">
      <h2 class="card-title" style="margin: 0">
        Streams ({{ flatRows.length }})
      </h2>
      <div class="row-actions">
        <label for="stream-sort" class="muted" style="font-size: 0.85rem">View sort:</label>
        <select id="stream-sort" v-model="sortBy">
          <option value="stable">Server identity order</option>
          <option value="name">Name (client view)</option>
          <option value="rate">Receive Hz (client view)</option>
        </select>
        <template v-if="needsWindowing">
          <button
            type="button"
            class="btn"
            :disabled="windowOffset === 0"
            @click="windowOffset = Math.max(0, windowOffset - WINDOW_SIZE)"
          >
            Prev 200
          </button>
          <button
            type="button"
            class="btn"
            :disabled="windowOffset + WINDOW_SIZE >= flatRows.length"
            @click="windowOffset = windowOffset + WINDOW_SIZE"
          >
            Next 200
          </button>
        </template>
      </div>
    </div>

    <div v-if="flatRows.length === 0" class="muted">No streams published yet.</div>
    <div v-else class="table-wrap">
      <table class="data-table">
        <thead>
          <tr>
            <th scope="col">Pipeline / Stream</th>
            <th scope="col">Entry Ready</th>
            <th scope="col">Source Ready</th>
            <th scope="col">Receiving</th>
            <th scope="col">Last Receive</th>
            <th scope="col" class="num">Hz</th>
            <th scope="col" class="num">Received (Rec / Bytes)</th>
            <th scope="col" class="num">Accepted / Rejected</th>
            <th scope="col">Top Reject Reason</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="item in visibleRows" :key="`${item.pipelineId}-${item.stream.stream_id}`">
            <td>
              <strong>{{ item.stream.name ?? item.stream.stream_key }}</strong>
              <div class="mono muted">
                p.{{ item.pipelineId }} · s.{{ item.stream.stream_id }} ({{ item.stream.stream_key }})
              </div>
            </td>
            <td>
              <StatusBadge
                :label="item.stream.entry_ready ? 'Ready' : 'Not Ready'"
                :tone="item.stream.entry_ready ? 'success' : 'warning'"
              />
            </td>
            <td>
              <StatusBadge
                :label="item.stream.source_ready ? 'Source Ready' : 'No Source'"
                :tone="item.stream.source_ready ? 'success' : 'warning'"
              />
            </td>
            <td>
              <StatusBadge
                :label="item.stream.receiving ? 'Receiving' : 'Idle'"
                :tone="item.stream.receiving ? 'info' : 'neutral'"
              />
            </td>
            <td class="mono">{{ formatTimestampUtc(item.stream.last_receive_at) }}</td>
            <td class="num">
              <StatusBadge
                :label="formatZeroHzLabel(item.stream).label"
                :tone="stale ? 'warning' : formatZeroHzLabel(item.stream).tone"
              />
            </td>
            <td class="num">
              {{ formatDecimalCount(item.stream.received.records) }} ·
              {{ formatIecBytes(item.stream.received.logical_bytes) }}
            </td>
            <td class="num">
              {{ formatDecimalCount(item.stream.admission.accepted_records) }} /
              {{ formatDecimalCount(item.stream.admission.rejected_records) }}
            </td>
            <td class="mono">
              {{ item.stream.admission.reject_reasons[0]?.reason ?? '—' }}
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  </article>
</template>
