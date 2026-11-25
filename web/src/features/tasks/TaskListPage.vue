<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { apiClient, useSession } from '../../composables/useSession'
import type { Page, TaskSummaryData } from '../../api/types'

const { hasScope } = useSession()
const items = ref<TaskSummaryData[]>([])
const nextCursor = ref('')
const hasMore = ref(false)
const loading = ref(false)
const error = ref<string | null>(null)

async function load(cursor = '') {
  loading.value = true
  error.value = null
  try {
    const page = await apiClient.request<Page<TaskSummaryData>>({
      method: 'GET',
      path: '/api/tasks',
      query: { limit: '50', cursor: cursor || undefined }
    })
    items.value = page.items
    hasMore.value = page.has_more
    nextCursor.value = page.next_cursor
  } catch (err) {
    error.value = err instanceof Error ? err.message : 'Failed to load tasks'
  } finally {
    loading.value = false
  }
}

onMounted(() => void load())
</script>

<template>
  <main class="main">
    <div class="row" style="margin-bottom: 16px">
      <h1 style="margin: 0">Tasks</h1>
      <span class="spacer" />
      <router-link v-if="hasScope('control')" to="/tasks/new" class="btn btn-primary">
        Create / Import Task
      </router-link>
      <button class="btn" @click="load()">Refresh</button>
    </div>

    <div v-if="error" class="error-box" role="alert">{{ error }}</div>

    <section class="card">
      <table v-if="items.length">
        <thead>
          <tr>
            <th>Task ID / Name</th>
            <th>Config Version</th>
            <th>Desired</th>
            <th>Execution</th>
            <th>Data Impact</th>
            <th>Freshness</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="t in items" :key="t.task_id">
            <td>
              <router-link :to="`/tasks/${t.task_id}`" class="mono">{{ t.name || t.task_id }}</router-link>
            </td>
            <td class="mono">{{ t.config_version.slice(0, 18) }}…</td>
            <td><span class="badge badge-info">{{ t.desired_state }}</span></td>
            <td><span class="badge badge-ok">{{ t.execution_condition }}</span></td>
            <td><span class="badge" :class="t.data_impact === 'none' ? 'badge-ok' : 'badge-danger'">{{ t.data_impact }}</span></td>
            <td>{{ t.freshness.state }}</td>
          </tr>
        </tbody>
      </table>
      <div v-else-if="!loading" style="color: var(--muted)">No tasks registered.</div>
      <div v-if="hasMore" style="margin-top: 12px">
        <button class="btn" @click="load(nextCursor)">Next Page</button>
      </div>
    </section>
  </main>
</template>
