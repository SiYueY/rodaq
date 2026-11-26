<script setup lang="ts">
import { computed } from 'vue';

const props = defineProps<{
  label: string;
  tone?: 'neutral' | 'info' | 'success' | 'warning' | 'danger';
}>();

const badgeClass = computed(() => `badge badge-${props.tone ?? 'neutral'}`);
</script>

<template>
  <span :class="badgeClass">{{ label }}</span>
</template>
