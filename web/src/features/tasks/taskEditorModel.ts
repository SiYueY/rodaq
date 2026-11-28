import type {
  BackendCapabilityData,
  CapabilitiesData,
  PortablePipelineConfig,
  PortableTaskConfig,
} from '../../api/generated';

export const OFFICIAL_ROS2_PLUGIN_ID = 'com.rodaq.acquisition.ros2';
export const OFFICIAL_MCAP_PLUGIN_ID = 'com.rodaq.storage.mcap';

export const COMPATIBLE_ROS2_VERSIONS = new Set<string>(['1.0.0']);
export const COMPATIBLE_MCAP_VERSIONS = new Set<string>(['1.0.0']);

export interface FieldIssue {
  path: string;
  message: string;
}

export interface IdentityDiffSummary {
  preservedPipelines: string[];
  addedPipelines: number;
  removedPipelines: string[];
  preservedStreams: string[];
  addedStreams: number;
  removedStreams: string[];
}

export function isOfficialRos2EditorSupported(pluginId: string, pluginVersion: string): boolean {
  return pluginId === OFFICIAL_ROS2_PLUGIN_ID && COMPATIBLE_ROS2_VERSIONS.has(pluginVersion);
}

export function isOfficialMcapEditorSupported(pluginId: string, pluginVersion: string): boolean {
  return pluginId === OFFICIAL_MCAP_PLUGIN_ID && COMPATIBLE_MCAP_VERSIONS.has(pluginVersion);
}

export function buildQualifiedMcapConfig(
  profile: 'ros2' | 'generic' = 'ros2',
  rosDistro?: string,
): Record<string, unknown> {
  const cfg: Record<string, unknown> = {
    format_version: '1.0',
    profile,
    compression: 'none',
    chunk_mode: 'none',
    chunk_crc: false,
    message_index: false,
    summary: false,
  };
  if (profile === 'ros2' && rosDistro && rosDistro.trim().length > 0) {
    cfg.ros_distro = rosDistro.trim();
  }
  return cfg;
}

export function buildDefaultRos2Config(streamKeys: string[]): Record<string, unknown> {
  return {
    format_version: '1.0',
    domain_id: 0,
    executor_threads: 2,
    graph_poll_interval: { value: '500', unit: 'ms' },
    qos_change_debounce: { value: '200', unit: 'ms' },
    streams: streamKeys.map((streamKey) => ({
      stream_key: streamKey,
      topic: `/${streamKey}`,
      qos: { mode: 'adapt', depth: '10' },
    })),
  };
}

export function eligibleBackendsForRole(
  caps: CapabilitiesData | null,
  role: 'acquisition' | 'storage',
): BackendCapabilityData[] {
  if (!caps) {
    return [];
  }
  return caps.backends.filter(
    (b) => b.installed && b.allowed && b.role === role && b.support_status !== 'deferred',
  );
}

export function createDefaultTaskDraft(caps: CapabilitiesData | null): PortableTaskConfig {
  const acqList = eligibleBackendsForRole(caps, 'acquisition');
  const stoList = eligibleBackendsForRole(caps, 'storage');
  const acq = acqList[0];
  const sto = stoList[0];
  const defaultProfile = caps?.resource_profiles[0]?.name ?? 'default';
  const defaultRoot = caps?.output_roots[0]?.name ?? 'local';

  const acqPluginId = acq?.plugin_id ?? 'rodaq.fake.acquisition';
  const acqPluginVer = acq?.plugin_version ?? '1.0.0';
  const stoPluginId = sto?.plugin_id ?? 'rodaq.fake.storage';
  const stoPluginVer = sto?.plugin_version ?? '1.0.0';

  const acqConfig = isOfficialRos2EditorSupported(acqPluginId, acqPluginVer)
    ? buildDefaultRos2Config(['s0'])
    : { streams: ['s0'], period_us: 2000 };
  const stoConfig = isOfficialMcapEditorSupported(stoPluginId, stoPluginVer)
    ? buildQualifiedMcapConfig()
    : {};

  return {
    format_version: '1.0',
    name: '',
    segment_policy: {
      max_duration_ms: 60_000,
      max_physical_bytes: null,
    },
    retention_policy: {
      mode: 'keep_all',
      max_duration_ms: null,
      max_physical_bytes: null,
    },
    pipelines: [
      {
        required: true,
        resource_profile: defaultProfile,
        output_root: defaultRoot,
        acquisition: {
          plugin_id: acqPluginId,
          plugin_version: acqPluginVer,
          config: acqConfig,
        },
        storage: {
          plugin_id: stoPluginId,
          plugin_version: stoPluginVer,
          config: stoConfig,
        },
        streams: [
          {
            stream_key: 's0',
            name: 's0',
            readiness_policy: 'entry_ready',
          },
        ],
      },
    ],
  };
}

function isPositiveInteger(n: number | null | undefined): boolean {
  return typeof n === 'number' && Number.isInteger(n) && n > 0;
}

export function validateTaskDraft(
  draft: PortableTaskConfig,
  caps: CapabilitiesData | null,
): FieldIssue[] {
  const issues: FieldIssue[] = [];
  if (draft.format_version !== '1.0') {
    issues.push({ path: '/format_version', message: 'format_version must be 1.0' });
  }
  if (draft.name && new TextEncoder().encode(draft.name).length > 128) {
    issues.push({ path: '/name', message: 'Task name must be <= 128 UTF-8 bytes' });
  }
  const hasSegDur = isPositiveInteger(draft.segment_policy.max_duration_ms);
  const hasSegBytes = isPositiveInteger(draft.segment_policy.max_physical_bytes);
  if (!hasSegDur && !hasSegBytes) {
    issues.push({
      path: '/segment_policy',
      message: 'At least one positive segment bound (max_duration_ms or max_physical_bytes) is required',
    });
  }
  if (draft.retention_policy.mode !== 'keep_all' && draft.retention_policy.mode !== 'rolling') {
    issues.push({ path: '/retention_policy/mode', message: 'retention_policy.mode must be keep_all or rolling' });
  } else if (draft.retention_policy.mode === 'rolling') {
    const hasRetDur = isPositiveInteger(draft.retention_policy.max_duration_ms);
    const hasRetBytes = isPositiveInteger(draft.retention_policy.max_physical_bytes);
    if (!hasRetDur && !hasRetBytes) {
      issues.push({
        path: '/retention_policy',
        message: 'Rolling retention requires at least one positive bound (max_duration_ms or max_physical_bytes)',
      });
    }
  }

  if (!draft.pipelines.length) {
    issues.push({ path: '/pipelines', message: 'At least one pipeline is required' });
  }

  draft.pipelines.forEach((p: PortablePipelineConfig, pIdx: number) => {
    const base = `/pipelines/${pIdx}`;
    if (!p.resource_profile) {
      issues.push({ path: `${base}/resource_profile`, message: 'Resource profile required' });
    }
    if (!p.output_root) {
      issues.push({ path: `${base}/output_root`, message: 'Output root required' });
    }
    if (!p.acquisition.plugin_id || !p.acquisition.plugin_version) {
      issues.push({
        path: `${base}/acquisition`,
        message: 'Exact acquisition plugin_id and plugin_version are required',
      });
    }
    if (!p.storage.plugin_id || !p.storage.plugin_version) {
      issues.push({
        path: `${base}/storage`,
        message: 'Exact storage plugin_id and plugin_version are required',
      });
    }
    if (caps) {
      const acqAllowed = eligibleBackendsForRole(caps, 'acquisition').some(
        (b) =>
          b.plugin_id === p.acquisition.plugin_id &&
          b.plugin_version === p.acquisition.plugin_version,
      );
      if (!acqAllowed) {
        issues.push({
          path: `${base}/acquisition/plugin_version`,
          message: `Acquisition ${p.acquisition.plugin_id}@${p.acquisition.plugin_version} is not installed+allowed`,
        });
      }
      const stoAllowed = eligibleBackendsForRole(caps, 'storage').some(
        (b) =>
          b.plugin_id === p.storage.plugin_id && b.plugin_version === p.storage.plugin_version,
      );
      if (!stoAllowed) {
        issues.push({
          path: `${base}/storage/plugin_version`,
          message: `Storage ${p.storage.plugin_id}@${p.storage.plugin_version} is not installed+allowed`,
        });
      }
    }
    if (!p.streams.length) {
      issues.push({ path: `${base}/streams`, message: 'At least one stream is required' });
    }
    if (isOfficialMcapEditorSupported(p.storage.plugin_id, p.storage.plugin_version)) {
      const mcapCfg = p.storage.config;
      const profile = mcapCfg.profile ?? 'ros2';
      if (profile !== 'ros2' && profile !== 'generic') {
        issues.push({
          path: `${base}/storage/config/profile`,
          message: 'MCAP profile must be ros2 or generic',
        });
      }
      if ('ros_distro' in mcapCfg && mcapCfg.ros_distro !== undefined) {
        if (profile === 'generic') {
          issues.push({
            path: `${base}/storage/config/ros_distro`,
            message: 'MCAP generic profile forbids ros_distro',
          });
        } else if (
          typeof mcapCfg.ros_distro !== 'string' ||
          !/^[a-z][a-z0-9_-]{0,31}$/.test(mcapCfg.ros_distro)
        ) {
          issues.push({
            path: `${base}/storage/config/ros_distro`,
            message: 'ros_distro must match ^[a-z][a-z0-9_-]{0,31}$',
          });
        }
      }
    }
    const seenKeys = new Set<string>();
    p.streams.forEach((s, sIdx) => {
      if (!s.stream_key || !/^[!-~]{1,128}$/.test(s.stream_key)) {
        issues.push({
          path: `${base}/streams/${sIdx}/stream_key`,
          message: 'stream_key must be 1..128 visible ASCII chars',
        });
      } else if (seenKeys.has(s.stream_key)) {
        issues.push({
          path: `${base}/streams/${sIdx}/stream_key`,
          message: `Duplicate stream_key "${s.stream_key}" within pipeline`,
        });
      } else {
        seenKeys.add(s.stream_key);
      }
    });
  });

  return issues;
}

export function computeIdentityDiff(
  original: PortableTaskConfig | null,
  draft: PortableTaskConfig,
): IdentityDiffSummary {
  const origPipes = new Set<string>();
  const origStreams = new Set<string>();
  if (original) {
    for (const p of original.pipelines) {
      if (p.pipeline_id) {
        origPipes.add(p.pipeline_id);
      }
      for (const s of p.streams) {
        if (s.stream_id) {
          origStreams.add(s.stream_id);
        }
      }
    }
  }
  const keptPipes = new Set<string>();
  const keptStreams = new Set<string>();
  let addedPipelines = 0;
  let addedStreams = 0;
  for (const p of draft.pipelines) {
    if (p.pipeline_id && origPipes.has(p.pipeline_id)) {
      keptPipes.add(p.pipeline_id);
    } else {
      addedPipelines += 1;
    }
    for (const s of p.streams) {
      if (s.stream_id && origStreams.has(s.stream_id)) {
        keptStreams.add(s.stream_id);
      } else {
        addedStreams += 1;
      }
    }
  }
  return {
    preservedPipelines: Array.from(keptPipes),
    addedPipelines,
    removedPipelines: Array.from(origPipes).filter((id) => !keptPipes.has(id)),
    preservedStreams: Array.from(keptStreams),
    addedStreams,
    removedStreams: Array.from(origStreams).filter((id) => !keptStreams.has(id)),
  };
}

export function serializePortableConfig(draft: PortableTaskConfig): string {
  const cleaned: Record<string, unknown> = {
    format_version: '1.0',
    segment_policy: {
      max_duration_ms: draft.segment_policy.max_duration_ms,
      max_physical_bytes: draft.segment_policy.max_physical_bytes,
    },
    retention_policy:
      draft.retention_policy.mode === 'keep_all'
        ? {
            mode: 'keep_all',
            max_duration_ms: null,
            max_physical_bytes: null,
          }
        : {
            mode: 'rolling',
            max_duration_ms: draft.retention_policy.max_duration_ms,
            max_physical_bytes: draft.retention_policy.max_physical_bytes,
          },
    pipelines: draft.pipelines.map((p) => {
      const pipeObj: Record<string, unknown> = {
        required: p.required,
        resource_profile: p.resource_profile,
        output_root: p.output_root,
        acquisition: {
          plugin_id: p.acquisition.plugin_id,
          plugin_version: p.acquisition.plugin_version,
          config: p.acquisition.config,
        },
        storage: {
          plugin_id: p.storage.plugin_id,
          plugin_version: p.storage.plugin_version,
          config: p.storage.config,
        },
        streams: p.streams.map((s) => {
          const stObj: Record<string, unknown> = {
            stream_key: s.stream_key,
            readiness_policy: s.readiness_policy,
          };
          if (s.stream_id) {
            stObj.stream_id = s.stream_id;
          }
          if (s.name && s.name.trim().length > 0) {
            stObj.name = s.name.trim();
          }
          return stObj;
        }),
      };
      if (p.pipeline_id) {
        pipeObj.pipeline_id = p.pipeline_id;
      }
      return pipeObj;
    }),
  };
  if (draft.name && draft.name.trim().length > 0) {
    cleaned.name = draft.name.trim();
  }
  return JSON.stringify(cleaned, null, 2);
}
