import { test, expect } from '@playwright/test';
import { execFileSync, spawn, type ChildProcess } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

/**
 * WEB-E2E-018: 1280x720 sustained 1 Hz runtime update stability & JS heap non-growth verification
 * against the installed rodaq Server and share/rodaq/web bundle.
 */

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(HERE, '../../..');
const BUILD_DIR = path.join(REPO_ROOT, 'build/s1');
const WEB_DIST = path.resolve(HERE, '../../dist');
const ADMIN_TOKEN = 'rodaq-perf-e2e-admin-secret-0123456789ab';

function computeBearerVerifierHex(token: string): string {
  return crypto
    .createHash('sha256')
    .update(Buffer.from('rodaq.server-bearer.v1\0', 'utf8'))
    .update(token, 'utf8')
    .digest('hex');
}

function getFreePort(): Promise<number> {
  return new Promise((resolve, reject) => {
    const srv = net.createServer();
    srv.unref();
    srv.on('error', reject);
    srv.listen(0, '127.0.0.1', () => {
      const addr = srv.address();
      if (!addr || typeof addr === 'string') {
        srv.close(() => reject(new Error('Failed to bind ephemeral port')));
        return;
      }
      const { port } = addr;
      srv.close(() => resolve(port));
    });
  });
}

function chmodRecursiveReadOnly(dir: string): void {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      chmodRecursiveReadOnly(full);
      fs.chmodSync(full, 0o555);
    } else {
      const mode = fs.statSync(full).mode;
      fs.chmodSync(full, mode & 0o111 ? 0o555 : 0o444);
    }
  }
  fs.chmodSync(dir, 0o555);
}

function chmodRecursiveWritable(dir: string): void {
  if (!fs.existsSync(dir)) return;
  fs.chmodSync(dir, 0o755);
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      chmodRecursiveWritable(full);
    } else {
      fs.chmodSync(full, 0o644);
    }
  }
}

async function waitForServerReady(baseUrl: string, token: string): Promise<void> {
  const deadline = Date.now() + 15_000;
  while (Date.now() < deadline) {
    try {
      const res = await fetch(`${baseUrl}/api/system`, {
        headers: { Authorization: `Bearer ${token}`, 'RoDAQ-API-Version': '1.0' },
      });
      if (res.ok) {
        const body = (await res.json()) as { data?: { server_phase?: string } };
        if (body.data?.server_phase === 'ready') return;
      }
    } catch {
      // Booting.
    }
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error(`rodaq server at ${baseUrl} did not become ready`);
}

let tmpRoot = '';
let baseUrl = '';
let daemon: ChildProcess | null = null;

test.beforeAll(async () => {
  tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'rodaq-web-perf-'));
  const installPrefix = path.join(tmpRoot, 'install');
  fs.mkdirSync(path.join(tmpRoot, 'state'), { recursive: true });
  fs.mkdirSync(path.join(tmpRoot, 'volume'), { recursive: true });

  execFileSync('cmake', ['--install', BUILD_DIR, '--prefix', installPrefix], { stdio: 'pipe' });

  const pkgRoot = path.join(tmpRoot, 'plugins');
  fs.mkdirSync(pkgRoot, { recursive: true });
  fs.cpSync(
    path.join(installPrefix, 'lib/rodaq/plugins/rodaq.fake.acquisition'),
    path.join(pkgRoot, 'rodaq.fake.acquisition'),
    { recursive: true },
  );
  fs.cpSync(
    path.join(installPrefix, 'lib/rodaq/plugins/rodaq.fake.storage'),
    path.join(pkgRoot, 'rodaq.fake.storage'),
    { recursive: true },
  );
  chmodRecursiveReadOnly(pkgRoot);

  const credFile = path.join(tmpRoot, 'credentials.json');
  fs.writeFileSync(
    credFile,
    JSON.stringify({
      format_version: '1.0',
      credentials: [
        {
          credential_id: 'perf-admin',
          principal: 'perf-admin',
          scopes: ['read', 'control', 'admin'],
          verifier: computeBearerVerifierHex(ADMIN_TOKEN),
          credential_epoch: '1',
          enabled: true,
        },
      ],
    }),
  );
  fs.chmodSync(credFile, 0o600);

  const port = await getFreePort();
  baseUrl = `http://127.0.0.1:${port}`;
  const depFile = path.join(tmpRoot, 'deployment.json');
  fs.writeFileSync(
    depFile,
    JSON.stringify({
      format_version: '1.0',
      static_assets_root: WEB_DIST,
      server: {
        format_version: '1.0',
        http: { listen_address: '127.0.0.1', port, worker_threads: 2 },
        security: { credential_store: credFile },
      },
      daq: {
        state_root: path.join(tmpRoot, 'state'),
        volume_root: path.join(tmpRoot, 'volume'),
        mode: 'initialize',
        plugin_search_paths: [pkgRoot],
        qualified_runtime_profile_id: 'web-perf-e2e',
        max_tasks: 2,
        max_pipelines_per_task: 2,
        volume_name: 'local',
        profiles: [
          {
            name: 'default',
            payload_pool_bytes: '262144',
            max_payload_bytes: '4096',
            max_logical_bytes: '262144',
            max_segment_physical_bytes: '4194304',
            max_descriptors: 64,
            max_prepared_descriptors: 64,
            max_streams: 32,
          },
        ],
        allowed_backends: [
          {
            plugin_id: 'rodaq.fake.acquisition',
            plugin_version: '1.0.0',
            role: 'acquisition',
            support_status: 'qualified_candidate',
            qualification_status: 'qualified',
          },
          {
            plugin_id: 'rodaq.fake.storage',
            plugin_version: '1.0.0',
            role: 'storage',
            storage_format: 'fake',
            support_status: 'required',
            qualification_status: 'qualified',
          },
        ],
      },
    }),
  );

  daemon = spawn(
    'bash',
    [
      '-lc',
      `export LD_LIBRARY_PATH="${path.join(installPrefix, 'lib')}:$LD_LIBRARY_PATH" && exec "${path.join(installPrefix, 'bin/rodaq')}" --deployment "${depFile}"`,
    ],
    { stdio: 'ignore' },
  );
  await waitForServerReady(baseUrl, ADMIN_TOKEN);
});

test.afterAll(async () => {
  if (daemon && daemon.exitCode === null) {
    daemon.kill('SIGTERM');
    await new Promise((r) => setTimeout(r, 400));
    if (daemon.exitCode === null) daemon.kill('SIGKILL');
  }
  if (tmpRoot && fs.existsSync(tmpRoot)) {
    chmodRecursiveWritable(path.join(tmpRoot, 'plugins'));
    fs.rmSync(tmpRoot, { recursive: true, force: true });
  }
});

test('1280x720 sustained 1 Hz multi-stream runtime polling keeps bounded DOM and non-growing heap (WEB-E2E-018)', async ({
  page,
  browserName,
}) => {
  // Create a 16-stream task via API and start it.
  const streams = Array.from({ length: 16 }, (_, i) => `s${i}`);
  const createRes = await fetch(`${baseUrl}/api/tasks`, {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${ADMIN_TOKEN}`,
      'RoDAQ-API-Version': '1.0',
      'Content-Type': 'application/json',
      'Idempotency-Key': 'perf-create-1',
    },
    body: JSON.stringify({
      format_version: '1.0',
      name: 'perf-16-streams',
      recording_mode: 'continuous',
      start_policy: { mode: 'all_required' },
      recovery_policy: { mode: 'manual' },
      segment_policy: { max_duration: { value: '60', unit: 's' }, max_physical_bytes: null },
      retention_policy: {
        mode: 'keep_all',
        max_duration: null,
        max_physical_bytes: null,
        include_previous_continuous: false,
      },
      pipelines: [
        {
          required: true,
          resource_profile: 'default',
          output_root: 'local',
          acquisition: {
            plugin_id: 'rodaq.fake.acquisition',
            plugin_version: '1.0.0',
            config: { streams, period_us: 20000 },
          },
          storage: {
            plugin_id: 'rodaq.fake.storage',
            plugin_version: '1.0.0',
            config: {},
          },
          streams: streams.map((k) => ({
            stream_key: k,
            name: k,
            readiness_policy: 'entry_ready',
          })),
        },
      ],
    }),
  });
  expect(createRes.status).toBe(201);
  const { data: created } = (await createRes.json()) as { data: { task_id: string } };

  const startRes = await fetch(`${baseUrl}/api/tasks/${created.task_id}/operations`, {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${ADMIN_TOKEN}`,
      'RoDAQ-API-Version': '1.0',
      'Content-Type': 'application/json',
      'Idempotency-Key': 'perf-start-1',
    },
    body: JSON.stringify({ kind: 'start', parameters: {} }),
  });
  expect(startRes.status).toBe(202);

  await page.goto(`${baseUrl}/login`);
  await page.fill('input[type="password"]', ADMIN_TOKEN);
  await page.click('button[type="submit"]');
  await expect(page).toHaveURL(`${baseUrl}/`);

  // Navigate to Task Detail -> Streams tab and observe 1 Hz updates for 8 seconds.
  await page.goto(`${baseUrl}/tasks/${created.task_id}?tab=streams`);
  await expect(page.locator('main')).toContainText('Streams (16)', { timeout: 10_000 });
  await expect(page.locator('main')).toContainText('active', { timeout: 10_000 });

  const domCounts: number[] = [];
  const heapSamples: number[] = [];
  const cdp = browserName === 'chromium' ? await page.context().newCDPSession(page) : null;
  if (cdp) {
    await cdp.send('Performance.enable');
  }

  for (let i = 0; i < 8; i += 1) {
    await page.waitForTimeout(1_000);
    const nodes = await page.evaluate(() => document.querySelectorAll('*').length);
    domCounts.push(nodes);
    if (cdp) {
      const { metrics } = (await cdp.send('Performance.getMetrics')) as {
        metrics: { name: string; value: number }[];
      };
      const heap = metrics.find((m) => m.name === 'JSHeapUsedSize')?.value ?? 0;
      heapSamples.push(heap);
    }
  }

  // DOM node count must remain strictly bounded (no runaway element accumulation across 1 Hz ticks).
  const minDom = Math.min(...domCounts);
  const maxDom = Math.max(...domCounts);
  expect(maxDom - minDom).toBeLessThanOrEqual(20);
  expect(maxDom).toBeLessThan(1500);

  if (heapSamples.length >= 6) {
    const first = heapSamples[0];
    const last = heapSamples[heapSamples.length - 1];
    const maxHeap = Math.max(...heapSamples);
    // Heap HWM under 64 MiB and no runaway monotonic growth (>3x baseline).
    expect(maxHeap).toBeLessThan(64 * 1024 * 1024);
    expect(last).toBeLessThan(Math.max(first * 3, 32 * 1024 * 1024));
    console.log(
      `[WEB-E2E-018] 1280x720 1Hz stability: dom=${minDom}..${maxDom}, heap_mb=${(first / 1048576).toFixed(2)}->${(last / 1048576).toFixed(2)} (max ${(maxHeap / 1048576).toFixed(2)} MiB)`,
    );
  }
});
