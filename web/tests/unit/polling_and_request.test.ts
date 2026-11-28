import { describe, expect, it, vi } from 'vitest';
import { ApiFailure } from '../../src/api/errors';
import { usePolling } from '../../src/composables/usePolling';
import { useRequest } from '../../src/composables/useRequest';

describe('usePolling scheduler and generation isolation (WEB-E2E-007)', () => {
  it('schedules post-completion, honors floor, hidden cadence, and 503 Retry-After', async () => {
    vi.useFakeTimers();
    let now = 10_000;
    let hidden = false;
    let floorMs = 1_000;
    let calls = 0;

    const handle = usePolling<number>({
      resourceKey: 'overview',
      enabled: () => true,
      fetcher: async () => {
        calls += 1;
        if (calls === 2) {
          throw new ApiFailure({
            message: 'Server busy',
            httpStatus: 503,
            requestId: 'req-503',
            retryAfterMs: 7_000,
            apiError: {
              code: 'server_busy',
              message: 'saturated',
              retryability: 'after_backoff',
              failure_domain: 'request',
              data_impact: 'none',
              details: [],
            },
            kind: 'server_error',
          });
        }
        return calls;
      },
      foregroundMs: 1_000,
      hiddenMs: 10_000,
      maxBackoffMs: 15_000,
      getFloorMs: () => floorMs,
      isHidden: () => hidden,
      nowFn: () => now,
    });

    handle.start();
    await vi.advanceTimersByTimeAsync(0);
    expect(calls).toBe(1);
    expect(handle.state.phase).toBe('success');
    expect(handle.state.data).toBe(1);

    now += 1_000;
    await vi.advanceTimersByTimeAsync(1_000);
    expect(calls).toBe(2);
    expect(handle.state.phase).toBe('stale');
    expect(handle.state.data).toBe(1);

    now += 6_000;
    await vi.advanceTimersByTimeAsync(6_000);
    expect(calls).toBe(2);

    hidden = true;
    floorMs = 1_000;
    now += 1_000;
    await vi.advanceTimersByTimeAsync(1_000);
    expect(calls).toBe(3);
    expect(handle.state.phase).toBe('success');
    expect(handle.state.data).toBe(3);

    now += 5_000;
    await vi.advanceTimersByTimeAsync(5_000);
    expect(calls).toBe(3);

    handle.stop();
    now += 20_000;
    await vi.advanceTimersByTimeAsync(20_000);
    expect(calls).toBe(3);
    vi.useRealTimers();
  });

  it('prevents an old in-flight stop() finally from stalling a newly started generation (WEB-E2E-007)', async () => {
    vi.useFakeTimers();
    let resolveGen1!: (v: string) => void;
    let resolveGen2!: (v: string) => void;
    let call = 0;

    const handle = usePolling<string>({
      resourceKey: 'race-check',
      enabled: () => true,
      fetcher: () => {
        call += 1;
        if (call === 1) {
          return new Promise<string>((r) => {
            resolveGen1 = r;
          });
        }
        if (call === 2) {
          return new Promise<string>((r) => {
            resolveGen2 = r;
          });
        }
        return Promise.resolve(`gen2-tick-${call}`);
      },
      foregroundMs: 500,
      hiddenMs: 5_000,
      maxBackoffMs: 10_000,
    });

    // Start gen 1 -> request 1 is in-flight.
    handle.start();
    expect(call).toBe(1);

    // Stop gen 1 and immediately start gen 2 while request 1 is still unresolved!
    handle.stop();
    handle.start();
    expect(call).toBe(2);

    // Old gen 1 finally settles late: must NOT overwrite data or break gen 2!
    resolveGen1('stale-gen1');
    await Promise.resolve();
    await Promise.resolve();
    expect(handle.state.data).toBeNull();

    // Gen 2 first response arrives and schedules next tick cleanly.
    resolveGen2('fresh-gen2');
    await vi.advanceTimersByTimeAsync(0);
    expect(handle.state.data).toBe('fresh-gen2');

    // Gen 2 continues polling normally on timer.
    await vi.advanceTimersByTimeAsync(500);
    expect(call).toBe(3);
    expect(handle.state.data).toBe('gen2-tick-3');

    handle.stop();
    vi.useRealTimers();
  });

  it('stops polling immediately on 401 unauthenticated', async () => {
    vi.useFakeTimers();
    let calls = 0;
    const handle = usePolling<string>({
      resourceKey: 'auth-check',
      enabled: () => true,
      fetcher: async () => {
        calls += 1;
        throw new ApiFailure({
          message: 'Unauthorized',
          httpStatus: 401,
          requestId: 'req-401',
          retryAfterMs: null,
          apiError: {
            code: 'unauthenticated',
            message: 'login required',
            retryability: 'never',
            failure_domain: 'request',
            data_impact: 'none',
            details: [],
          },
          kind: 'unauthenticated',
        });
      },
      foregroundMs: 500,
      hiddenMs: 5_000,
      maxBackoffMs: 15_000,
    });

    handle.start();
    await vi.advanceTimersByTimeAsync(0);
    expect(calls).toBe(1);
    expect(handle.state.phase).toBe('error');

    await vi.advanceTimersByTimeAsync(30_000);
    expect(calls).toBe(1);
    vi.useRealTimers();
  });
});

describe('useRequest generation isolation', () => {
  it('discards stale out-of-order response when a newer request completes first', async () => {
    let resolveFirst!: (val: string) => void;
    let resolveSecond!: (val: string) => void;
    let call = 0;

    const req = useRequest<string>(
      () =>
        new Promise<string>((resolve) => {
          call += 1;
          if (call === 1) {
            resolveFirst = resolve;
          } else {
            resolveSecond = resolve;
          }
        }),
    );

    const p1 = req.run();
    const p2 = req.refresh();
    resolveSecond('fresh');
    await p2;
    expect(req.state.data).toBe('fresh');

    resolveFirst('stale-late');
    await p1;
    expect(req.state.data).toBe('fresh');
  });
});
