/**
 * Wire DTO contract strictly projected from server/api/openapi.yaml (OpenAPI 3.1.0)
 * and server/src/api/dto.cpp.
 * Checked automatically by tests/unit/api_contract.test.ts and live Playwright E2E.
 * All uint64 fields are DecimalString and must never be cast to JS Number.
 */

export type DecimalString = string;
export type EntityId = string;
export type Rfc3339Timestamp = string;

export interface ApiVersion {
  major: number;
  minor: number;
}

export interface DurationValue {
  value: DecimalString;
  unit: 'ns' | 'us' | 'ms' | 's';
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
  | 'artifact_quarantined'
  | 'historical_artifact_lost_or_corrupt';

export type DesiredState = 'stopped' | 'running' | 'paused';

export type ExecutionCondition =
  | 'idle'
  | 'preparing'
  | 'active'
  | 'degraded'
  | 'draining'
  | 'protected'
  | 'pending_cleanup'
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

export type Scope = 'read' | 'control' | 'admin';
export type ServerPhase = 'recovering' | 'ready' | 'draining' | 'failed';
export type SnapshotFreshnessState = 'fresh' | 'stale' | 'unknown';
export type VolumePressure = 'normal' | 'low' | 'critical' | 'protected';
export type BackendRole = 'acquisition' | 'storage';
export type SupportStatus = 'required' | 'qualified_candidate' | 'deferred';
export type QualificationStatus = 'qualified' | 'not_qualified';

export interface FreshnessInfo {
  state: SnapshotFreshnessState;
  age: DurationValue | null;
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

export interface SessionData {
  principal: string;
  scopes: Scope[];
  api_version: ApiVersion;
  csrf_token: string;
}

export interface VolumeStatusData {
  name: string;
  total_bytes: DecimalString;
  free_bytes: DecimalString;
  pressure: VolumePressure;
}

export interface SystemData {
  server_phase: ServerPhase;
  core_ready: boolean;
  recovery: {
    complete: boolean;
    reason: string | null;
    progress: number | null;
  };
  resources: {
    cpu_cores: number;
    rss_bytes: DecimalString;
    threads: DecimalString;
    open_fds: DecimalString;
  };
  volumes: VolumeStatusData[];
  backlog: {
    commit_items: DecimalString;
    commit_bytes: DecimalString;
    active_downloads: DecimalString;
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
  free_bytes: DecimalString;
  pressure: VolumePressure;
}

export interface ResourceProfileCapabilityData {
  name: string;
  allowed: boolean;
  max_segment_physical_bytes: DecimalString;
  max_streams_per_pipeline: DecimalString;
}

export interface CapabilitiesData {
  application_version: string;
  backends: BackendCapabilityData[];
  output_roots: OutputRootCapabilityData[];
  resource_profiles: ResourceProfileCapabilityData[];
  api_limits: {
    max_request_body_bytes: DecimalString;
    max_page_size: DecimalString;
    idempotency_retention: DurationValue;
    recommended_polling_floor: DurationValue;
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
    commit_lag: DurationValue | null;
    receiving_streams: DecimalString;
    total_streams: DecimalString;
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
  oldest_retained_control_generation: DecimalString | null;
  oldest_retained_at: Rfc3339Timestamp | null;
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
  records: DecimalString;
}

export interface StreamRuntimeData {
  stream_id: DecimalString;
  stream_key: string;
  name: string | null;
  entry_ready: boolean;
  source_ready: boolean;
  receiving: boolean;
  last_receive_at: Rfc3339Timestamp | null;
  window: DurationValue;
  received: {
    records: DecimalString;
    logical_bytes: DecimalString;
    records_per_second: number;
    logical_bytes_per_second: number;
  };
  admission: {
    accepted_records: DecimalString;
    rejected_records: DecimalString;
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
    records: DecimalString;
    logical_bytes: DecimalString;
    records_per_second: number;
    logical_bytes_per_second: number;
  };
  admission: {
    accepted_records: DecimalString;
    rejected_records: DecimalString;
    reject_reasons: RejectReasonCount[];
  };
  queue: {
    records: DecimalString;
    bytes: DecimalString;
    high_watermark_records: DecimalString;
    high_watermark_bytes: DecimalString;
    oldest_age: DurationValue | null;
  };
  writer: {
    logical_bytes_per_second: number;
    pending_records: DecimalString;
    last_progress_at: Rfc3339Timestamp | null;
  };
  storage: {
    physical_bytes_per_second: number;
    pending_bytes: DecimalString;
    last_progress_at: Rfc3339Timestamp | null;
  };
  commit: {
    lag: DurationValue | null;
    durable_bytes_per_second: number;
    pending_segments: DecimalString;
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
  begin: DurationValue;
  end: DurationValue;
}

export interface ArtifactSummaryData {
  artifact_id: EntityId;
  task_id: EntityId;
  pipeline_id: DecimalString;
  segment_id: DecimalString;
  committed_at: Rfc3339Timestamp;
  recording_range: RecordingRangeData;
  format: string;
  logical_bytes: DecimalString;
  physical_bytes: DecimalString;
  data_impact: DataImpact;
}

export interface ArtifactFileData {
  file_id: EntityId;
  display_name: string;
  size: DecimalString;
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
  gap_count: DecimalString;
  gaps: {
    kind: CoverageGapKind;
    recording_begin: DurationValue | null;
    recording_end: DurationValue | null;
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
  recording_mode: 'continuous' | 'rolling';
  start_policy: { mode: 'all_required' | 'best_effort' };
  recovery_policy: { mode: 'manual' | 'resume' };
  segment_policy: {
    max_duration: DurationValue | null;
    max_physical_bytes: DecimalString | null;
  };
  retention_policy: {
    mode: 'keep_all' | 'rolling';
    max_duration: DurationValue | null;
    max_physical_bytes: DecimalString | null;
    include_previous_continuous?: boolean;
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
    'artifact_quarantined',
    'historical_artifact_lost_or_corrupt',
  ] as const,
  DesiredState: ['stopped', 'running', 'paused'] as const,
  ExecutionCondition: [
    'idle',
    'preparing',
    'active',
    'degraded',
    'draining',
    'protected',
    'pending_cleanup',
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
