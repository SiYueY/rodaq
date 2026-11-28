import { describe, expect, it, vi } from 'vitest';
import { flushPromises, mount } from '@vue/test-utils';
import { createMemoryHistory, createRouter } from 'vue-router';
import { ApiFailure } from '../../src/api/errors';
import type {
  OperationPageData,
  PipelineRuntimeData,
  StreamRuntimeData,
  TaskData,
} from '../../src/api/generated';
import ErrorPanel from '../../src/components/ErrorPanel.vue';
import ConfirmDialog from '../../src/components/ConfirmDialog.vue';
import TaskControlPanel from '../../src/features/tasks/TaskControlPanel.vue';
import TaskDetailPage from '../../src/features/tasks/TaskDetailPage.vue';
import StreamRuntimeTable, {
  formatZeroHzLabel,
} from '../../src/features/tasks/runtime/StreamRuntimeTable.vue';
import OperationTable from '../../src/features/tasks/operations/OperationTable.vue';
import { createSessionStore, SESSION_STORE_KEY } from '../../src/composables/useSession';

function makeRouter() {
  return createRouter({
    history: createMemoryHistory(),
    routes: [
      { path: '/tasks/:taskId', name: 'task-detail', component: TaskDetailPage },
      { path: '/:pathMatch(.*)*', component: { template: '<div />' } },
    ],
  });
}

function sampleTask(overrides: Partial<TaskData> = {}): TaskData {
  return {
    task_id: '11111111-1111-4111-8111-111111111111',
    name: 'demo-task',
    config_version: 'cfg-1',
    desired_state: 'stopped',
    execution_condition: 'idle',
    control_generation: '3',
    current_operation_id: null,
    data_impact: 'none',
    snapshot_sequence: '1',
    captured_at: '2026-11-27T12:00:00Z',
    freshness: { state: 'fresh', age_ms: 0 },
    ...overrides,
  };
}

describe('ErrorPanel XSS safety and metadata', () => {
  it('renders attacker-supplied script payloads strictly as plain text with request_id', () => {
    const xssPayload = '<img src=x onerror="window.__xss=1"><script>alert(1)</script>';
    const err = new ApiFailure({
      message: xssPayload,
      httpStatus: 400,
      requestId: 'req-xss-123',
      retryAfterMs: null,
      apiError: {
        code: 'validation_failed',
        message: xssPayload,
        retryability: 'never',
        failure_domain: 'request',
        data_impact: 'none',
        details: [{ path: '/name', code: 'invalid', message: xssPayload }],
      },
      kind: 'validation',
    });

    const wrapper = mount(ErrorPanel, { props: { error: err } });
    expect(wrapper.find('script').exists()).toBe(false);
    expect(wrapper.find('img').exists()).toBe(false);
    expect(wrapper.text()).toContain(xssPayload);
    expect(wrapper.text()).toContain('req-xss-123');
    expect(wrapper.text()).toContain('validation_failed');
  });
});

describe('TaskControlPanel 4-state semantics & ConfirmDialog', () => {
  it('keeps Stop enabled in Degraded state and disables Reset/Retire while running', async () => {
    const router = makeRouter();
    await router.push('/');
    await router.isReady();

    const wrapper = mount(TaskControlPanel, {
      global: { plugins: [router] },
      props: {
        task: sampleTask({ desired_state: 'running', execution_condition: 'degraded' }),
        busy: false,
      },
    });

    const stopBtn = wrapper.findAll('button').find((b) => b.text() === 'Stop');
    const resetBtn = wrapper.findAll('button').find((b) => b.text() === 'Reset Task');
    expect(stopBtn?.attributes('disabled')).toBeUndefined();
    expect(resetBtn?.attributes('disabled')).toBeDefined();
  });

  it('requires explicit ConfirmDialog for Reset/Retire and supports Escape', async () => {
    const router = makeRouter();
    await router.push('/');
    await router.isReady();

    const wrapper = mount(TaskControlPanel, {
      global: { plugins: [router] },
      props: {
        task: sampleTask({ desired_state: 'stopped', execution_condition: 'idle' }),
        busy: false,
      },
    });

    const resetBtn = wrapper.findAll('button').find((b) => b.text() === 'Reset Task');
    await resetBtn!.trigger('click');

    const dialog = wrapper.find('[role="dialog"]');
    expect(dialog.exists()).toBe(true);
    expect(dialog.text()).toContain('Confirm Reset Task');

    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
    await wrapper.vm.$nextTick();
    expect(wrapper.emitted('operate')).toBeUndefined();

    await resetBtn!.trigger('click');
    const confirmBtn = wrapper
      .find('[role="dialog"]')
      .findAll('button')
      .find((b) => b.text() === 'Reset Task');
    await confirmBtn!.trigger('click');
    expect(wrapper.emitted('operate')).toEqual([['reset']]);
  });

  it('ConfirmDialog renders modal attributes and emits cancel on Cancel click', async () => {
    const wrapper = mount(ConfirmDialog, {
      props: { open: true, title: 'Confirm Action', confirmLabel: 'Proceed' },
    });
    expect(wrapper.find('[role="dialog"]').attributes('aria-modal')).toBe('true');
    const cancelBtn = wrapper.findAll('button').find((b) => b.text() === 'Cancel');
    await cancelBtn!.trigger('click');
    expect(wrapper.emitted('cancel')).toHaveLength(1);
  });
});

describe('TaskDetailPage route A -> B stale snapshot guard & 504 unknown outcome (WEB-E2E-005/006)', () => {
  it('hides controls during A -> B transition before B snapshot arrives and shows unknown outcome on 504 without auto-resend', async () => {
    let resolveTaskB!: (resp: Response) => void;
    let operationPostCount = 0;

    const mockFetch = vi.fn<typeof fetch>(async (input, init) => {
      const url = String(input);
      if (url === '/api/system') {
        return new Response(
          JSON.stringify({
            api_version: { major: 1, minor: 0 },
            request_id: 'r-sys',
            data: {
              server_phase: 'ready',
              core_ready: true,
              resources: { cpu_cores: 0.1, rss_bytes: 100, threads: 2, open_fds: 10 },
              volumes: [],
              backlog: { commit_items: 0, commit_bytes: 0, active_downloads: 0 },
              captured_at: '2026-11-27T12:00:00Z',
              snapshot_sequence: '1',
              freshness: { state: 'fresh', age_ms: 0 },
            },
          }),
          { status: 200, headers: { 'Content-Type': 'application/json', 'RoDAQ-API-Version': '1.0' } },
        );
      }
      if (url === '/api/capabilities') {
        return new Response(
          JSON.stringify({
            api_version: { major: 1, minor: 0 },
            request_id: 'r-cap',
            data: {
              application_version: '0.1.0',
              backends: [],
              output_roots: [],
              resource_profiles: [],
              api_limits: {
                max_request_body_bytes: 4194304,
                max_page_size: 200,
                recommended_polling_floor_ms: 500,
              },
            },
          }),
          { status: 200, headers: { 'Content-Type': 'application/json', 'RoDAQ-API-Version': '1.0' } },
        );
      }
      if (url === '/api/tasks/task-A') {
        return new Response(
          JSON.stringify({
            api_version: { major: 1, minor: 0 },
            request_id: 'r-a',
            data: sampleTask({ task_id: 'task-A', name: 'Task A' }),
          }),
          { status: 200, headers: { 'Content-Type': 'application/json', 'RoDAQ-API-Version': '1.0' } },
        );
      }
      if (url === '/api/tasks/task-B') {
        return new Promise<Response>((resolve) => {
          resolveTaskB = resolve;
        });
      }
      if (url.endsWith('/runtime')) {
        const tid = url.includes('task-B') ? 'task-B' : 'task-A';
        return new Response(
          JSON.stringify({
            api_version: { major: 1, minor: 0 },
            request_id: 'r-rt',
            data: {
              task_id: tid,
              captured_at: '2026-11-27T12:00:00Z',
              snapshot_sequence: '1',
              freshness: { state: 'fresh', age_ms: 0 },
              pipelines: [],
            },
          }),
          { status: 200, headers: { 'Content-Type': 'application/json', 'RoDAQ-API-Version': '1.0' } },
        );
      }
      if (url === '/api/tasks/task-A/operations' && init?.method === 'POST') {
        operationPostCount += 1;
        return new Response(
          JSON.stringify({
            api_version: { major: 1, minor: 0 },
            request_id: 'r-504',
            error: {
              code: 'observer_timeout',
              message: 'observer deadline exceeded',
              retryability: 'after_backoff',
              failure_domain: 'request',
              data_impact: 'none',
              details: [],
            },
          }),
          { status: 504, headers: { 'Content-Type': 'application/json', 'RoDAQ-API-Version': '1.0' } },
        );
      }
      return new Response('{}', { status: 404 });
    });

    const store = createSessionStore(mockFetch);
    await store.login('memory-only-bearer-token-0123456789abcdef');
    const router = makeRouter();
    await router.push('/tasks/task-A');
    await router.isReady();

    const wrapper = mount(TaskDetailPage, {
      global: {
        plugins: [router],
        provide: { [SESSION_STORE_KEY as symbol]: store },
      },
    });
    await flushPromises();
    expect(wrapper.find('[data-testid="task-header"]').text()).toContain('stopped');

    // Trigger Start on Task A -> 504 returns outcome-unknown banner and NEVER auto-resends!
    const startBtn = wrapper.findAll('button').find((b) => b.text() === 'Start');
    await startBtn!.trigger('click');
    await flushPromises();
    expect(operationPostCount).toBe(1);
    expect(wrapper.text()).toContain('Request outcome is unknown');

    // Navigate A -> B while B snapshot is still pending: controls MUST NOT render stale A!
    await router.push('/tasks/task-B');
    await wrapper.vm.$nextTick();
    expect(wrapper.find('[aria-label="Task Controls"]').exists()).toBe(false);
    expect(wrapper.text()).toContain('Loading authoritative snapshot for Task task-B');

    // Once B snapshot arrives, controls render for B cleanly.
    resolveTaskB(
      new Response(
        JSON.stringify({
          api_version: { major: 1, minor: 0 },
          request_id: 'r-b',
          data: sampleTask({ task_id: 'task-B', name: 'Task B' }),
        }),
        { status: 200, headers: { 'Content-Type': 'application/json', 'RoDAQ-API-Version': '1.0' } },
      ),
    );
    await flushPromises();
    expect(wrapper.find('[aria-label="Task Controls"]').exists()).toBe(true);
    expect(wrapper.text()).toContain('Task B');
    wrapper.unmount();
  });
});

describe('StreamRuntimeTable 0 Hz semantics & 200-row windowing', () => {
  function makeStream(id: string, rate = 0, sourceReady = false): StreamRuntimeData {
    return {
      stream_id: id,
      stream_key: `key-${id}`,
      name: `stream-${id}`,
      entry_ready: true,
      source_ready: sourceReady,
      receiving: rate > 0,
      last_receive_at: null,
      window_ms: 1000,
      received: {
        records: 0,
        logical_bytes: 0,
        records_per_second: rate,
        logical_bytes_per_second: 0,
      },
      admission: {
        accepted_records: 0,
        rejected_records: 0,
        reject_reasons: [],
      },
      captured_at: '2026-11-27T12:00:00Z',
      freshness: { state: 'fresh', age_ms: 0 },
    };
  }

  it('distinguishes 0 Hz no source vs 0 Hz source present/silent and windows >200 rows', () => {
    expect(formatZeroHzLabel(makeStream('1', 0, false)).label).toBe('0 Hz · no source');
    expect(formatZeroHzLabel(makeStream('2', 0, true)).label).toBe('0 Hz · source present/silent');
    expect(formatZeroHzLabel(makeStream('3', 25, true)).label).toBe('25.0 Hz');

    const streams = Array.from({ length: 250 }, (_, idx) =>
      makeStream(String(idx + 1), idx === 0 ? 12.5 : 0, idx % 2 === 0),
    );
    const pipeline: PipelineRuntimeData = {
      pipeline_id: '1',
      required: true,
      instance_id: 'inst-1',
      condition: 'active',
      gate_state: 'open',
      received: {
        records: 100,
        logical_bytes: 1024,
        records_per_second: 12.5,
        logical_bytes_per_second: 1024,
      },
      admission: {
        accepted_records: 100,
        rejected_records: 0,
        reject_reasons: [],
      },
      queue: {
        records: 0,
        bytes: 0,
        high_watermark_records: 4,
        high_watermark_bytes: 256,
        oldest_age_ms: null,
      },
      writer: {
        logical_bytes_per_second: 1024,
        pending_records: 0,
        last_progress_at: null,
      },
      storage: {
        physical_bytes_per_second: 1024,
        pending_bytes: 0,
        last_progress_at: null,
      },
      commit: {
        lag_ms: null,
        durable_bytes_per_second: 0,
        pending_segments: 0,
        last_committed_artifact_id: null,
        last_progress_at: null,
      },
      streams,
    };

    const wrapper = mount(StreamRuntimeTable, {
      props: { pipelines: [pipeline], stale: false },
    });
    expect(wrapper.text()).toContain('Streams (250)');
    expect(wrapper.text()).toContain('Next 200');
    expect(wrapper.findAll('tbody tr')).toHaveLength(200);
    expect(wrapper.text()).toContain('0 Hz · no source');
    expect(wrapper.text()).toContain('0 Hz · source present/silent');
  });
});

describe('OperationTable truncation notice', () => {
  it('renders history_truncated warning and operation rows cleanly', () => {
    const page: OperationPageData = {
      history_truncated: true,
      items: [
        {
          operation_id: 'op-1',
          task_id: 't-1',
          kind: 'start',
          control_generation: '14',
          state: 'succeeded',
          accepted_at: '2026-11-27T12:00:00Z',
          terminal_at: '2026-11-27T12:00:01Z',
          retryability: null,
          failure_domain: null,
          data_impact: 'none',
          message: null,
        },
      ],
      next_cursor: '',
      has_more: false,
    };
    const wrapper = mount(OperationTable, {
      global: { provide: { [SESSION_STORE_KEY as symbol]: createSessionStore() } },
      props: { taskId: 't-1', page, hasPrev: false, error: null },
    });
    expect(wrapper.text()).toContain('Older operation history exceeds');
    expect(wrapper.text()).toContain('op-1');
  });
});
