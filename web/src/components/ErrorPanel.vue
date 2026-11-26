<script setup lang="ts">
import { ref } from 'vue';
import { ApiFailure } from '../api/errors';
import StatusBadge from './StatusBadge.vue';

const props = defineProps<{
  error: ApiFailure;
  title?: string;
}>();

const copied = ref(false);

async function copyRequestId(): Promise<void> {
  if (!props.error.requestId || typeof navigator === 'undefined' || !navigator.clipboard) {
    return;
  }
  try {
    await navigator.clipboard.writeText(props.error.requestId);
    copied.value = true;
    setTimeout(() => {
      copied.value = false;
    }, 1500);
  } catch {
    // Ignore clipboard failure in headless contexts.
  }
}
</script>

<template>
  <section class="card" role="alert" style="border-color: #ef4444">
    <div class="row-actions" style="justify-content: space-between; margin-bottom: 8px">
      <strong>{{ title ?? 'Request Error' }}</strong>
      <div class="row-actions">
        <StatusBadge
          v-if="error.apiError?.code"
          :label="error.apiError.code"
          tone="danger"
        />
        <StatusBadge
          v-if="error.httpStatus !== null"
          :label="`HTTP ${error.httpStatus}`"
          tone="neutral"
        />
      </div>
    </div>

    <p style="margin: 0 0 8px 0">{{ error.message }}</p>

    <div v-if="error.apiError" class="row-actions" style="margin-bottom: 8px">
      <StatusBadge :label="`Retry: ${error.apiError.retryability}`" tone="info" />
      <StatusBadge :label="`Domain: ${error.apiError.failure_domain}`" tone="neutral" />
      <StatusBadge
        :label="`Impact: ${error.apiError.data_impact}`"
        :tone="error.apiError.data_impact === 'none' ? 'neutral' : 'warning'"
      />
    </div>

    <ul
      v-if="error.apiError && error.apiError.details.length > 0"
      style="margin: 0 0 8px 18px; padding: 0; font-size: 0.85rem"
    >
      <li v-for="(d, idx) in error.apiError.details" :key="`${d.path}-${d.code}-${idx}`">
        <span class="mono">{{ d.path || '/' }}</span> — {{ d.message }} ({{ d.code }})
      </li>
    </ul>

    <div v-if="error.requestId" class="row-actions muted" style="font-size: 0.8rem">
      <span>Request ID: <code class="mono">{{ error.requestId }}</code></span>
      <button type="button" class="btn" style="padding: 2px 8px" @click="copyRequestId">
        {{ copied ? 'Copied' : 'Copy ID' }}
      </button>
    </div>
  </section>
</template>
