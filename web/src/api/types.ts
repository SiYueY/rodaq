import type {
  ApiErrorBody,
  ApiVersion,
  OperationKind,
  OperationState,
  ServerPhase,
} from './generated';

export type AppLifecycleState =
  | 'Bootstrapping'
  | 'ServerUnavailable'
  | 'Unauthenticated'
  | 'Authenticating'
  | 'AuthenticatedReady'
  | 'ApiIncompatible';

export type ClientRequestPhase = 'idle' | 'loading' | 'success' | 'stale' | 'error';

export type ApiFailureKind =
  | 'network'
  | 'timeout'
  | 'unauthenticated'
  | 'forbidden'
  | 'api_incompatible'
  | 'server_error'
  | 'validation';

export interface HeadMetadata {
  contentLength: string | null;
  etag: string | null;
  acceptRanges: string | null;
  contentType: string | null;
  requestId: string | null;
}

export interface ConfigExportResult {
  rawJson: string;
  config: import('./generated').PortableTaskConfig;
  etag: string | null;
  requestId: string | null;
}

export function isTerminalOperationState(state: OperationState): boolean {
  return (
    state === 'succeeded' ||
    state === 'partially_succeeded' ||
    state === 'failed' ||
    state === 'superseded'
  );
}

export function isApiPhaseReady(phase: ServerPhase): boolean {
  return phase === 'ready';
}

export type { ApiErrorBody, ApiVersion, OperationKind, OperationState, ServerPhase };
