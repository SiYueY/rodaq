import type {
  ApiErrorBody,
  ApiVersion,
  OperationKind,
  OperationState,
  Scope,
  ServerPhase,
} from './generated';

export type AppLifecycleState =
  | 'Bootstrapping'
  | 'ServerUnavailable'
  | 'Unauthenticated'
  | 'Authenticating'
  | 'AuthenticatedRecovering'
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

export interface SessionIdentity {
  principal: string;
  scopes: Scope[];
  apiVersion: ApiVersion;
}

export function isTerminalOperationState(state: OperationState): boolean {
  return (
    state === 'succeeded' ||
    state === 'partially_succeeded' ||
    state === 'failed' ||
    state === 'superseded'
  );
}

export function requiredScopeForOperation(kind: OperationKind): Scope {
  if (kind === 'reset' || kind === 'retire') {
    return 'admin';
  }
  return 'control';
}

export function isApiPhaseReady(phase: ServerPhase): boolean {
  return phase === 'ready';
}

export function hasScope(scopes: readonly Scope[], required: Scope): boolean {
  return scopes.includes(required);
}

export type { ApiErrorBody, ApiVersion, OperationKind, OperationState, Scope, ServerPhase };
