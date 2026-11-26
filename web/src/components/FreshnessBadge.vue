<script setup lang="ts">
import { computed } from 'vue';
import type { DurationValue, SnapshotFreshnessState } from '../api/generated';
import { formatAgeSeconds, formatDuration } from '../format/time';
import StatusBadge from './StatusBadge.vue';

const props = defineProps<{
  freshness: SnapshotFreshnessState;
  serverAge?: DurationValue | null;
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
        : props.serverAge
          ? ` (${formatDuration(props.serverAge)})`
          : ''
      : '';
  return props.freshness === 'fresh' ? 'Fresh' : `Stale${ageSuffix}`;
});
</script>

<template>
  <StatusBadge :label="text" :tone="tone" />
</template>
