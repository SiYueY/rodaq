<script setup lang="ts">
import { computed, ref } from 'vue';
import { RouterLink } from 'vue-router';
import type { OperationKind, TaskData } from '../../api/generated';
import ConfirmDialog from '../../components/ConfirmDialog.vue';

const props = defineProps<{
  task: TaskData;
  canControl: boolean;
  canAdmin: boolean;
  busy: boolean;
}>();

const emit = defineEmits<{
  operate: [kind: OperationKind];
}>();

const confirmKind = ref<'reset' | 'retire' | null>(null);

const isStoppedIdle = computed(
  () => props.task.desired_state === 'stopped' && props.task.execution_condition === 'idle',
);

const canStart = computed(
  () =>
    props.canControl &&
    !props.busy &&
    (isStoppedIdle.value || props.task.execution_condition === 'failed'),
);

const canPause = computed(
  () =>
    props.canControl &&
    !props.busy &&
    props.task.desired_state === 'running' &&
    (props.task.execution_condition === 'active' || props.task.execution_condition === 'degraded'),
);

const canResume = computed(
  () =>
    props.canControl &&
    !props.busy &&
    props.task.desired_state === 'paused' &&
    props.task.execution_condition === 'idle',
);

// Stop stays reachable in Running/Paused/Degraded/Protected/Preparing/Draining/PendingCleanup/Failed (WEB-E2E-005).
const canStop = computed(
  () =>
    props.canControl &&
    !props.busy &&
    (props.task.desired_state !== 'stopped' || props.task.execution_condition !== 'idle'),
);

const canConfigure = computed(() => props.canControl && isStoppedIdle.value);
const canResetOrRetire = computed(() => props.canAdmin && !props.busy && isStoppedIdle.value);

function confirmDanger(): void {
  const k = confirmKind.value;
  confirmKind.value = null;
  if (k) {
    emit('operate', k);
  }
}
</script>

<template>
  <section class="card" aria-label="Task Controls">
    <div class="row-actions" style="justify-content: space-between">
      <div class="row-actions">
        <button
          v-if="canControl"
          type="button"
          class="btn btn-primary"
          :disabled="!canStart"
          @click="emit('operate', 'start')"
        >
          Start
        </button>
        <button
          v-if="canControl"
          type="button"
          class="btn"
          :disabled="!canPause"
          @click="emit('operate', 'pause')"
        >
          Pause
        </button>
        <button
          v-if="canControl"
          type="button"
          class="btn"
          :disabled="!canResume"
          @click="emit('operate', 'resume')"
        >
          Resume
        </button>
        <button
          v-if="canControl"
          type="button"
          class="btn btn-danger"
          :disabled="!canStop"
          @click="emit('operate', 'stop')"
        >
          Stop
        </button>
        <RouterLink
          v-if="canConfigure"
          :to="`/tasks/${encodeURIComponent(task.task_id)}/config`"
          class="btn"
        >
          Configure
        </RouterLink>
      </div>

      <div v-if="canAdmin" class="row-actions">
        <button
          type="button"
          class="btn btn-danger"
          :disabled="!canResetOrRetire"
          @click="confirmKind = 'reset'"
        >
          Reset Task
        </button>
        <button
          type="button"
          class="btn btn-danger"
          :disabled="!canResetOrRetire"
          @click="confirmKind = 'retire'"
        >
          Retire Task
        </button>
      </div>
    </div>

    <ConfirmDialog
      :open="confirmKind === 'reset'"
      title="Confirm Reset Task"
      confirm-label="Reset Task"
      :danger="true"
      :busy="busy"
      @cancel="confirmKind = null"
      @confirm="confirmDanger"
    >
      <p>
        Task <code class="mono">{{ task.task_id }}</code> (generation
        <code class="mono">{{ task.control_generation }}</code>) will be reset.
      </p>
      <p>
        <strong>Consequence:</strong> All committed artifacts belonging to this Task will be
        permanently deleted and reset generation will advance. Active ReadLeases may block completion.
      </p>
    </ConfirmDialog>

    <ConfirmDialog
      :open="confirmKind === 'retire'"
      title="Confirm Retire Task"
      confirm-label="Retire Task"
      :danger="true"
      :busy="busy"
      @cancel="confirmKind = null"
      @confirm="confirmDanger"
    >
      <p>
        Task <code class="mono">{{ task.task_id }}</code> will be permanently retired (irreversible).
      </p>
      <p>
        <strong>Preconditions:</strong> Task must be Stopped + Idle and eligible for artifact cleanup
        under Core authority. Once retired, this Task ID cannot be restarted.
      </p>
    </ConfirmDialog>
  </section>
</template>
