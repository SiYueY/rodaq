export type DecimalString = string
export type EntityId = string
export type Rfc3339Timestamp = string

export interface ApiVersion {
  major: number
  minor: number
}

export interface DurationValue {
  value: DecimalString
  unit: 'ns' | 'us' | 'ms' | 's'
}

export interface FreshnessData {
  state: 'fresh' | 'stale' | 'unknown'
  age: DurationValue | null
}

export type Scope = 'read' | 'control' | 'admin'
export type ServerPhase = 'recovering' | 'ready' | 'draining' | 'failed'
export type DesiredState = 'stopped' | 'running' | 'paused'
export type ExecutionCondition =
  | 'idle'
  | 'preparing'
  | 'active'
  | 'degraded'
  | 'draining'
  | 'protected'
  | 'pending_cleanup'
  | 'failed'
export type OperationKind = 'configure' | 'start' | 'pause' | 'resume' | 'stop' | 'reset' | 'retire'
export type LifecycleOperationKind = 'start' | 'pause' | 'resume' | 'stop' | 'reset' | 'retire'
export type OperationState =
  | 'accepted'
  | 'in_progress'
  | 'succeeded'
  | 'partially_succeeded'
  | 'failed'
  | 'superseded'
export type Retryability =
  | 'never'
  | 'after_condition_change'
  | 'after_backoff'
  | 'after_process_restart'
  | 'manual_recovery_required'
export type FailureDomain =
  | 'request'
  | 'task_control'
  | 'pipeline'
  | 'collector'
  | 'storage'
  | 'volume'
  | 'persistence_state'
  | 'process'
export type DataImpact =
  | 'none'
  | 'pre_admission_only'
  | 'accepted_known_failed'
  | 'persistence_unknown'
  | 'artifact_quarantined'
  | 'historical_artifact_lost_or_corrupt'

export interface ErrorDetail {
  code: string
  path: string
  message: string
}

export interface ApiError {
  code: string
  message: string
  retryability: Retryability
  failure_domain: FailureDomain
  data_impact: DataImpact
  details: ErrorDetail[]
}

export interface SuccessEnvelope<T> {
  api_version: ApiVersion
  request_id: string
  data: T
}

export interface ErrorEnvelope {
  api_version: ApiVersion
  request_id: string
  error: ApiError
}

export interface Page<T> {
  items: T[]
  has_more: boolean
  next_cursor: string
}

export interface SessionData {
  principal: string
  scopes: Scope[]
  api_version: ApiVersion
  csrf_token: string
}

export interface VolumeStatusData {
  name: string
  total_bytes: DecimalString
  free_bytes: DecimalString
  managed_bytes?: DecimalString
  quarantined_bytes?: DecimalString
  pressure: 'normal' | 'low' | 'critical' | 'protected'
}

export interface SystemData {
  server_phase: ServerPhase
  core_ready: boolean
  recovery: {
    complete: boolean
    reason: string | null
    progress: number | null
  }
  resources: {
    cpu_cores: number
    rss_bytes: DecimalString
    threads: DecimalString
    open_fds: DecimalString
  }
  volumes: VolumeStatusData[]
  backlog: {
    commit_items: DecimalString
    commit_bytes: DecimalString
    active_downloads: DecimalString
    quarantined_items?: DecimalString
  }
  captured_at: Rfc3339Timestamp
  snapshot_sequence: DecimalString
  freshness: FreshnessData
}

export interface BackendCapability {
  plugin_id: string
  plugin_version: string
  role: 'acquisition' | 'storage'
  installed: boolean
  allowed: boolean
  support_status: 'required' | 'qualified_candidate' | 'deferred'
  qualification_status: 'qualified' | 'not_qualified'
}

export interface CapabilitiesData {
  application_version: string
  backends: BackendCapability[]
  output_roots: Array<{
    name: string
    allowed: boolean
    writable: boolean
    free_bytes: DecimalString
    pressure: 'normal' | 'low' | 'critical' | 'protected'
  }>
  resource_profiles: Array<{
    name: string
    allowed: boolean
    max_streams_per_pipeline: DecimalString
    max_segment_physical_bytes: DecimalString
  }>
  api_limits: {
    max_request_body_bytes: DecimalString
    max_page_size: DecimalString
    idempotency_retention: DurationValue
    recommended_polling_floor: DurationValue
  }
}

export interface TaskData {
  task_id: EntityId
  name: string | null
  config_version: string
  desired_state: DesiredState
  execution_condition: ExecutionCondition
  control_generation: DecimalString
  current_operation_id: EntityId | null
  data_impact: DataImpact
  captured_at: Rfc3339Timestamp
  snapshot_sequence: DecimalString
  freshness: FreshnessData
}

export interface TaskSummaryData extends TaskData {
  runtime_summary: {
    accepted_records_per_second: number
    commit_lag: DurationValue | null
    receiving_streams: DecimalString
    total_streams: DecimalString
  }
}

export interface TaskCreateData {
  task_id: EntityId
  config_version: string
}

export interface MutationAcceptedData {
  task_id: EntityId
  operation_id: EntityId
  state: 'accepted'
}

export interface OperationData {
  task_id: EntityId
  operation_id: EntityId
  kind: OperationKind
  state: OperationState
  control_generation: DecimalString
  accepted_at: Rfc3339Timestamp
  terminal_at: Rfc3339Timestamp | null
  retryability: Retryability
  failure_domain: FailureDomain
  data_impact: DataImpact
  message: string | null
}

export interface OperationPage extends Page<OperationData> {
  history_truncated: boolean
  oldest_retained_control_generation: DecimalString | null
  oldest_retained_at: Rfc3339Timestamp | null
}

export interface StreamRuntimeData {
  stream_id: EntityId
  stream_key: string
  name: string | null
  entry_ready: boolean
  source_ready: boolean
  receiving: boolean
  last_receive_at: Rfc3339Timestamp | null
  window: DurationValue
  received: {
    records: DecimalString
    logical_bytes: DecimalString
    records_per_second: number
    logical_bytes_per_second: number
  }
  admission: {
    accepted_records: DecimalString
    rejected_records: DecimalString
    reject_reasons: Array<{ reason: string; records: DecimalString }>
  }
}

export interface PipelineRuntimeData {
  pipeline_id: EntityId
  required: boolean
  instance_id: string | null
  condition: ExecutionCondition
  gate_state: 'closed' | 'open'
  received: {
    records: DecimalString
    logical_bytes: DecimalString
    records_per_second: number
    logical_bytes_per_second: number
  }
  admission: {
    accepted_records: DecimalString
    rejected_records: DecimalString
    reject_reasons: Array<{ reason: string; records: DecimalString }>
  }
  queue: {
    records: DecimalString
    bytes: DecimalString
    high_watermark_records: DecimalString
    high_watermark_bytes: DecimalString
    oldest_age: DurationValue | null
  }
  writer: {
    logical_bytes_per_second: number
    pending_records: DecimalString
    last_progress_at: Rfc3339Timestamp | null
  }
  storage: {
    physical_bytes_per_second: number
    pending_bytes: DecimalString
    last_progress_at: Rfc3339Timestamp | null
  }
  commit: {
    lag: DurationValue | null
    durable_bytes_per_second: number
    pending_segments: DecimalString
    last_committed_artifact_id: EntityId | null
    last_progress_at: Rfc3339Timestamp | null
  }
  streams: StreamRuntimeData[]
}

export interface TaskRuntimeData {
  task_id: EntityId
  captured_at: Rfc3339Timestamp
  snapshot_sequence: DecimalString
  freshness: FreshnessData
  pipelines: PipelineRuntimeData[]
}

export interface ArtifactSummaryData {
  artifact_id: EntityId
  task_id: EntityId
  pipeline_id: EntityId
  segment_id: DecimalString
  committed_at: Rfc3339Timestamp
  recording_range: {
    begin: DurationValue
    end: DurationValue
  }
  format: string
  logical_bytes: DecimalString
  physical_bytes: DecimalString
  data_impact: DataImpact
}

export interface ArtifactFileData {
  file_id: EntityId
  display_name: string
  size: DecimalString
  media_type: string
  etag: string
  download_url: string
}

export interface ArtifactData extends ArtifactSummaryData {
  coverage: {
    complete: boolean
    gap_count: DecimalString
    gaps: Array<{
      kind: string
      recording_begin: DurationValue | null
      recording_end: DurationValue | null
      message: string | null
    }>
  }
  files: ArtifactFileData[]
}
