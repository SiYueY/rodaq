<script setup lang="ts">
import { nextTick, onBeforeUnmount, ref, watch } from 'vue';

const props = defineProps<{
  open: boolean;
  title: string;
  confirmLabel: string;
  danger?: boolean;
  busy?: boolean;
}>();

const emit = defineEmits<{
  confirm: [];
  cancel: [];
}>();

const panelRef = ref<HTMLElement | null>(null);
let previouslyFocused: HTMLElement | null = null;

function focusableElements(): HTMLElement[] {
  if (!panelRef.value) {
    return [];
  }
  return Array.from(
    panelRef.value.querySelectorAll<HTMLElement>(
      'button:not([disabled]), [href], input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])',
    ),
  );
}

function onKeydown(ev: KeyboardEvent): void {
  if (!props.open) {
    return;
  }
  if (ev.key === 'Escape' && !props.busy) {
    ev.preventDefault();
    emit('cancel');
    return;
  }
  if (ev.key === 'Tab') {
    const items = focusableElements();
    if (items.length === 0) {
      return;
    }
    const first = items[0];
    const last = items[items.length - 1];
    if (ev.shiftKey && document.activeElement === first) {
      ev.preventDefault();
      last.focus();
    } else if (!ev.shiftKey && document.activeElement === last) {
      ev.preventDefault();
      first.focus();
    }
  }
}

watch(
  () => props.open,
  async (isOpen) => {
    if (isOpen) {
      previouslyFocused = document.activeElement as HTMLElement | null;
      document.addEventListener('keydown', onKeydown);
      await nextTick();
      const items = focusableElements();
      if (items.length > 0) {
        items[0].focus();
      } else {
        panelRef.value?.focus();
      }
    } else {
      document.removeEventListener('keydown', onKeydown);
      previouslyFocused?.focus();
      previouslyFocused = null;
    }
  },
  { immediate: true },
);

onBeforeUnmount(() => {
  document.removeEventListener('keydown', onKeydown);
});
</script>

<template>
  <div v-if="open" class="modal-backdrop" role="presentation">
    <div
      ref="panelRef"
      class="modal-panel"
      role="dialog"
      aria-modal="true"
      :aria-label="title"
      tabindex="-1"
    >
      <h2 style="margin: 0 0 12px 0; font-size: 1.125rem">{{ title }}</h2>
      <div style="margin-bottom: 16px">
        <slot />
      </div>
      <div class="row-actions" style="justify-content: flex-end">
        <button type="button" class="btn" :disabled="busy" @click="emit('cancel')">Cancel</button>
        <button
          type="button"
          class="btn"
          :class="danger ? 'btn-danger' : 'btn-primary'"
          :disabled="busy"
          @click="emit('confirm')"
        >
          {{ confirmLabel }}
        </button>
      </div>
    </div>
  </div>
</template>
