import type {
  ApiErrorBody,
  DecimalString,
  ErrorEnvelope,
  PortableTaskConfig,
  SuccessEnvelope,
} from './generated';
import type { ConfigExportResult, HeadMetadata } from './types';
import { ApiFailure, classifyFailureKind, parseApiVersionHeader, parseRetryAfterMs } from './errors';

export const CLIENT_API_MAJOR = 1;
export const CLIENT_API_VERSION_HEADER = '1.0';

export const CLIENT_TIMEOUTS = {
  sessionOrSystem: 5_000,
  query: 10_000,
  mutation: 15_000,
  headPreflight: 10_000,
} as const;

export interface RequestOptions<TBody = never> {
  method: 'GET' | 'POST' | 'PUT' | 'DELETE' | 'HEAD';
  path: string;
  query?: Record<string, string | undefined>;
  body?: TBody;
  rawJsonBody?: string;
  timeoutMs: number;
  signal?: AbortSignal;
  idempotencyKey?: string;
  controlGeneration?: DecimalString | null;
  bearerToken?: string;
  suppressAuthHook?: boolean;
}

export interface ApiClientHooks {
  getBearerToken: () => string | null;
  onUnauthenticated: () => void;
  onApiIncompatible: (reason: string) => void;
}

export interface ApiClient {
  request<TResponse, TBody = never>(options: RequestOptions<TBody>): Promise<TResponse>;
  requestRawConfig(path: string, signal?: AbortSignal): Promise<ConfigExportResult>;
  head(path: string, signal?: AbortSignal): Promise<HeadMetadata>;
  downloadBlob(path: string, signal?: AbortSignal): Promise<{ blob: Blob; contentDisposition: string | null }>;
}

function buildUrl(path: string, query?: Record<string, string | undefined>): string {
  if (!query) {
    return path;
  }
  const params = new URLSearchParams();
  for (const [k, v] of Object.entries(query)) {
    if (v !== undefined && v !== '') {
      params.set(k, v);
    }
  }
  const qs = params.toString();
  return qs ? `${path}?${qs}` : path;
}

function mergeSignals(timeoutMs: number, external?: AbortSignal): {
  signal: AbortSignal;
  cleanup: () => void;
  didTimeout: () => boolean;
} {
  const controller = new AbortController();
  let timedOut = false;
  const timer = globalThis.setTimeout(() => {
    timedOut = true;
    controller.abort();
  }, timeoutMs);

  const onExternalAbort = () => {
    controller.abort();
  };
  if (external) {
    if (external.aborted) {
      controller.abort();
    } else {
      external.addEventListener('abort', onExternalAbort, { once: true });
    }
  }

  return {
    signal: controller.signal,
    didTimeout: () => timedOut,
    cleanup: () => {
      globalThis.clearTimeout(timer);
      if (external) {
        external.removeEventListener('abort', onExternalAbort);
      }
    },
  };
}

export function createApiClient(
  hooks: ApiClientHooks,
  fetchImpl: typeof fetch = globalThis.fetch.bind(globalThis),
): ApiClient {
  async function executeRaw(options: RequestOptions<unknown>): Promise<Response> {
    const url = buildUrl(options.path, options.query);
    const headers: Record<string, string> = {
      'RoDAQ-API-Version': CLIENT_API_VERSION_HEADER,
    };

    const token = options.bearerToken ?? hooks.getBearerToken();
    if (token) {
      headers['Authorization'] = `Bearer ${token}`;
    }

    if (options.idempotencyKey) {
      headers['Idempotency-Key'] = options.idempotencyKey;
    }
    if (options.controlGeneration) {
      headers['If-Control-Generation'] = options.controlGeneration;
    }

    let payload: string | undefined;
    if (options.rawJsonBody !== undefined) {
      headers['Content-Type'] = 'application/json';
      payload = options.rawJsonBody;
    } else if (options.body !== undefined) {
      headers['Content-Type'] = 'application/json';
      payload = JSON.stringify(options.body);
    }

    const merged = mergeSignals(options.timeoutMs, options.signal);
    try {
      const resp = await fetchImpl(url, {
        method: options.method,
        headers,
        body: payload,
        credentials: 'omit',
        signal: merged.signal,
      });

      const verHeader = parseApiVersionHeader(resp.headers.get('RoDAQ-API-Version'));
      if (verHeader && verHeader.major !== CLIENT_API_MAJOR) {
        const msg = `Server API major ${verHeader.major} is incompatible with Web major ${CLIENT_API_MAJOR}`;
        hooks.onApiIncompatible(msg);
        throw new ApiFailure({
          message: msg,
          httpStatus: resp.status,
          requestId: resp.headers.get('RoDAQ-Request-ID'),
          retryAfterMs: null,
          apiError: null,
          kind: 'api_incompatible',
        });
      }

      return resp;
    } catch (err) {
      if (err instanceof ApiFailure) {
        throw err;
      }
      if (merged.didTimeout()) {
        throw new ApiFailure({
          message: `Request timed out after ${options.timeoutMs} ms`,
          httpStatus: null,
          requestId: null,
          retryAfterMs: null,
          apiError: null,
          kind: 'timeout',
        });
      }
      if (options.signal?.aborted) {
        throw err;
      }
      throw new ApiFailure({
        message: err instanceof Error ? err.message : 'Network error',
        httpStatus: null,
        requestId: null,
        retryAfterMs: null,
        apiError: null,
        kind: 'network',
      });
    } finally {
      merged.cleanup();
    }
  }

  async function handleErrorResponse(resp: Response, suppressAuthHook = false): Promise<never> {
    const requestId = resp.headers.get('RoDAQ-Request-ID');
    const retryAfterMs = parseRetryAfterMs(resp.headers.get('Retry-After'));
    let apiError: ApiErrorBody | null = null;
    try {
      const text = await resp.text();
      if (text) {
        const parsed = JSON.parse(text) as Partial<ErrorEnvelope>;
        if (parsed.error && typeof parsed.error.code === 'string') {
          apiError = parsed.error;
        }
      }
    } catch {
      // Non-JSON error body handled via HTTP status fallback.
    }

    const kind = classifyFailureKind(resp.status, apiError);
    if (kind === 'unauthenticated' && !suppressAuthHook) {
      hooks.onUnauthenticated();
    }
    if (kind === 'api_incompatible') {
      hooks.onApiIncompatible(apiError?.message ?? 'Unsupported API version');
    }

    throw new ApiFailure({
      message: apiError?.message ?? `HTTP ${resp.status}`,
      httpStatus: resp.status,
      requestId,
      retryAfterMs,
      apiError,
      kind,
    });
  }

  return {
    async request<TResponse, TBody = never>(options: RequestOptions<TBody>): Promise<TResponse> {
      const resp = await executeRaw(options as RequestOptions<unknown>);
      if (!resp.ok) {
        await handleErrorResponse(resp, Boolean(options.suppressAuthHook));
      }
      if (resp.status === 204 || options.method === 'HEAD') {
        return undefined as unknown as TResponse;
      }
      const env = (await resp.json()) as SuccessEnvelope<TResponse>;
      if (env.api_version && env.api_version.major !== CLIENT_API_MAJOR) {
        const msg = `Response envelope API major ${env.api_version.major} incompatible`;
        hooks.onApiIncompatible(msg);
        throw new ApiFailure({
          message: msg,
          httpStatus: resp.status,
          requestId: env.request_id ?? resp.headers.get('RoDAQ-Request-ID'),
          retryAfterMs: null,
          apiError: null,
          kind: 'api_incompatible',
        });
      }
      return env.data;
    },

    async requestRawConfig(path: string, signal?: AbortSignal): Promise<ConfigExportResult> {
      const resp = await executeRaw({
        method: 'GET',
        path,
        timeoutMs: CLIENT_TIMEOUTS.query,
        signal,
      });
      if (!resp.ok) {
        await handleErrorResponse(resp);
      }
      const rawJson = await resp.text();
      const config = JSON.parse(rawJson) as PortableTaskConfig;
      return {
        rawJson,
        config,
        etag: resp.headers.get('ETag'),
        requestId: resp.headers.get('RoDAQ-Request-ID'),
      };
    },

    async head(path: string, signal?: AbortSignal): Promise<HeadMetadata> {
      const resp = await executeRaw({
        method: 'HEAD',
        path,
        timeoutMs: CLIENT_TIMEOUTS.headPreflight,
        signal,
      });
      if (!resp.ok) {
        await handleErrorResponse(resp);
      }
      return {
        contentLength: resp.headers.get('Content-Length'),
        etag: resp.headers.get('ETag'),
        acceptRanges: resp.headers.get('Accept-Ranges'),
        contentType: resp.headers.get('Content-Type'),
        requestId: resp.headers.get('RoDAQ-Request-ID'),
      };
    },

    async downloadBlob(
      path: string,
      signal?: AbortSignal,
    ): Promise<{ blob: Blob; contentDisposition: string | null }> {
      const headers: Record<string, string> = {
        'RoDAQ-API-Version': CLIENT_API_VERSION_HEADER,
      };
      const token = hooks.getBearerToken();
      if (token) {
        headers['Authorization'] = `Bearer ${token}`;
      }
      const resp = await fetchImpl(path, {
        method: 'GET',
        headers,
        credentials: 'omit',
        signal,
      });
      if (!resp.ok) {
        await handleErrorResponse(resp);
      }
      const blob = await resp.blob();
      return { blob, contentDisposition: resp.headers.get('Content-Disposition') };
    },
  };
}
