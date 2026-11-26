import { describe, expect, it, vi } from 'vitest';
import { ApiFailure } from '../../src/api/errors';
import { usePolling } from '../../src/composables/usePolling';
import { useRequest } from '../../src/composables/useRequest';

describe('usePolling scheduler (WEB-E2E-013)', () => {
  it('schedules post-completion, honors floor, hidden cadence, and Retry-After', async () => {
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
            message: 'Too many requests',
            httpStatus: 429,
            requestId: 'req-429',
            retryAfterMs: 7_000,
            apiError: {
              code: 'rate_limited',
              message: 'slow down',
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

    // Advance by 1s -> 2nd call throws 429 with Retry-After 7s.
    now += 1_000;
    await vi.advanceTimersByTimeAsync(1_000);
    expect(calls).toBe(2);
    expect(handle.state.phase).toBe('stale');
    expect(handle.state.data).toBe(1);

    // Must NOT poll at +1s, +3s, or +6s during Retry-After suppression.
    now += 6_000;
    await vi.advanceTimersByTimeAsync(6_000);
    expect(calls).toBe(2);

    // Switch to hidden before call 3 completes so its post-completion timer uses hiddenMs (10s).
    hidden = true;
    floorMs = 1_000;
    now += 1_000;
    await vi.advanceTimersByTimeAsync(1_000);
    expect(calls).toBe(3);
    expect(handle.state.phase).toBe('success');
    expect(handle.state.data).toBe(3);

    // Must NOT fire at +5s under hidden 10s cadence.
    now += 5_000;
    await vi.advanceTimersByTimeAsync(5_000);
    expect(calls).toBe(3);

    handle.stop();
    now += 20_000;
    await vi.advanceTimersByTimeAsync(20_000);
    expect(calls).toBe(3);
    vi.useRealTimers();
  });

  it('stops polling immediately on 401 unauthenticated and discards late stop responses', async () => {
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
