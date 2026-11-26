<script setup lang="ts">
import { onBeforeUnmount } from 'vue';
import type { ArtifactData } from '../../api/generated';
import { formatIecBytes } from '../../format/decimal';
import { formatDuration, formatTimestampUtc } from '../../format/time';
import { useArtifactDownload } from './useArtifactDownload';
import StatusBadge from '../../components/StatusBadge.vue';
import ErrorPanel from '../../components/ErrorPanel.vue';

defineProps<{
  artifact: ArtifactData;
}>();

const download = useArtifactDownload();

onBeforeUnmount(() => {
  download.abortPreflight();
});
</script>

<template>
  <article class="card">
    <div class="row-actions" style="justify-content: space-between; margin-bottom: 10px">
      <h2 class="card-title" style="margin: 0">
        Artifact Detail: <code class="mono">{{ artifact.artifact_id }}</code>
      </h2>
      <StatusBadge
        :label="artifact.coverage.complete ? 'Coverage Complete' : `Coverage Gaps (${artifact.coverage.gap_count})`"
        :tone="artifact.coverage.complete ? 'success' : 'warning'"
      />
    </div>

    <div class="grid-3" style="margin-bottom: 12px; font-size: 0.875rem">
      <div>
        <div>Task: <code class="mono">{{ artifact.task_id }}</code></div>
        <div>Pipeline: <code class="mono">p.{{ artifact.pipeline_id }}</code> · Segment: <code class="mono">{{ artifact.segment_id }}</code></div>
      </div>
      <div>
        <div>Committed At (Wall-Clock): <code class="mono">{{ formatTimestampUtc(artifact.committed_at) }}</code></div>
        <div>
          Recording Range (Task-Local):
          <code class="mono">
            {{ formatDuration(artifact.recording_range.begin) }} → {{ formatDuration(artifact.recording_range.end) }}
          </code>
        </div>
      </div>
      <div>
        <div>Format: <strong>{{ artifact.format }}</strong></div>
        <div>
          Logical: <strong class="mono">{{ formatIecBytes(artifact.logical_bytes) }}</strong> ·
          Physical: <strong class="mono">{{ formatIecBytes(artifact.physical_bytes) }}</strong>
        </div>
      </div>
    </div>

    <div v-if="artifact.coverage.gaps.length > 0" style="margin-bottom: 12px">
      <strong>Coverage Gaps:</strong>
      <ul style="margin: 4px 0 0 18px; padding: 0; font-size: 0.85rem">
        <li v-for="(g, i) in artifact.coverage.gaps" :key="i">
          <StatusBadge :label="g.kind" tone="warning" />
          {{ formatDuration(g.recording_begin) }} – {{ formatDuration(g.recording_end) }}
          <span v-if="g.message"> · {{ g.message }}</span>
        </li>
      </ul>
    </div>

    <ErrorPanel v-if="download.error.value" :error="download.error.value" title="Download Preflight Failed" />
    <div v-if="download.notice.value" class="banner banner-info" role="status">
      <span>{{ download.notice.value }}</span>
    </div>

    <h3 style="margin: 0 0 8px 0; font-size: 0.95rem">Committed Files</h3>
    <div class="table-wrap">
      <table class="data-table">
        <thead>
          <tr>
            <th scope="col">File ID</th>
            <th scope="col">Display Name</th>
            <th scope="col">Media Type</th>
            <th scope="col" class="num">Size</th>
            <th scope="col">ETag</th>
            <th scope="col">Action</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="f in artifact.files" :key="f.file_id">
            <td class="mono">{{ f.file_id }}</td>
            <td>{{ f.display_name }}</td>
            <td class="mono">{{ f.media_type }}</td>
            <td class="num">{{ formatIecBytes(f.size) }} ({{ f.size }} B)</td>
            <td class="mono">{{ f.etag.slice(0, 16) }}…</td>
            <td>
              <button
                type="button"
                class="btn btn-primary"
                :disabled="download.busyFileId.value === f.file_id"
                @click="download.downloadFile(f)"
              >
                {{ download.busyFileId.value === f.file_id ? 'Checking…' : 'Download' }}
              </button>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
  </article>
</template>
