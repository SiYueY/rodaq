<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { apiClient } from '../../composables/useSession'
import { formatBytesDecimal, formatDuration } from '../../format/format'
import type { ArtifactData, ArtifactFileData, ArtifactSummaryData, Page } from '../../api/types'

const items = ref<ArtifactSummaryData[]>([])
const selected = ref<ArtifactData | null>(null)
const filterTaskId = ref('')
const filterFormat = ref('')
const nextCursor = ref('')
const hasMore = ref(false)
const error = ref<string | null>(null)
const downloadNotice = ref<string | null>(null)

async function load(cursor = '') {
  error.value = null
  try {
    const page = await apiClient.request<Page<ArtifactSummaryData>>({
      method: 'GET',
      path: '/api/artifacts',
      query: {
        task_id: filterTaskId.value.trim() || undefined,
        format: filterFormat.value.trim() || undefined,
        cursor: cursor || undefined,
        limit: '50'
      }
    })
    items.value = page.items
    hasMore.value = page.has_more
    nextCursor.value = page.next_cursor
  } catch (err) {
    error.value = err instanceof Error ? err.message : 'Failed to query artifacts'
  }
}

async function openDetail(artifactId: string) {
  error.value = null
  downloadNotice.value = null
  try {
    selected.value = await apiClient.request<ArtifactData>({
      method: 'GET',
      path: `/api/artifacts/${artifactId}`
    })
  } catch (err) {
    error.value = err instanceof Error ? err.message : 'Failed to load artifact detail'
  }
}

// Web §2.6: HEAD preflight then same-origin browser navigation (never Blob-buffer large files).
async function downloadFile(file: ArtifactFileData) {
  error.value = null
  downloadNotice.value = null
  try {
    await apiClient.request<void>({
      method: 'HEAD',
      path: file.download_url,
      timeoutMs: 10000
    })
    window.location.assign(file.download_url)
    downloadNotice.value =
      'Download initiated. If concurrent retention/reset occurred after preflight, refresh artifact status and retry.'
  } catch (err) {
    error.value = err instanceof Error ? err.message : 'Artifact file unavailable for download'
  }
}

onMounted(() => void load())
</script>

<template>
  <main class="main">
    <div class="row" style="margin-bottom: 16px">
      <h1 style="margin: 0">Artifacts</h1>
      <span class="spacer" />
      <input v-model="filterTaskId" placeholder="Filter by task_id" aria-label="Filter by task ID" />
      <input v-model="filterFormat" placeholder="Format (e.g. mcap)" aria-label="Filter by format" />
      <button class="btn btn-primary" @click="load()">Query</button>
    </div>

    <div v-if="error" class="error-box" role="alert">{{ error }}</div>
    <div v-if="downloadNotice" class="card" role="status">{{ downloadNotice }}</div>

    <section class="card">
      <table v-if="items.length">
        <thead>
          <tr>
            <th>Artifact ID</th>
            <th>Task ID</th>
            <th>Committed At</th>
            <th>Recording Range</th>
            <th>Format</th>
            <th class="num">Physical</th>
            <th>Data Impact</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="a in items" :key="a.artifact_id">
            <td>
              <button class="btn" @click="openDetail(a.artifact_id)">{{ a.artifact_id }}</button>
            </td>
            <td class="mono">{{ a.task_id }}</td>
            <td class="mono">{{ a.committed_at }}</td>
            <td class="mono">
              {{ formatDuration(a.recording_range.begin) }} → {{ formatDuration(a.recording_range.end) }}
            </td>
            <td>{{ a.format }}</td>
            <td class="num mono">{{ formatBytesDecimal(a.physical_bytes) }}</td>
            <td>{{ a.data_impact }}</td>
          </tr>
        </tbody>
      </table>
      <div v-else style="color: var(--muted)">No committed artifacts found.</div>
      <div v-if="hasMore" style="margin-top: 12px">
        <button class="btn" @click="load(nextCursor)">Next Page</button>
      </div>
    </section>

    <section v-if="selected" class="card">
      <h2 style="margin-top: 0">Artifact {{ selected.artifact_id }}</h2>
      <p class="mono" style="color: var(--muted)">
        Coverage: {{ selected.coverage.complete ? 'Complete' : `Gaps (${selected.coverage.gap_count})` }} ·
        Logical: {{ formatBytesDecimal(selected.logical_bytes) }} ·
        Physical: {{ formatBytesDecimal(selected.physical_bytes) }}
      </p>
      <table>
        <thead>
          <tr>
            <th>File</th>
            <th>Media Type</th>
            <th class="num">Size</th>
            <th>Action</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="f in selected.files" :key="f.file_id">
            <td class="mono">{{ f.display_name }}</td>
            <td>{{ f.media_type }}</td>
            <td class="num mono">{{ formatBytesDecimal(f.size) }}</td>
            <td>
              <button class="btn btn-primary" @click="downloadFile(f)">Download</button>
            </td>
          </tr>
        </tbody>
      </table>
    </section>
  </main>
</template>
