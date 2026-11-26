import type { ApiErrorBody } from './generated';
import type { ApiFailureKind } from './types';

export class ApiFailure extends Error {
  readonly httpStatus: number | null;
  readonly requestId: string | null;
  readonly retryAfterMs: number | null;
  readonly apiError: ApiErrorBody | null;
  readonly kind: ApiFailureKind;

  constructor(params: {
    message: string;
    httpStatus: number | null;
    requestId: string | null;
    retryAfterMs: number | null;
    apiError: ApiErrorBody | null;
    kind: ApiFailureKind;
  }) {
    super(params.message);
    this.name = 'ApiFailure';
    this.httpStatus = params.httpStatus;
    this.requestId = params.requestId;
    this.retryAfterMs = params.retryAfterMs;
    this.apiError = params.apiError;
    this.kind = params.kind;
  }
}

export function parseRetryAfterMs(headerValue: string | null, nowMs = Date.now()): number | null {
  if (!headerValue) {
    return null;
  }
  const trimmed = headerValue.trim();
  if (/^\d+$/.test(trimmed)) {
    const secs = Number(trimmed);
    if (Number.isFinite(secs) && secs >= 0) {
      return Math.min(secs * 1000, 300_000);
    }
    return null;
  }
  const parsedDate = Date.parse(trimmed);
  if (!Number.isNaN(parsedDate)) {
    const diff = parsedDate - nowMs;
    return diff > 0 ? Math.min(diff, 300_000) : 0;
  }
  return null;
}

export function classifyFailureKind(
  httpStatus: number | null,
  apiError: ApiErrorBody | null,
): ApiFailureKind {
  if (httpStatus === null) {
    return 'network';
  }
  if (apiError?.code === 'unsupported_api_version') {
    return 'api_incompatible';
  }
  if (httpStatus === 401) {
    return 'unauthenticated';
  }
  if (httpStatus === 403) {
    return 'forbidden';
  }
  if (httpStatus === 400 || httpStatus === 409 || httpStatus === 413 || httpStatus === 415) {
    return 'validation';
  }
  if (httpStatus === 504) {
    return 'timeout';
  }
  return 'server_error';
}

export function parseApiVersionHeader(raw: string | null): { major: number; minor: number } | null {
  if (!raw) {
    return null;
  }
  const match = /^(\d+)\.(\d+)$/.exec(raw.trim());
  if (!match) {
    return null;
  }
  return { major: Number(match[1]), minor: Number(match[2]) };
}
