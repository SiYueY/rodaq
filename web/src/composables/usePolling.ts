import { shallowReactive, shallowReadonly } from 'vue';
import type { RequestState } from './useRequest';
import { ApiFailure } from '../api/errors';

export interface PollingOptions<T> {
  resourceKey: string;
  enabled: () => boolean;
  fetcher: (signal: AbortSignal) => Promise<T>;
  foregroundMs: number;
  hiddenMs: number;
  maxBackoffMs: number;
  getFloorMs?: () => number;
  isHidden?: () => boolean;
  nowFn?: () => number;
  setTimer?: typeof globalThis.setTimeout;
  clearTimer?: typeof globalThis.clearTimeout;
}

export interface PollingHandle<T> {
  readonly state: Readonly<RequestState<T>>;
  start(): void;
  refreshNow(): void;
  stop(): void;
  reset(): void;
}

const BACKOFF_LADDER_MS = [1_000, 2_000, 5_000, 10_000, 15_000] as const;

export function usePolling<T>(options: PollingOptions<T>): PollingHandle<T> {
  const nowFn = options.nowFn ?? (() => Date.now());
  const setTimer = options.setTimer ?? globalThis.setTimeout.bind(globalThis);
  const clearTimer = options.clearTimer ?? globalThis.clearTimeout.bind(globalThis);
  const isHidden =
    options.isHidden ?? (() => typeof document !== 'undefined' && document.visibilityState === 'hidden');

  const state = shallowReactive<RequestState<T>>({
    phase: 'idle',
    data: null,
    error: null,
    generation: 0,
    receivedAt: null,
  });

  let running = false;
  let inFlightGen: number | null = null;
  let failureStreak = 0;
  let retryAfterDeadlineMs = 0;
  let timerId: ReturnType<typeof globalThis.setTimeout> | null = null;
  let activeController: AbortController | null = null;

  function clearScheduled(): void {
    if (timerId !== null) {
      clearTimer(timerId);
      timerId = null;
    }
  }

  function nextCadenceMs(): number {
    const floor = Math.max(100, options.getFloorMs ? options.getFloorMs() : 0);
    const base = isHidden() ? options.hiddenMs : options.foregroundMs;
    let delay = Math.max(base, floor);
    if (failureStreak > 0) {
      const ladderIdx = Math.min(failureStreak - 1, BACKOFF_LADDER_MS.length - 1);
      delay = Math.max(delay, Math.min(BACKOFF_LADDER_MS[ladderIdx], options.maxBackoffMs));
    }
    const now = nowFn();
    if (retryAfterDeadlineMs > now) {
      delay = Math.max(delay, retryAfterDeadlineMs - now);
    }
    return delay;
  }

  function scheduleNext(gen: number): void {
    clearScheduled();
    if (!running || gen !== state.generation) {
      return;
    }
    const delay = nextCadenceMs();
    timerId = setTimer(() => {
      timerId = null;
      void tick(gen);
    }, delay);
  }

  async function tick(gen: number): Promise<void> {
    if (!running || gen !== state.generation) {
      return;
    }
    if (!options.enabled()) {
      scheduleNext(gen);
      return;
    }
    if (inFlightGen === gen) {
      return;
    }
    inFlightGen = gen;
    const controller = new AbortController();
    activeController = controller;
    if (state.data === null) {
      state.phase = 'loading';
    }

    try {
      const result = await options.fetcher(controller.signal);
      if (!running || gen !== state.generation) {
        return;
      }
      failureStreak = 0;
      retryAfterDeadlineMs = 0;
      state.data = result;
      state.error = null;
      state.phase = 'success';
      state.receivedAt = nowFn();
    } catch (err) {
      if (!running || gen !== state.generation || controller.signal.aborted) {
        return;
      }
      const failure =
        err instanceof ApiFailure
          ? err
          : new ApiFailure({
              message: err instanceof Error ? err.message : 'Poll failed',
              httpStatus: null,
              requestId: null,
              retryAfterMs: null,
              apiError: null,
              kind: 'network',
            });
      state.error = failure;
      state.phase = state.data !== null ? 'stale' : 'error';

      if (failure.kind === 'unauthenticated' || failure.kind === 'api_incompatible') {
        running = false;
        clearScheduled();
        return;
      }

      failureStreak += 1;
      if (failure.httpStatus === 503 && failure.retryAfterMs !== null) {
        retryAfterDeadlineMs = Math.max(retryAfterDeadlineMs, nowFn() + failure.retryAfterMs);
      }
    } finally {
      // Strictly generation-scoped: late completion of an old generation never
      // clears or disrupts a newer generation's in-flight state or timer (Web §3.4).
      if (gen === state.generation) {
        if (activeController === controller) {
          activeController = null;
        }
        if (inFlightGen === gen) {
          inFlightGen = null;
        }
        if (running) {
          scheduleNext(gen);
        }
      }
    }
  }

  function onVisibilityChange(): void {
    if (!running) {
      return;
    }
    if (!isHidden() && inFlightGen !== state.generation && nowFn() >= retryAfterDeadlineMs) {
      clearScheduled();
      void tick(state.generation);
    }
  }

  function start(): void {
    if (running) {
      return;
    }
    running = true;
    state.generation += 1;
    inFlightGen = null;
    if (typeof document !== 'undefined') {
      document.addEventListener('visibilitychange', onVisibilityChange);
    }
    void tick(state.generation);
  }

  function refreshNow(): void {
    if (!running) {
      start();
      return;
    }
    if (inFlightGen === state.generation) {
      return;
    }
    clearScheduled();
    void tick(state.generation);
  }

  function stop(): void {
    running = false;
    state.generation += 1;
    inFlightGen = null;
    clearScheduled();
    if (activeController) {
      activeController.abort();
      activeController = null;
    }
    if (typeof document !== 'undefined') {
      document.removeEventListener('visibilitychange', onVisibilityChange);
    }
  }

  function reset(): void {
    stop();
    failureStreak = 0;
    retryAfterDeadlineMs = 0;
    state.phase = 'idle';
    state.data = null;
    state.error = null;
    state.receivedAt = null;
  }

  return {
    state: shallowReadonly(state),
    start,
    refreshNow,
    stop,
    reset,
  };
}
