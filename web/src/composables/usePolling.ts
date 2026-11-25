import { onUnmounted, reactive } from 'vue'
import { ApiFailure } from '../api/client'

export interface PollingOptions<T> {
  fetcher: (signal: AbortSignal) => Promise<T>
  foregroundMs: number
  hiddenMs: number
  maxBackoffMs: number
  enabled?: () => boolean
}

export function usePolling<T>(opts: PollingOptions<T>) {
  const state = reactive({
    phase: 'idle' as 'idle' | 'loading' | 'success' | 'stale' | 'error',
    data: null as T | null,
    error: null as ApiFailure | null,
    receivedAt: null as number | null
  })

  let timer: number | null = null
  let controller: AbortController | null = null
  let stopped = false
  let generation = 0
  let consecutiveFailures = 0

  function clearTimer() {
    if (timer !== null) {
      window.clearTimeout(timer)
      timer = null
    }
  }

  function nextDelayMs(retryAfterMs: number | null): number {
    if (retryAfterMs !== null && retryAfterMs > 0) {
      return retryAfterMs
    }
    const base = document.hidden ? opts.hiddenMs : opts.foregroundMs
    if (consecutiveFailures === 0) return base
    const mult = Math.min(8, 1 << Math.min(3, consecutiveFailures - 1))
    return Math.min(opts.maxBackoffMs, base * mult)
  }

  async function runOnce(): Promise<void> {
    if (stopped) return
    if (opts.enabled && !opts.enabled()) {
      schedule(nextDelayMs(null))
      return
    }
    controller?.abort()
    controller = new AbortController()
    const myGen = ++generation
    if (state.data === null) {
      state.phase = 'loading'
    }
    let retryAfterMs: number | null = null
    try {
      const res = await opts.fetcher(controller.signal)
      if (stopped || myGen !== generation) return
      state.data = res as never
      state.error = null
      state.phase = 'success'
      state.receivedAt = Date.now()
      consecutiveFailures = 0
    } catch (err) {
      if (stopped || myGen !== generation) return
      const fail = err instanceof ApiFailure ? err : new ApiFailure({ message: String(err), kind: 'network' })
      state.error = fail
      state.phase = state.data !== null ? 'stale' : 'error'
      retryAfterMs = fail.retryAfterMs
      consecutiveFailures++
      if (fail.kind === 'unauthenticated' || fail.kind === 'api_incompatible') {
        stop()
        return
      }
    }
    schedule(nextDelayMs(retryAfterMs))
  }

  function schedule(delayMs: number) {
    if (stopped) return
    clearTimer()
    timer = window.setTimeout(() => {
      void runOnce()
    }, delayMs)
  }

  function start() {
    stopped = false
    void runOnce()
  }

  function refreshNow() {
    if (stopped) return
    clearTimer()
    void runOnce()
  }

  function stop() {
    stopped = true
    clearTimer()
    controller?.abort()
    controller = null
  }

  onUnmounted(stop)

  return { state, start, refreshNow, stop }
}
