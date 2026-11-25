<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { apiClient, useSession } from '../../composables/useSession'
import {
  computeLocalBodyDigest,
  createPendingMutationStore
} from '../../composables/usePendingMutations'
import type { MutationAcceptedData, TaskCreateData, TaskData } from '../../api/types'

const route = useRoute()
const router = useRouter()
const { state: sessionState } = useSession()
const pendingStore = createPendingMutationStore()

const taskId = computed(() => (route.params.taskId ? String(route.params.taskId) : null))
const mode = computed<'create' | 'configure'>(() => (taskId.value ? 'configure' : 'create'))

const rawJson = ref<string>(
  JSON.stringify(
    {
      schema_version: '1.0',
      name: 'new-recording-task',
      recording_mode: 'continuous',
      start_policy: 'all_required',
      recovery_policy: { mode: 'manual' },
      segment_policy: { max_duration: { value: '60', unit: 's' } },
      retention_policy: { mode: 'keep_all' },
      pipelines: [
        {
          required: true,
          resource_profile: 'basic',
          output_root: 'local',
          acquisition: {
            plugin_id: 'com.rodaq.acquisition.ros2',
            plugin_version: '1.0.0',
            config: {
              format_version: '1.0',
              domain_id: 0,
              executor_threads: 1,
              graph_poll_interval: { value: '500', unit: 'ms' },
              qos_change_debounce: { value: '200', unit: 'ms' },
              streams: [
                {
                  stream_key: 'joint_states',
                  topic: '/joint_states',
                  qos: { mode: 'adapt', depth: 10 }
                }
              ]
            }
          },
          storage: {
            plugin_id: 'com.rodaq.storage.mcap',
            plugin_version: '1.0.0',
            config: {
              format_version: '1.0',
              compression: 'none',
              chunk_mode: 'none',
              chunk_crc: false,
              message_index: false,
              summary: false,
              rosbag2_metadata: 'legacy_ros_distro'
            }
          },
          streams: [
            {
              stream_key: 'joint_states',
              name: 'Joint States',
              readiness_policy: 'entry_ready'
            }
          ]
        }
      ]
    },
    null,
    2
  )
)

const baseControlGeneration = ref<string | null>(null)
const submitting = ref(false)
const error = ref<string | null>(null)

async function loadExisting() {
  if (!taskId.value) return
  try {
    const [task, exported] = await Promise.all([
      apiClient.request<TaskData>({ method: 'GET', path: `/api/tasks/${taskId.value}` }),
      apiClient.request<string>({
        method: 'GET',
        path: `/api/tasks/${taskId.value}/config`,
        rawResponse: true
      })
    ])
    baseControlGeneration.value = task.control_generation
    rawJson.value = exported
  } catch (err) {
    error.value = err instanceof Error ? err.message : 'Failed to load existing config'
  }
}

// Web §2.2 / WEB-E2E-012: Preserve exact imported UTF-8 bytes without JSON.parse->stringify rewrite.
async function onImportFile(ev: Event) {
  const input = ev.target as HTMLInputElement
  const file = input.files?.[0]
  if (!file) return
  rawJson.value = await file.text()
}

async function submit() {
  if (!sessionState.session) return
  submitting.value = true
  error.value = null
  const idemKey = crypto.randomUUID()
  const path = mode.value === 'create' ? '/api/tasks' : `/api/tasks/${taskId.value}/config`
  const method = mode.value === 'create' ? 'POST' : 'PUT'
  const bodyUtf8 = rawJson.value

  const saved = pendingStore.save({
    version: 1,
    method,
    path,
    idempotencyKey: idemKey,
    originPrincipal: sessionState.session.principal,
    requiredScope: 'control',
    controlGeneration: baseControlGeneration.value,
    bodyUtf8,
    bodyDigest: computeLocalBodyDigest(bodyUtf8),
    createdAt: new Date().toISOString(),
    operationId: null
  })
  if (!saved) {
    error.value = 'Too many unsettled pending mutations in this tab.'
    submitting.value = false
    return
  }

  try {
    if (mode.value === 'create') {
      const created = await apiClient.request<TaskCreateData>({
        method: 'POST',
        path,
        bodyText: bodyUtf8,
        idempotencyKey: idemKey
      })
      pendingStore.remove(idemKey)
      await router.push(`/tasks/${created.task_id}`)
    } else {
      await apiClient.request<MutationAcceptedData>({
        method: 'PUT',
        path,
        bodyText: bodyUtf8,
        idempotencyKey: idemKey,
        controlGeneration: baseControlGeneration.value ?? undefined
      })
      pendingStore.remove(idemKey)
      await router.push(`/tasks/${taskId.value}`)
    }
  } catch (err) {
    error.value = err instanceof Error ? err.message : 'Submission failed'
  } finally {
    submitting.value = false
  }
}

onMounted(() => void loadExisting())
</script>

<template>
  <main class="main">
    <div class="row" style="margin-bottom: 16px">
      <h1 style="margin: 0">{{ mode === 'create' ? 'Create / Import Task' : `Configure Task ${taskId}` }}</h1>
      <span class="spacer" />
      <label class="btn" style="cursor: pointer">
        Import Portable JSON
        <input type="file" accept=".json,application/json" style="display: none" @change="onImportFile" />
      </label>
    </div>

    <div v-if="error" class="error-box" role="alert">{{ error }}</div>

    <section class="card">
      <p style="color: var(--muted); margin-top: 0">
        Portable TaskConfig V1 (exact <code>plugin_id</code> + <code>plugin_version</code> +
        <code>recovery_policy.mode</code>; imported UTF-8 bytes are submitted verbatim to Server
        strict JSON validation).
      </p>
      <textarea
        v-model="rawJson"
        class="mono"
        rows="26"
        style="width: 100%; resize: vertical"
        aria-label="Portable TaskConfig JSON"
      />
      <div class="row" style="margin-top: 12px">
        <button class="btn btn-primary" :disabled="submitting" @click="submit">
          {{ submitting ? 'Submitting…' : mode === 'create' ? 'Create Task' : 'Submit Configure' }}
        </button>
        <router-link :to="taskId ? `/tasks/${taskId}` : '/tasks'" class="btn">Cancel</router-link>
      </div>
    </section>
  </main>
</template>
