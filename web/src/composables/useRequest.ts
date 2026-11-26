import { shallowReactive, shallowReadonly } from 'vue';
import type { ClientRequestPhase } from '../api/types';
import { ApiFailure } from '../api/errors';

export interface RequestState<T> {
  phase: ClientRequestPhase;
  data: T | null;
  error: ApiFailure | null;
  generation: number;
  receivedAt: number | null;
}

export interface RequestHandle<T> {
  readonly state: Readonly<RequestState<T>>;
  run(): Promise<void>;
  refresh(): Promise<void>;
  abort(): void;
}

export function useRequest<T>(
  fetcher: (signal: AbortSignal) => Promise<T>,
  nowFn: () => number = () => Date.now(),
): RequestHandle<T> {
  const state = shallowReactive<RequestState<T>>({
    phase: 'idle',
    data: null,
    error: null,
    generation: 0,
    receivedAt: null,
  });

  let activeController: AbortController | null = null;

  function abort(): void {
    state.generation += 1;
    if (activeController) {
      activeController.abort();
      activeController = null;
    }
  }

  async function execute(): Promise<void> {
    if (activeController) {
      activeController.abort();
    }
    const gen = state.generation + 1;
    state.generation = gen;
    const controller = new AbortController();
    activeController = controller;

    if (state.data === null) {
      state.phase = 'loading';
    }

    try {
      const result = await fetcher(controller.signal);
      if (gen !== state.generation) {
        return;
      }
      state.data = result;
      state.error = null;
      state.phase = 'success';
      state.receivedAt = nowFn();
    } catch (err) {
      if (gen !== state.generation || controller.signal.aborted) {
        return;
      }
      const failure =
        err instanceof ApiFailure
          ? err
          : new ApiFailure({
              message: err instanceof Error ? err.message : 'Request failed',
              httpStatus: null,
              requestId: null,
              retryAfterMs: null,
              apiError: null,
              kind: 'network',
            });
      state.error = failure;
      state.phase = state.data !== null ? 'stale' : 'error';
    } finally {
      if (activeController === controller) {
        activeController = null;
      }
    }
  }

  return {
    state: shallowReadonly(state),
    run: execute,
    refresh: execute,
    abort,
  };
}
