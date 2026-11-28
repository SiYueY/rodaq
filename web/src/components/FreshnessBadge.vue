<script setup lang="ts">
import { computed } from 'vue';
import type { SnapshotFreshnessState } from '../api/generated';
import { formatAgeSeconds, formatDurationMs } from '../format/time';
import StatusBadge from './StatusBadge.vue';

const props = defineProps<{
  freshness: SnapshotFreshnessState;
  serverAgeMs?: number | null;
  receivedAtMs?: number | null;
  nowMs?: number;
}>();

const tone = computed(() => {
  if (props.freshness === 'fresh') {
    return 'success';
  }
  if (props.freshness === 'stale') {
    return 'warning';
  }
  return 'neutral';
});

const text = computed(() => {
  if (props.freshness === 'unknown') {
    return 'Not observed';
  }
  const ageSuffix =
    props.freshness === 'stale'
      ? props.receivedAtMs && props.nowMs
        ? ` (${formatAgeSeconds(props.nowMs, props.receivedAtMs)})`
        : props.serverAgeMs !== null && props.serverAgeMs !== undefined
          ? ` (${formatDurationMs(props.serverAgeMs)})`
          : ''
      : '';
  return props.freshness === 'fresh' ? 'Fresh' : `Stale${ageSuffix}`;
});
</script>

<template>
  <StatusBadge :label="text" :tone="tone" />
</template>
