import { describe, expect, it } from 'vitest';
import type { CapabilitiesData, PortableTaskConfig } from '../../src/api/generated';
import {
  buildDefaultRos2Config,
  buildQualifiedMcapConfig,
  computeIdentityDiff,
  createDefaultTaskDraft,
  eligibleBackendsForRole,
  isOfficialMcapEditorSupported,
  isOfficialRos2EditorSupported,
  OFFICIAL_MCAP_PLUGIN_ID,
  OFFICIAL_ROS2_PLUGIN_ID,
  serializePortableConfig,
  validateTaskDraft,
} from '../../src/features/tasks/taskEditorModel';

const SAMPLE_CAPS: CapabilitiesData = {
  application_version: '0.1.0',
  backends: [
    {
      plugin_id: OFFICIAL_ROS2_PLUGIN_ID,
      plugin_version: '1.0.0',
      role: 'acquisition',
      installed: true,
      allowed: true,
      support_status: 'required',
      qualification_status: 'qualified',
    },
    {
      plugin_id: OFFICIAL_MCAP_PLUGIN_ID,
      plugin_version: '1.0.0',
      role: 'storage',
      installed: true,
      allowed: true,
      support_status: 'required',
      qualification_status: 'qualified',
    },
    {
      plugin_id: 'com.rodaq.storage.deferred_future',
      plugin_version: '1.0.0',
      role: 'storage',
      installed: true,
      allowed: true,
      support_status: 'deferred',
      qualification_status: 'not_qualified',
    },
  ],
  resource_profiles: [
    {
      name: 'default',
      allowed: true,
      max_segment_physical_bytes: '1048576',
      max_streams_per_pipeline: '16',
    },
  ],
  output_roots: [
    { name: 'local', allowed: true, writable: true, free_bytes: '104857600', pressure: 'normal' },
  ],
  api_limits: {
    max_request_body_bytes: '1048576',
    max_page_size: '200',
    idempotency_retention: { value: '86400', unit: 's' },
    recommended_polling_floor: { value: '1000', unit: 'ms' },
  },
};

describe('Task Editor compatibility table & validation (WEB-E2E-016/017)', () => {
  it('gates official ROS 2 and MCAP editors strictly on exact plugin_id + plugin_version', () => {
    expect(isOfficialRos2EditorSupported(OFFICIAL_ROS2_PLUGIN_ID, '1.0.0')).toBe(true);
    expect(isOfficialRos2EditorSupported(OFFICIAL_ROS2_PLUGIN_ID, '2.0.0')).toBe(false);
    expect(isOfficialRos2EditorSupported('other.plugin', '1.0.0')).toBe(false);

    expect(isOfficialMcapEditorSupported(OFFICIAL_MCAP_PLUGIN_ID, '1.0.0')).toBe(true);
    expect(isOfficialMcapEditorSupported(OFFICIAL_MCAP_PLUGIN_ID, '1.1.0')).toBe(false);

    expect(buildQualifiedMcapConfig().format_version).toBe('1.0');
    expect(buildDefaultRos2Config(['cam']).format_version).toBe('1.0');
  });

  it('excludes deferred backends and validates portable task drafts', () => {
    const storageList = eligibleBackendsForRole(SAMPLE_CAPS, 'storage');
    expect(storageList.map((b) => b.plugin_id)).toEqual([OFFICIAL_MCAP_PLUGIN_ID]);

    const draft = createDefaultTaskDraft(SAMPLE_CAPS);
    expect(validateTaskDraft(draft, SAMPLE_CAPS)).toEqual([]);

    // Rolling mode requires rolling retention with positive bound.
    draft.recording_mode = 'rolling';
    const issues = validateTaskDraft(draft, SAMPLE_CAPS);
    expect(issues.some((i) => i.path === '/retention_policy')).toBe(true);

    const json = serializePortableConfig(draft);
    expect(json).toContain('"format_version": "1.0"');
  });

  it('computes explicit pipeline_id and stream_id preservation/add/remove diff (WEB-E2E-016)', () => {
    const original: PortableTaskConfig = createDefaultTaskDraft(SAMPLE_CAPS);
    original.pipelines[0].pipeline_id = 'pipe-uuid-1';
    original.pipelines[0].streams = [
      { stream_id: 'stream-uuid-a', stream_key: 's0', name: 's0', readiness_policy: 'entry_ready' },
      { stream_id: 'stream-uuid-b', stream_key: 's1', name: 's1', readiness_policy: 'entry_ready' },
    ];

    const edited: PortableTaskConfig = JSON.parse(JSON.stringify(original)) as PortableTaskConfig;
    edited.pipelines[0].streams = [
      edited.pipelines[0].streams[0],
      { stream_key: 's1', name: 's1-replaced', readiness_policy: 'source_ready' },
    ];

    const diff = computeIdentityDiff(original, edited);
    expect(diff.preservedPipelines).toEqual(['pipe-uuid-1']);
    expect(diff.removedPipelines).toEqual([]);
    expect(diff.addedPipelines).toBe(0);
    expect(diff.preservedStreams).toEqual(['stream-uuid-a']);
    expect(diff.removedStreams).toEqual(['stream-uuid-b']);
    expect(diff.addedStreams).toBe(1);
  });
});
