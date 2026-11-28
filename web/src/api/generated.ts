/**
 * Wire DTO contract strictly projected from server/api/openapi.yaml (OpenAPI 3.1.0)
 * and server/src/api/dto.cpp.
 * Bounded counts, bytes, and millisecond durations use JSON number;
 * 64-bit identifiers and control_generation/snapshot_sequence use DecimalString.
 */

export type DecimalString = string;
export type EntityId = string;
export type Rfc3339Timestamp = string;

export interface ApiVersion {
  major: number;
  minor: number;
}

export type Retryability =
  | 'never'
  | 'after_condition_change'
  | 'after_backoff'
  | 'after_process_restart'
  | 'manual_recovery_required';

export type FailureDomain =
  | 'request'
  | 'task_control'
  | 'pipeline'
  | 'collector'
  | 'storage'
  | 'volume'
  | 'persistence_state'
  | 'process';

export type DataImpact =
  | 'none'
  | 'pre_admission_only'
  | 'accepted_known_failed'
  | 'persistence_unknown'
  | 'artifact_unavailable_or_corrupt';

export type DesiredState = 'stopped' | 'running' | 'paused';

export type ExecutionCondition =
  | 'idle'
  | 'preparing'
  | 'active'
  | 'degraded'
  | 'draining'
  | 'protected'
  | 'failed';

export type OperationKind =
  | 'configure'
  | 'start'
  | 'pause'
  | 'resume'
  | 'stop'
  | 'reset'
  | 'retire';

export type OperationState =
  | 'accepted'
  | 'in_progress'
  | 'succeeded'
  | 'partially_succeeded'
  | 'failed'
  | 'superseded';

export type ServerPhase = 'starting' | 'ready' | 'draining' | 'failed';
export type SnapshotFreshnessState = 'fresh' | 'stale' | 'unknown';
export type VolumePressure = 'normal' | 'low' | 'critical' | 'protected';
export type BackendRole = 'acquisition' | 'storage';
export type SupportStatus = 'required' | 'qualified_candidate' | 'deferred';
export type QualificationStatus = 'qualified' | 'not_qualified';

export interface FreshnessInfo {
  state: SnapshotFreshnessState;
  age_ms: number | null;
}

export interface ErrorDetail {
  code: string;
  path: string;
  message: string;
}

export interface ApiErrorBody {
  code: string;
  message: string;
  retryability: Retryability;
  failure_domain: FailureDomain;
  data_impact: DataImpact;
  details: ErrorDetail[];
}

export interface SuccessEnvelope<T> {
  api_version: ApiVersion;
  request_id: string;
  data: T;
}

export interface ErrorEnvelope {
  api_version: ApiVersion;
  request_id: string;
  error: ApiErrorBody;
}

export interface VolumeStatusData {
  name: string;
  total_bytes: number;
  free_bytes: number;
  pressure: VolumePressure;
}

export interface SystemData {
  server_phase: ServerPhase;
  core_ready: boolean;
  deployment_error?: string;
  resources: {
    cpu_cores: number;
    rss_bytes: number;
    threads: number;
    open_fds: number;
  };
  volumes: VolumeStatusData[];
  backlog: {
    commit_items: number;
    commit_bytes: number;
    active_downloads: number;
  };
  captured_at: Rfc3339Timestamp;
  snapshot_sequence: DecimalString;
  freshness: FreshnessInfo;
}

export interface BackendCapabilityData {
  plugin_id: string;
  plugin_version: string;
  role: BackendRole;
  installed: boolean;
  allowed: boolean;
  support_status: SupportStatus;
  qualification_status: QualificationStatus;
}

export interface OutputRootCapabilityData {
  name: string;
  allowed: boolean;
  writable: boolean;
  free_bytes: number;
  pressure: VolumePressure;
}

export interface ResourceProfileCapabilityData {
  name: string;
  allowed: boolean;
  max_segment_physical_bytes: number;
  max_streams_per_pipeline: number;
}

export interface CapabilitiesData {
  application_version: string;
  backends: BackendCapabilityData[];
  output_roots: OutputRootCapabilityData[];
  resource_profiles: ResourceProfileCapabilityData[];
  api_limits: {
    max_request_body_bytes: number;
    max_page_size: number;
    recommended_polling_floor_ms: number;
  };
}

export interface TaskData {
  task_id: EntityId;
  name: string | null;
  config_version: string;
  desired_state: DesiredState;
  execution_condition: ExecutionCondition;
  control_generation: DecimalString;
  current_operation_id: EntityId | null;
  data_impact: DataImpact;
  captured_at: Rfc3339Timestamp;
  snapshot_sequence: DecimalString;
  freshness: FreshnessInfo;
}

export interface TaskSummaryData {
  task_id: EntityId;
  name: string | null;
  config_version: string;
  desired_state: DesiredState;
  execution_condition: ExecutionCondition;
  control_generation: DecimalString;
  current_operation_id: EntityId | null;
  data_impact: DataImpact;
  runtime_summary: {
    accepted_records_per_second: number;
    commit_lag_ms: number | null;
    receiving_streams: number;
    total_streams: number;
  };
  captured_at: Rfc3339Timestamp;
  freshness: FreshnessInfo;
}

export interface TaskCreateData {
  task_id: EntityId;
  config_version: string;
}

export interface PageData<T> {
  items: T[];
  next_cursor: string;
  has_more: boolean;
}

export interface OperationPageData extends PageData<OperationData> {
  history_truncated: boolean;
}

export interface MutationAcceptedData {
  task_id: EntityId;
  operation_id: EntityId;
  state: OperationState;
}

export interface OperationData {
  operation_id: EntityId;
  task_id: EntityId;
  kind: OperationKind;
  control_generation: DecimalString;
  state: OperationState;
  accepted_at: Rfc3339Timestamp;
  terminal_at: Rfc3339Timestamp | null;
  retryability: Retryability | null;
  failure_domain: FailureDomain | null;
  data_impact: DataImpact;
  message: string | null;
}

export interface RejectReasonCount {
  reason: string;
  records: number;
}

export interface StreamRuntimeData {
  stream_id: DecimalString;
  stream_key: string;
  name: string | null;
  entry_ready: boolean;
  source_ready: boolean;
  receiving: boolean;
  last_receive_at: Rfc3339Timestamp | null;
  window_ms: number;
  received: {
    records: number;
    logical_bytes: number;
    records_per_second: number;
    logical_bytes_per_second: number;
  };
  admission: {
    accepted_records: number;
    rejected_records: number;
    reject_reasons: RejectReasonCount[];
  };
  captured_at: Rfc3339Timestamp;
  freshness: FreshnessInfo;
}

export interface PipelineRuntimeData {
  pipeline_id: DecimalString;
  required: boolean;
  instance_id: string | null;
  condition: ExecutionCondition;
  gate_state: 'open' | 'closed';
  received: {
    records: number;
    logical_bytes: number;
    records_per_second: number;
    logical_bytes_per_second: number;
  };
  admission: {
    accepted_records: number;
    rejected_records: number;
    reject_reasons: RejectReasonCount[];
  };
  queue: {
    records: number;
    bytes: number;
    high_watermark_records: number;
    high_watermark_bytes: number;
    oldest_age_ms: number | null;
  };
  writer: {
    logical_bytes_per_second: number;
    pending_records: number;
    last_progress_at: Rfc3339Timestamp | null;
  };
  storage: {
    physical_bytes_per_second: number;
    pending_bytes: number;
    last_progress_at: Rfc3339Timestamp | null;
  };
  commit: {
    lag_ms: number | null;
    durable_bytes_per_second: number;
    pending_segments: number;
    last_committed_artifact_id: EntityId | null;
    last_progress_at: Rfc3339Timestamp | null;
  };
  streams: StreamRuntimeData[];
}

export interface TaskRuntimeData {
  task_id: EntityId;
  captured_at: Rfc3339Timestamp;
  snapshot_sequence: DecimalString;
  freshness: FreshnessInfo;
  pipelines: PipelineRuntimeData[];
}

export interface RecordingRangeData {
  begin_ms: number;
  end_ms: number;
}

export interface ArtifactSummaryData {
  artifact_id: EntityId;
  task_id: EntityId;
  pipeline_id: DecimalString;
  segment_id: DecimalString;
  committed_at: Rfc3339Timestamp;
  recording_range: RecordingRangeData;
  format: string;
  logical_bytes: number;
  physical_bytes: number;
  data_impact: DataImpact;
}

export interface ArtifactFileData {
  file_id: EntityId;
  display_name: string;
  size: number;
  media_type: string;
  etag: string;
  download_url: string;
}

export type CoverageGapKind =
  | 'source_unavailable'
  | 'admission_reject'
  | 'overflow'
  | 'storage_failure'
  | 'persistence_unknown'
  | 'reset_or_retention';

export interface CoverageData {
  complete: boolean;
  gap_count: number;
  gaps: {
    kind: CoverageGapKind;
    recording_begin_ms: number | null;
    recording_end_ms: number | null;
    message: string | null;
  }[];
}

export interface ArtifactData extends ArtifactSummaryData {
  coverage: CoverageData;
  files: ArtifactFileData[];
}

export interface PortablePluginConfig {
  plugin_id: string;
  plugin_version: string;
  config: Record<string, unknown>;
}

export interface PortableStreamConfig {
  stream_id?: DecimalString;
  stream_key: string;
  name?: string;
  readiness_policy: 'entry_ready' | 'source_ready';
}

export interface PortablePipelineConfig {
  pipeline_id?: DecimalString;
  required: boolean;
  resource_profile: string;
  output_root: string;
  acquisition: PortablePluginConfig;
  storage: PortablePluginConfig;
  streams: PortableStreamConfig[];
}

export interface PortableTaskConfig {
  format_version: '1.0';
  name?: string;
  segment_policy: {
    max_duration_ms: number | null;
    max_physical_bytes: number | null;
  };
  retention_policy: {
    mode: 'keep_all' | 'rolling';
    max_duration_ms: number | null;
    max_physical_bytes: number | null;
  };
  pipelines: PortablePipelineConfig[];
}

export const CLOSED_ENUM_VALUES = {
  Retryability: [
    'never',
    'after_condition_change',
    'after_backoff',
    'after_process_restart',
    'manual_recovery_required',
  ] as const,
  FailureDomain: [
    'request',
    'task_control',
    'pipeline',
    'collector',
    'storage',
    'volume',
    'persistence_state',
    'process',
  ] as const,
  DataImpact: [
    'none',
    'pre_admission_only',
    'accepted_known_failed',
    'persistence_unknown',
    'artifact_unavailable_or_corrupt',
  ] as const,
  DesiredState: ['stopped', 'running', 'paused'] as const,
  ExecutionCondition: [
    'idle',
    'preparing',
    'active',
    'degraded',
    'draining',
    'protected',
    'failed',
  ] as const,
  OperationKind: ['configure', 'start', 'pause', 'resume', 'stop', 'reset', 'retire'] as const,
  OperationState: [
    'accepted',
    'in_progress',
    'succeeded',
    'partially_succeeded',
    'failed',
    'superseded',
  ] as const,
} as const;
