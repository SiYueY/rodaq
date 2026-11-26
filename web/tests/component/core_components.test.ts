import { describe, expect, it } from 'vitest';
import { mount } from '@vue/test-utils';
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
import StreamRuntimeTable, {
  formatZeroHzLabel,
} from '../../src/features/tasks/runtime/StreamRuntimeTable.vue';
import OperationTable from '../../src/features/tasks/operations/OperationTable.vue';
import { createSessionStore, SESSION_STORE_KEY } from '../../src/composables/useSession';

function makeRouter() {
  return createRouter({
    history: createMemoryHistory(),
    routes: [{ path: '/:pathMatch(.*)*', component: { template: '<div />' } }],
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
    captured_at: '2026-11-26T12:00:00Z',
    freshness: { state: 'fresh', age: { value: '0', unit: 'ms' } },
    ...overrides,
  };
}

describe('ErrorPanel XSS safety and metadata (WEB-E2E-018)', () => {
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

describe('TaskControlPanel 4-state semantics & ConfirmDialog (WEB-E2E-005/007/008)', () => {
  it('keeps Stop enabled in Degraded state and hides Reset/Retire for non-admin', async () => {
    const router = makeRouter();
    await router.push('/');
    await router.isReady();

    const wrapper = mount(TaskControlPanel, {
      global: { plugins: [router] },
      props: {
        task: sampleTask({ desired_state: 'running', execution_condition: 'degraded' }),
        canControl: true,
        canAdmin: false,
        busy: false,
      },
    });

    const stopBtn = wrapper.findAll('button').find((b) => b.text() === 'Stop');
    expect(stopBtn).toBeDefined();
    expect(stopBtn?.attributes('disabled')).toBeUndefined();
    expect(wrapper.text()).not.toContain('Reset Task');
    expect(wrapper.text()).not.toContain('Retire Task');
  });

  it('requires explicit ConfirmDialog with explicit action verb for Reset/Retire and supports Escape', async () => {
    const router = makeRouter();
    await router.push('/');
    await router.isReady();

    const wrapper = mount(TaskControlPanel, {
      global: { plugins: [router] },
      props: {
        task: sampleTask({ desired_state: 'stopped', execution_condition: 'idle' }),
        canControl: true,
        canAdmin: true,
        busy: false,
      },
    });

    const resetBtn = wrapper.findAll('button').find((b) => b.text() === 'Reset Task');
    await resetBtn!.trigger('click');

    const dialog = wrapper.find('[role="dialog"]');
    expect(dialog.exists()).toBe(true);
    expect(dialog.text()).toContain('Confirm Reset Task');
    expect(dialog.text()).toContain('permanently deleted');

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

describe('StreamRuntimeTable 0 Hz semantics & 200-row windowing (WEB-E2E-004)', () => {
  function makeStream(id: string, rate = 0, sourceReady = false): StreamRuntimeData {
    return {
      stream_id: id,
      stream_key: `key-${id}`,
      name: `stream-${id}`,
      entry_ready: true,
      source_ready: sourceReady,
      receiving: rate > 0,
      last_receive_at: null,
      window: { value: '1000', unit: 'ms' },
      received: {
        records: '0',
        logical_bytes: '0',
        records_per_second: rate,
        logical_bytes_per_second: 0,
      },
      admission: {
        accepted_records: '0',
        rejected_records: '0',
        reject_reasons: [],
      },
      captured_at: '2026-11-26T12:00:00Z',
      freshness: { state: 'fresh', age: { value: '0', unit: 'ms' } },
    };
  }

  it('distinguishes 0 Hz no source vs 0 Hz source present/silent and windows >200 rows', () => {
    expect(formatZeroHzLabel(makeStream('1', 0, false)).label).toBe('0 Hz · no source');
    expect(formatZeroHzLabel(makeStream('2', 0, true)).label).toBe('0 Hz · source present/silent');
    expect(formatZeroHzLabel(makeStream('3', 25, true)).label).toBe('25.0 Hz');

    const streams = Array.from({ length: 250 }, (_, idx) =>
      makeStream(`s${idx}`, idx === 0 ? 12.5 : 0, idx % 2 === 0),
    );
    const pipeline: PipelineRuntimeData = {
      pipeline_id: 'pipe-1',
      required: true,
      instance_id: 'inst-1',
      condition: 'active',
      gate_state: 'open',
      received: {
        records: '100',
        logical_bytes: '1024',
        records_per_second: 12.5,
        logical_bytes_per_second: 1024,
      },
      admission: {
        accepted_records: '100',
        rejected_records: '0',
        reject_reasons: [],
      },
      queue: {
        records: '0',
        bytes: '0',
        high_watermark_records: '4',
        high_watermark_bytes: '256',
        oldest_age: null,
      },
      writer: {
        logical_bytes_per_second: 1024,
        pending_records: '0',
        last_progress_at: null,
      },
      storage: {
        physical_bytes_per_second: 1024,
        pending_bytes: '0',
        last_progress_at: null,
      },
      commit: {
        lag: null,
        durable_bytes_per_second: 0,
        pending_segments: '0',
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

describe('OperationTable truncation notice (WEB-E2E-006)', () => {
  it('renders history_truncated warning and operation rows cleanly', () => {
    const page: OperationPageData = {
      history_truncated: true,
      oldest_retained_control_generation: '12',
      items: [
        {
          operation_id: 'op-1',
          task_id: 't-1',
          kind: 'start',
          control_generation: '14',
          state: 'succeeded',
          accepted_at: '2026-11-26T12:00:00Z',
          terminal_at: '2026-11-26T12:00:01Z',
          retryability: null,
          failure_domain: null,
          data_impact: 'none',
          message: null,
        },
      ],
      next_cursor: '',
      has_more: false,
      oldest_retained_at: null,
    };
    const wrapper = mount(OperationTable, {
      global: { provide: { [SESSION_STORE_KEY as symbol]: createSessionStore() } },
      props: { taskId: 't-1', page, hasPrev: false, error: null },
    });
    expect(wrapper.text()).toContain('Older operation history exceeds the retained window');
    expect(wrapper.text()).toContain('12');
    expect(wrapper.text()).toContain('op-1');
  });
});
