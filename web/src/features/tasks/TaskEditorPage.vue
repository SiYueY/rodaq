<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { onBeforeRouteLeave, useRoute, useRouter } from 'vue-router';
import { CLIENT_TIMEOUTS } from '../../api/client';
import type { PortablePipelineConfig, PortableTaskConfig, TaskData } from '../../api/generated';
import { ApiFailure } from '../../api/errors';
import { useSessionStore } from '../../composables/useSession';
import {
  buildDefaultRos2Config,
  buildQualifiedMcapConfig,
  computeIdentityDiff,
  createDefaultTaskDraft,
  eligibleBackendsForRole,
  isOfficialMcapEditorSupported,
  isOfficialRos2EditorSupported,
  serializePortableConfig,
  validateTaskDraft,
  type FieldIssue,
} from './taskEditorModel';
import { useTaskMutation } from './useTaskMutation';
import StatusBadge from '../../components/StatusBadge.vue';
import ErrorPanel from '../../components/ErrorPanel.vue';

const route = useRoute();
const router = useRouter();
const session = useSessionStore();
const mutation = useTaskMutation();

const taskId = computed(() => (route.params.taskId ? String(route.params.taskId) : null));
const mode = computed<'create' | 'configure'>(() => (taskId.value ? 'configure' : 'create'));

const originalConfig = ref<PortableTaskConfig | null>(null);
const baseConfigVersion = ref<string | null>(null);
const baseControlGeneration = ref<string | null>(null);
const draft = ref<PortableTaskConfig>(createDefaultTaskDraft(session.capabilities));
const dirty = ref(false);
const loading = ref(false);
const loadError = ref<ApiFailure | null>(null);
const conflictError = ref<ApiFailure | null>(null);
const submitError = ref<ApiFailure | null>(null);
const validationIssues = ref<FieldIssue[]>([]);

// Raw JSON string buffers per pipeline for non-official or unknown plugin versions.
const acqRawJson = ref<Record<number, string>>({});
const stoRawJson = ref<Record<number, string>>({});
const rawJsonParseErrors = ref<Record<string, string>>({});

const acqBackends = computed(() => eligibleBackendsForRole(session.capabilities, 'acquisition'));
const stoBackends = computed(() => eligibleBackendsForRole(session.capabilities, 'storage'));
const diffSummary = computed(() => computeIdentityDiff(originalConfig.value, draft.value));

function syncRawBuffersFromDraft(): void {
  const aMap: Record<number, string> = {};
  const sMap: Record<number, string> = {};
  draft.value.pipelines.forEach((p, idx) => {
    aMap[idx] = JSON.stringify(p.acquisition.config ?? {}, null, 2);
    sMap[idx] = JSON.stringify(p.storage.config ?? {}, null, 2);
  });
  acqRawJson.value = aMap;
  stoRawJson.value = sMap;
  rawJsonParseErrors.value = {};
}

function versionsForBackend(role: 'acquisition' | 'storage', pluginId: string): string[] {
  const list = role === 'acquisition' ? acqBackends.value : stoBackends.value;
  return list.filter((b) => b.plugin_id === pluginId).map((b) => b.plugin_version);
}

function distinctPluginIds(role: 'acquisition' | 'storage'): string[] {
  const list = role === 'acquisition' ? acqBackends.value : stoBackends.value;
  return Array.from(new Set(list.map((b) => b.plugin_id)));
}

function onPluginChange(pIdx: number, role: 'acquisition' | 'storage'): void {
  dirty.value = true;
  const p = draft.value.pipelines[pIdx];
  const versions = versionsForBackend(
    role,
    role === 'acquisition' ? p.acquisition.plugin_id : p.storage.plugin_id,
  );
  if (role === 'acquisition') {
    if (!versions.includes(p.acquisition.plugin_version) && versions.length > 0) {
      p.acquisition.plugin_version = versions[0];
    }
    if (isOfficialRos2EditorSupported(p.acquisition.plugin_id, p.acquisition.plugin_version)) {
      p.acquisition.config = buildDefaultRos2Config(p.streams.map((s) => s.stream_key));
    }
    acqRawJson.value[pIdx] = JSON.stringify(p.acquisition.config, null, 2);
  } else {
    if (!versions.includes(p.storage.plugin_version) && versions.length > 0) {
      p.storage.plugin_version = versions[0];
    }
    if (isOfficialMcapEditorSupported(p.storage.plugin_id, p.storage.plugin_version)) {
      p.storage.config = buildQualifiedMcapConfig();
    }
    stoRawJson.value[pIdx] = JSON.stringify(p.storage.config, null, 2);
  }
}

function addPipeline(): void {
  dirty.value = true;
  const template = createDefaultTaskDraft(session.capabilities).pipelines[0];
  draft.value.pipelines.push(template);
  syncRawBuffersFromDraft();
}

function removePipeline(pIdx: number): void {
  dirty.value = true;
  draft.value.pipelines.splice(pIdx, 1);
  syncRawBuffersFromDraft();
}

function addStream(p: PortablePipelineConfig, pIdx: number): void {
  dirty.value = true;
  const key = `s${p.streams.length}`;
  p.streams.push({ stream_key: key, name: key, readiness_policy: 'entry_ready' });
  syncRos2StreamsIfOfficial(p, pIdx);
}

function removeStream(p: PortablePipelineConfig, pIdx: number, sIdx: number): void {
  dirty.value = true;
  p.streams.splice(sIdx, 1);
  syncRos2StreamsIfOfficial(p, pIdx);
}

// Existing stream identity cannot rebind stream_key in place; Replace deletes old identity + adds new stream (WEB-E2E-016).
function replaceStream(p: PortablePipelineConfig, pIdx: number, sIdx: number): void {
  dirty.value = true;
  const old = p.streams[sIdx];
  p.streams.splice(sIdx, 1, {
    stream_key: `${old.stream_key}_new`,
    name: old.name,
    readiness_policy: old.readiness_policy,
  });
  syncRos2StreamsIfOfficial(p, pIdx);
}

function syncRos2StreamsIfOfficial(p: PortablePipelineConfig, pIdx: number): void {
  if (!isOfficialRos2EditorSupported(p.acquisition.plugin_id, p.acquisition.plugin_version)) {
    return;
  }
  const existing = Array.isArray(p.acquisition.config.streams)
    ? (p.acquisition.config.streams as Record<string, unknown>[])
    : [];
  const byKey = new Map<string, Record<string, unknown>>();
  for (const entry of existing) {
    if (typeof entry.stream_key === 'string') {
      byKey.set(entry.stream_key, entry);
    }
  }
  p.acquisition.config.streams = p.streams.map((s) => {
    const prev = byKey.get(s.stream_key);
    return (
      prev ?? {
        stream_key: s.stream_key,
        topic: `/${s.stream_key}`,
        qos: { mode: 'adapt', depth: '10' },
      }
    );
  });
  acqRawJson.value[pIdx] = JSON.stringify(p.acquisition.config, null, 2);
}

interface Ros2StreamBinding {
  stream_key: string;
  topic: string;
  type?: string;
  qos: { mode: 'adapt' | 'explicit'; depth: string; reliability?: string; durability?: string };
}

function getRos2Streams(p: PortablePipelineConfig): Ros2StreamBinding[] {
  if (!Array.isArray(p.acquisition.config.streams)) {
    p.acquisition.config.streams = [];
  }
  return p.acquisition.config.streams as Ros2StreamBinding[];
}

function onRos2QosModeChange(binding: Ros2StreamBinding): void {
  if (binding.qos.mode === 'adapt') {
    binding.qos = { mode: 'adapt', depth: binding.qos.depth || '10' };
  } else {
    binding.qos = {
      mode: 'explicit',
      depth: binding.qos.depth || '10',
      reliability: binding.qos.reliability ?? 'reliable',
      durability: binding.qos.durability ?? 'volatile',
    };
  }
}

function parseRawPluginConfigs(): boolean {
  rawJsonParseErrors.value = {};
  let ok = true;
  draft.value.pipelines.forEach((p, idx) => {
    if (!isOfficialRos2EditorSupported(p.acquisition.plugin_id, p.acquisition.plugin_version)) {
      try {
        const parsed = JSON.parse(acqRawJson.value[idx] ?? '{}') as unknown;
        if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
          throw new Error('Acquisition config must be a JSON object');
        }
        p.acquisition.config = parsed as Record<string, unknown>;
      } catch (err) {
        rawJsonParseErrors.value[`acq-${idx}`] =
          err instanceof Error ? err.message : 'Invalid JSON';
        ok = false;
      }
    }
    if (!isOfficialMcapEditorSupported(p.storage.plugin_id, p.storage.plugin_version)) {
      try {
        const parsed = JSON.parse(stoRawJson.value[idx] ?? '{}') as unknown;
        if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
          throw new Error('Storage config must be a JSON object');
        }
        p.storage.config = parsed as Record<string, unknown>;
      } catch (err) {
        rawJsonParseErrors.value[`sto-${idx}`] =
          err instanceof Error ? err.message : 'Invalid JSON';
        ok = false;
      }
    }
  });
  return ok;
}

async function loadExistingForConfigure(): Promise<void> {
  if (!taskId.value) {
    return;
  }
  loading.value = true;
  loadError.value = null;
  conflictError.value = null;
  try {
    const [taskSnap, cfgExport] = await Promise.all([
      session.api.request<TaskData>({
        method: 'GET',
        path: `/api/tasks/${encodeURIComponent(taskId.value)}`,
        timeoutMs: CLIENT_TIMEOUTS.query,
      }),
      session.api.requestRawConfig(`/api/tasks/${encodeURIComponent(taskId.value)}/config`),
    ]);
    baseConfigVersion.value = taskSnap.config_version;
    baseControlGeneration.value = taskSnap.control_generation;
    originalConfig.value = JSON.parse(cfgExport.rawJson) as PortableTaskConfig;
    draft.value = JSON.parse(cfgExport.rawJson) as PortableTaskConfig;
    syncRawBuffersFromDraft();
    dirty.value = false;
  } catch (err) {
    if (err instanceof ApiFailure) {
      loadError.value = err;
    }
  } finally {
    loading.value = false;
  }
}

async function submit(): Promise<void> {
  submitError.value = null;
  conflictError.value = null;
  if (!parseRawPluginConfigs()) {
    return;
  }
  validationIssues.value = validateTaskDraft(draft.value, session.capabilities);
  if (validationIssues.value.length > 0) {
    return;
  }
  const body = serializePortableConfig(draft.value);
  try {
    if (mode.value === 'create') {
      const created = await mutation.submitCreateTask(body);
      dirty.value = false;
      await router.push(`/tasks/${encodeURIComponent(created.task_id)}`);
    } else if (taskId.value) {
      await mutation.submitConfigureTask(taskId.value, body, baseControlGeneration.value);
      dirty.value = false;
      await router.push(`/tasks/${encodeURIComponent(taskId.value)}`);
    }
  } catch (err) {
    if (err instanceof ApiFailure) {
      if (err.httpStatus === 409 && mode.value === 'configure') {
        // Never overwrite user draft automatically on 409 generation conflict (WEB-E2E-016).
        conflictError.value = err;
      } else {
        submitError.value = err;
      }
    }
  }
}

function handleBeforeUnload(ev: BeforeUnloadEvent): void {
  if (dirty.value) {
    ev.preventDefault();
    ev.returnValue = '';
  }
}

onBeforeRouteLeave(() => {
  if (!dirty.value) {
    return true;
  }
  return window.confirm('You have unsaved Task draft changes. Discard and leave?');
});

onMounted(async () => {
  window.addEventListener('beforeunload', handleBeforeUnload);
  if (!session.capabilities && session.lifecycle === 'AuthenticatedReady') {
    await session.refreshCapabilities();
  }
  if (mode.value === 'configure') {
    await loadExistingForConfigure();
  } else {
    draft.value = createDefaultTaskDraft(session.capabilities);
    syncRawBuffersFromDraft();
  }
});

onBeforeUnmount(() => {
  window.removeEventListener('beforeunload', handleBeforeUnload);
});
</script>

<template>
  <section>
    <div class="page-header">
      <h1>{{ mode === 'create' ? 'Create Task' : `Configure Task ${taskId}` }}</h1>
      <div class="row-actions">
        <button type="button" class="btn" @click="router.back()">Cancel</button>
      </div>
    </div>

    <ErrorPanel v-if="loadError" :error="loadError" title="Failed to Load Active Config" />
    <ErrorPanel v-if="submitError" :error="submitError" title="Submission Failed" />

    <section v-if="conflictError" class="card" role="alert" style="border-color: #f59e0b">
      <h2 class="card-title">Control Generation Conflict (409)</h2>
      <p>
        Another operation updated this Task after you loaded generation
        <code class="mono">{{ baseControlGeneration }}</code>. Your draft has been preserved so you
        can inspect changes before deciding.
      </p>
      <div class="row-actions">
        <button type="button" class="btn btn-primary" @click="loadExistingForConfigure">
          Reload Latest Server Config
        </button>
        <button type="button" class="btn" @click="conflictError = null">
          Keep Editing Draft
        </button>
      </div>
    </section>

    <section v-if="validationIssues.length > 0" class="card" role="alert" style="border-color: #ef4444">
      <h2 class="card-title">Validation Issues</h2>
      <ul style="margin: 0; padding-left: 18px">
        <li v-for="(issue, idx) in validationIssues" :key="idx">
          <code class="mono">{{ issue.path }}</code>: {{ issue.message }}
        </li>
      </ul>
    </section>

    <div v-if="loading" class="skeleton" aria-hidden="true" />

    <form v-else @submit.prevent="submit" @input="dirty = true">
      <article class="card">
        <h2 class="card-title">Recording &amp; Policy</h2>
        <div class="grid-3">
          <div class="form-field">
            <label for="task-name">Task Name (optional, ≤ 128 UTF-8 bytes)</label>
            <input id="task-name" v-model="draft.name" type="text" placeholder="e.g. bench-run-1" />
          </div>
          <div class="form-field">
            <label for="recording-mode">Recording Mode</label>
            <select id="recording-mode" v-model="draft.recording_mode">
              <option value="continuous">continuous</option>
              <option value="rolling">rolling</option>
            </select>
          </div>
          <div class="form-field">
            <label for="start-policy">Start Policy</label>
            <select id="start-policy" v-model="draft.start_policy.mode">
              <option value="all_required">all_required</option>
              <option value="best_effort">best_effort</option>
            </select>
          </div>
          <div class="form-field">
            <label for="recovery-policy">Recovery Policy (Auto Re-admission After Restart)</label>
            <select id="recovery-policy" v-model="draft.recovery_policy.mode">
              <option value="manual">manual</option>
              <option value="resume">resume</option>
            </select>
          </div>
          <div class="form-field">
            <label for="seg-duration">Segment Max Duration (seconds, optional)</label>
            <input
              id="seg-duration"
              :value="draft.segment_policy.max_duration?.value ?? ''"
              type="text"
              placeholder="60"
              @input="
                (ev) => {
                  const v = (ev.target as HTMLInputElement).value.trim();
                  draft.segment_policy.max_duration = v ? { value: v, unit: 's' } : null;
                }
              "
            />
          </div>
          <div class="form-field">
            <label for="seg-bytes">Segment Max Physical Bytes (decimal uint64, optional)</label>
            <input
              id="seg-bytes"
              :value="draft.segment_policy.max_physical_bytes ?? ''"
              type="text"
              placeholder="e.g. 536870912"
              @input="
                (ev) => {
                  const v = (ev.target as HTMLInputElement).value.trim();
                  draft.segment_policy.max_physical_bytes = v || null;
                }
              "
            />
          </div>
        </div>

        <div v-if="draft.recording_mode === 'rolling'" class="grid-3" style="margin-top: 8px">
          <div class="form-field">
            <label for="ret-dur">Retention Max Duration (seconds)</label>
            <input
              id="ret-dur"
              :value="draft.retention_policy.max_duration?.value ?? ''"
              type="text"
              @input="
                (ev) => {
                  const v = (ev.target as HTMLInputElement).value.trim();
                  draft.retention_policy.mode = 'rolling';
                  draft.retention_policy.max_duration = v ? { value: v, unit: 's' } : null;
                }
              "
            />
          </div>
          <div class="form-field">
            <label for="ret-bytes">Retention Max Physical Bytes (decimal uint64)</label>
            <input
              id="ret-bytes"
              :value="draft.retention_policy.max_physical_bytes ?? ''"
              type="text"
              @input="
                (ev) => {
                  const v = (ev.target as HTMLInputElement).value.trim();
                  draft.retention_policy.mode = 'rolling';
                  draft.retention_policy.max_physical_bytes = v || null;
                }
              "
            />
          </div>
        </div>
      </article>

      <article v-for="(p, pIdx) in draft.pipelines" :key="pIdx" class="card">
        <div class="row-actions" style="justify-content: space-between; margin-bottom: 10px">
          <h2 class="card-title" style="margin: 0">
            Pipeline #{{ pIdx + 1 }}
            <StatusBadge
              v-if="p.pipeline_id"
              :label="`Existing ID: ${p.pipeline_id}`"
              tone="info"
            />
            <StatusBadge v-else label="New Pipeline (Core assigns ID)" tone="neutral" />
          </h2>
          <button
            type="button"
            class="btn btn-danger"
            :disabled="draft.pipelines.length <= 1"
            @click="removePipeline(pIdx)"
          >
            Remove Pipeline
          </button>
        </div>

        <div class="grid-3">
          <div class="form-field">
            <label>
              <input v-model="p.required" type="checkbox" /> Required Pipeline
            </label>
          </div>
          <div class="form-field">
            <label :for="`profile-${pIdx}`">Resource Profile</label>
            <select :id="`profile-${pIdx}`" v-model="p.resource_profile">
              <option
                v-for="prof in session.capabilities?.resource_profiles ?? [{ name: p.resource_profile }]"
                :key="prof.name"
                :value="prof.name"
              >
                {{ prof.name }}
              </option>
            </select>
          </div>
          <div class="form-field">
            <label :for="`root-${pIdx}`">Output Root</label>
            <select :id="`root-${pIdx}`" v-model="p.output_root">
              <option
                v-for="r in session.capabilities?.output_roots ?? [{ name: p.output_root }]"
                :key="r.name"
                :value="r.name"
              >
                {{ r.name }}
              </option>
            </select>
          </div>
        </div>

        <!-- Acquisition Plugin Selection -->
        <div class="grid-2">
          <div class="form-field">
            <label :for="`acq-id-${pIdx}`">Acquisition Plugin ID</label>
            <select
              :id="`acq-id-${pIdx}`"
              v-model="p.acquisition.plugin_id"
              @change="onPluginChange(pIdx, 'acquisition')"
            >
              <option v-for="id in distinctPluginIds('acquisition')" :key="id" :value="id">
                {{ id }}
              </option>
            </select>
          </div>
          <div class="form-field">
            <label :for="`acq-ver-${pIdx}`">Acquisition Exact Version</label>
            <select
              :id="`acq-ver-${pIdx}`"
              v-model="p.acquisition.plugin_version"
              @change="onPluginChange(pIdx, 'acquisition')"
            >
              <option
                v-for="v in versionsForBackend('acquisition', p.acquisition.plugin_id)"
                :key="v"
                :value="v"
              >
                {{ v }}
              </option>
            </select>
          </div>
        </div>

        <div
          v-if="isOfficialRos2EditorSupported(p.acquisition.plugin_id, p.acquisition.plugin_version)"
          class="card"
          style="background: var(--bg-subtle)"
        >
          <strong>Official ROS 2 Acquisition Editor (format_version 1.0)</strong>
          <div class="grid-2" style="margin-top: 8px">
            <div class="form-field">
              <label>Domain ID (0..232)</label>
              <input v-model.number="p.acquisition.config.domain_id" type="number" min="0" max="232" />
            </div>
            <div class="form-field">
              <label>Executor Threads</label>
              <input v-model.number="p.acquisition.config.executor_threads" type="number" min="1" max="16" />
            </div>
          </div>
          <div style="margin-top: 8px">
            <div class="muted" style="font-size: 0.85rem; margin-bottom: 6px">
              ROS 2 Topic &amp; QoS Bindings (per stream_key)
            </div>
            <div
              v-for="(rs, rIdx) in getRos2Streams(p)"
              :key="rs.stream_key"
              class="grid-4"
              style="margin-bottom: 6px"
            >
              <div class="form-field" style="margin-bottom: 0">
                <label :for="`ros2-topic-${pIdx}-${rIdx}`">Topic ({{ rs.stream_key }})</label>
                <input :id="`ros2-topic-${pIdx}-${rIdx}`" v-model="rs.topic" type="text" class="mono" />
              </div>
              <div class="form-field" style="margin-bottom: 0">
                <label :for="`ros2-qos-${pIdx}-${rIdx}`">QoS Mode</label>
                <select
                  :id="`ros2-qos-${pIdx}-${rIdx}`"
                  v-model="rs.qos.mode"
                  @change="onRos2QosModeChange(rs)"
                >
                  <option value="adapt">adapt</option>
                  <option value="explicit">explicit</option>
                </select>
              </div>
              <div class="form-field" style="margin-bottom: 0">
                <label :for="`ros2-depth-${pIdx}-${rIdx}`">QoS Depth (decimal)</label>
                <input :id="`ros2-depth-${pIdx}-${rIdx}`" v-model="rs.qos.depth" type="text" class="mono" />
              </div>
              <div v-if="rs.qos.mode === 'explicit'" class="form-field" style="margin-bottom: 0">
                <label>Reliability / Durability</label>
                <div class="row-actions">
                  <select v-model="rs.qos.reliability">
                    <option value="reliable">reliable</option>
                    <option value="best_effort">best_effort</option>
                  </select>
                  <select v-model="rs.qos.durability">
                    <option value="volatile">volatile</option>
                    <option value="transient_local">transient_local</option>
                  </select>
                </div>
              </div>
            </div>
          </div>
        </div>
        <div v-else class="form-field">
          <label :for="`acq-raw-${pIdx}`">Acquisition Private Config (Raw JSON)</label>
          <textarea
            :id="`acq-raw-${pIdx}`"
            v-model="acqRawJson[pIdx]"
            class="mono"
            rows="4"
            spellcheck="false"
          />
          <span v-if="rawJsonParseErrors[`acq-${pIdx}`]" class="field-error">
            {{ rawJsonParseErrors[`acq-${pIdx}`] }}
          </span>
        </div>

        <!-- Storage Plugin Selection -->
        <div class="grid-2">
          <div class="form-field">
            <label :for="`sto-id-${pIdx}`">Storage Plugin ID</label>
            <select
              :id="`sto-id-${pIdx}`"
              v-model="p.storage.plugin_id"
              @change="onPluginChange(pIdx, 'storage')"
            >
              <option v-for="id in distinctPluginIds('storage')" :key="id" :value="id">
                {{ id }}
              </option>
            </select>
          </div>
          <div class="form-field">
            <label :for="`sto-ver-${pIdx}`">Storage Exact Version</label>
            <select
              :id="`sto-ver-${pIdx}`"
              v-model="p.storage.plugin_version"
              @change="onPluginChange(pIdx, 'storage')"
            >
              <option
                v-for="v in versionsForBackend('storage', p.storage.plugin_id)"
                :key="v"
                :value="v"
              >
                {{ v }}
              </option>
            </select>
          </div>
        </div>

        <div
          v-if="isOfficialMcapEditorSupported(p.storage.plugin_id, p.storage.plugin_version)"
          class="card"
          style="background: var(--bg-subtle)"
        >
          <strong>Qualified MCAP Baseline (format_version 1.0 · read-only)</strong>
          <div class="mono muted" style="margin-top: 4px">
            compression=none · chunk_mode=none · chunk_crc=false · message_index=false · summary=false · rosbag2_metadata=legacy_ros_distro
          </div>
        </div>
        <div v-else class="form-field">
          <label :for="`sto-raw-${pIdx}`">Storage Private Config (Raw JSON)</label>
          <textarea
            :id="`sto-raw-${pIdx}`"
            v-model="stoRawJson[pIdx]"
            class="mono"
            rows="3"
            spellcheck="false"
          />
          <span v-if="rawJsonParseErrors[`sto-${pIdx}`]" class="field-error">
            {{ rawJsonParseErrors[`sto-${pIdx}`] }}
          </span>
        </div>

        <!-- Streams -->
        <div class="row-actions" style="justify-content: space-between; margin: 12px 0 8px 0">
          <strong>Streams ({{ p.streams.length }})</strong>
          <button type="button" class="btn" @click="addStream(p, pIdx)">Add Stream</button>
        </div>

        <div
          v-for="(s, sIdx) in p.streams"
          :key="sIdx"
          class="grid-4"
          style="padding: 8px; border: 1px solid var(--border-subtle); border-radius: 4px; margin-bottom: 8px"
        >
          <div class="form-field" style="margin-bottom: 0">
            <label>Stream Key {{ s.stream_id ? `(locked ID ${s.stream_id})` : '(new)' }}</label>
            <input
              v-model="s.stream_key"
              type="text"
              :disabled="Boolean(s.stream_id)"
              @change="syncRos2StreamsIfOfficial(p, pIdx)"
            />
          </div>
          <div class="form-field" style="margin-bottom: 0">
            <label>Display Name</label>
            <input v-model="s.name" type="text" />
          </div>
          <div class="form-field" style="margin-bottom: 0">
            <label>Readiness Policy</label>
            <select v-model="s.readiness_policy">
              <option value="entry_ready">entry_ready</option>
              <option value="source_ready">source_ready</option>
            </select>
          </div>
          <div class="row-actions" style="align-self: end">
            <button
              v-if="s.stream_id"
              type="button"
              class="btn"
              @click="replaceStream(p, pIdx, sIdx)"
            >
              Replace Stream
            </button>
            <button
              type="button"
              class="btn btn-danger"
              :disabled="p.streams.length <= 1"
              @click="removeStream(p, pIdx, sIdx)"
            >
              Remove
            </button>
          </div>
        </div>
      </article>

      <div class="row-actions" style="margin-bottom: 16px">
        <button type="button" class="btn" @click="addPipeline">Add Pipeline</button>
      </div>

      <!-- Review Summary -->
      <article class="card">
        <h2 class="card-title">Review Summary</h2>
        <div style="font-size: 0.875rem">
          <div>
            Mode: <strong>{{ draft.recording_mode }}</strong> · Start: <strong>{{ draft.start_policy.mode }}</strong> · Recovery: <strong>{{ draft.recovery_policy.mode }}</strong> · Pipelines: <strong>{{ draft.pipelines.length }}</strong>
          </div>
          <div v-if="mode === 'configure'" class="mono muted" style="margin-top: 6px">
            Preserved pipelines: {{ diffSummary.preservedPipelines.join(', ') || 'none' }} ·
            Added pipelines: {{ diffSummary.addedPipelines }} ·
            Removed pipelines: {{ diffSummary.removedPipelines.join(', ') || 'none' }} |
            Removed streams: {{ diffSummary.removedStreams.join(', ') || 'none' }}
          </div>
        </div>
        <div class="row-actions" style="margin-top: 12px">
          <button type="submit" class="btn btn-primary" :disabled="mutation.submitting.value">
            {{
              mutation.submitting.value
                ? 'Submitting…'
                : mode === 'create'
                  ? 'Create Task'
                  : 'Submit Configure'
            }}
          </button>
        </div>
      </article>
    </form>
  </section>
</template>
