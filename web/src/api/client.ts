import type { ApiError, DecimalString, ErrorEnvelope, SuccessEnvelope } from './types'

export const API_MAJOR = 1
export const API_VERSION_HEADER = '1.0'

export type ApiFailureKind =
  | 'network'
  | 'timeout'
  | 'unauthenticated'
  | 'forbidden'
  | 'api_incompatible'
  | 'server_error'
  | 'validation'

export class ApiFailure extends Error {
  readonly httpStatus: number | null
  readonly requestId: string | null
  readonly retryAfterMs: number | null
  readonly apiError: ApiError | null
  readonly kind: ApiFailureKind

  constructor(params: {
    message: string
    kind: ApiFailureKind
    httpStatus?: number | null
    requestId?: string | null
    retryAfterMs?: number | null
    apiError?: ApiError | null
  }) {
    super(params.message)
    this.name = 'ApiFailure'
    this.kind = params.kind
    this.httpStatus = params.httpStatus ?? null
    this.requestId = params.requestId ?? null
    this.retryAfterMs = params.retryAfterMs ?? null
    this.apiError = params.apiError ?? null
  }
}

export interface RequestOptions {
  method: 'GET' | 'POST' | 'PUT' | 'DELETE' | 'HEAD'
  path: string
  query?: Record<string, string | undefined>
  bodyText?: string
  timeoutMs?: number
  signal?: AbortSignal
  idempotencyKey?: string
  controlGeneration?: DecimalString
  bearerToken?: string
  rawResponse?: boolean
}

export interface ApiClientHooks {
  getCsrfToken: () => string | null
  onUnauthenticated: () => void
  onApiIncompatible: (reason: string) => void
}

export function parseRetryAfterMs(header: string | null): number | null {
  if (!header) return null
  const trimmed = header.trim()
  if (/^\d+$/.test(trimmed)) {
    return Number(trimmed) * 1000
  }
  const dateMs = Date.parse(trimmed)
  if (!Number.isNaN(dateMs)) {
    return Math.max(0, dateMs - Date.now())
  }
  return null
}

export function createApiClient(hooks: ApiClientHooks, fetchImpl: typeof fetch = fetch) {
  async function request<T>(opts: RequestOptions): Promise<T> {
    const url = new URL(opts.path, window.location.origin)
    if (opts.query) {
      for (const [k, v] of Object.entries(opts.query)) {
        if (v !== undefined && v !== '') {
          url.searchParams.set(k, v)
        }
      }
    }

    const headers = new Headers()
    headers.set('RoDAQ-API-Version', API_VERSION_HEADER)
    if (opts.bearerToken) {
      headers.set('Authorization', `Bearer ${opts.bearerToken}`)
    } else if (opts.method === 'POST' || opts.method === 'PUT' || opts.method === 'DELETE') {
      const csrf = hooks.getCsrfToken()
      if (csrf) {
        headers.set('RoDAQ-CSRF-Token', csrf)
      }
    }
    if (opts.idempotencyKey) {
      headers.set('Idempotency-Key', opts.idempotencyKey)
    }
    if (opts.controlGeneration) {
      headers.set('If-Control-Generation', opts.controlGeneration)
    }
    if (opts.bodyText !== undefined) {
      headers.set('Content-Type', 'application/json')
    }

    const controller = new AbortController()
    const timeoutMs = opts.timeoutMs ?? (opts.method === 'GET' || opts.method === 'HEAD' ? 10000 : 15000)
    const timer = window.setTimeout(() => controller.abort(), timeoutMs)
    const onCallerAbort = () => controller.abort()
    opts.signal?.addEventListener('abort', onCallerAbort, { once: true })

    let response: Response
    try {
      response = await fetchImpl(url.pathname + url.search, {
        method: opts.method,
        credentials: 'same-origin',
        headers,
        body: opts.bodyText,
        signal: controller.signal
      })
    } catch (err) {
      window.clearTimeout(timer)
      opts.signal?.removeEventListener('abort', onCallerAbort)
      const abortedByTimeout = controller.signal.aborted && !opts.signal?.aborted
      throw new ApiFailure({
        message: abortedByTimeout ? 'Request timed out' : 'Network request failed',
        kind: abortedByTimeout ? 'timeout' : 'network'
      })
    } finally {
      window.clearTimeout(timer)
      opts.signal?.removeEventListener('abort', onCallerAbort)
    }

    const requestId = response.headers.get('RoDAQ-Request-ID')
    const retryAfterMs = parseRetryAfterMs(response.headers.get('Retry-After'))
    const respVer = response.headers.get('RoDAQ-API-Version')
    if (respVer) {
      const major = Number(respVer.split('.')[0])
      if (major !== API_MAJOR) {
        hooks.onApiIncompatible(`Server API major ${respVer} incompatible with UI v${API_MAJOR}`)
        throw new ApiFailure({
          message: `Incompatible API version: ${respVer}`,
          kind: 'api_incompatible',
          httpStatus: response.status,
          requestId
        })
      }
    }

    if (response.status === 204 || opts.method === 'HEAD') {
      if (!response.ok) {
        if (response.status === 401) hooks.onUnauthenticated()
        throw new ApiFailure({
          message: `HTTP ${response.status}`,
          kind: response.status === 401 ? 'unauthenticated' : 'server_error',
          httpStatus: response.status,
          requestId,
          retryAfterMs
        })
      }
      return undefined as unknown as T
    }

    if (opts.rawResponse && response.ok) {
      return (await response.text()) as unknown as T
    }

    const text = await response.text()
    let parsed: unknown = null
    if (text.length > 0) {
      try {
        parsed = JSON.parse(text)
      } catch {
        throw new ApiFailure({
          message: `Malformed JSON response (HTTP ${response.status})`,
          kind: 'server_error',
          httpStatus: response.status,
          requestId
        })
      }
    }

    if (!response.ok) {
      const env = parsed as ErrorEnvelope | null
      const apiErr = env?.error ?? null
      if (response.status === 401) {
        hooks.onUnauthenticated()
      }
      if (apiErr?.code === 'unsupported_api_version') {
        hooks.onApiIncompatible(apiErr.message)
      }
      const kind: ApiFailureKind =
        response.status === 401
          ? 'unauthenticated'
          : response.status === 403
            ? 'forbidden'
            : apiErr?.code === 'unsupported_api_version'
              ? 'api_incompatible'
              : response.status === 400 || response.status === 409 || response.status === 413 || response.status === 415
                ? 'validation'
                : 'server_error'
      throw new ApiFailure({
        message: apiErr?.message ?? `HTTP ${response.status}`,
        kind,
        httpStatus: response.status,
        requestId: env?.request_id ?? requestId,
        retryAfterMs,
        apiError: apiErr
      })
    }

    const env = parsed as SuccessEnvelope<T>
    if (env.api_version && env.api_version.major !== API_MAJOR) {
      hooks.onApiIncompatible(`Envelope API major ${env.api_version.major} incompatible`)
      throw new ApiFailure({
        message: 'API major mismatch',
        kind: 'api_incompatible',
        httpStatus: response.status,
        requestId: env.request_id ?? requestId
      })
    }
    return env.data
  }

  return { request }
}
