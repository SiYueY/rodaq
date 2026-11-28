import { test, expect } from '@playwright/test';
import { execFileSync, spawn, type ChildProcess } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

/**
 * WEB-E2E-014: 1280x720 sustained 1 Hz runtime update stability & JS heap non-growth verification
 * against the installed rodaq Server and share/rodaq/web bundle.
 */

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(HERE, '../../..');
const BUILD_DIR = path.join(REPO_ROOT, 'build/s1');
const WEB_DIST = path.resolve(HERE, '../../dist');
const BEARER_TOKEN = 'rodaq-perf-e2e-bearer-secret-0123456789abcd';

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

  const tokenFile = path.join(tmpRoot, 'server.token');
  fs.writeFileSync(tokenFile, `${BEARER_TOKEN}\n`);
  fs.chmodSync(tokenFile, 0o600);

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
        security: { bearer_token_file: tokenFile },
      },
      daq: {
        state_root: path.join(tmpRoot, 'state'),
        volume_root: path.join(tmpRoot, 'volume'),
        mode: 'initialize',
        plugin_search_paths: [pkgRoot],
        qualified_runtime_profile_id: 'web-perf-e2e',
        max_tasks: 2,
        max_pipelines_per_task: 2,
        volume: { name: 'local' },
        profiles: [
          {
            name: 'default',
            payload_pool_bytes: 262144,
            max_payload_bytes: 4096,
            max_logical_bytes: 262144,
            max_segment_physical_bytes: 4194304,
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
      `export LD_LIBRARY_PATH="${path.join(installPrefix, 'lib')}:$LD_LIBRARY_PATH" && exec "${path.join(installPrefix, 'bin/rodaq')}" --config "${depFile}"`,
    ],
    { stdio: 'ignore' },
  );
  await waitForServerReady(baseUrl, BEARER_TOKEN);
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

test('1280x720 sustained 1 Hz multi-stream runtime polling keeps bounded DOM and non-growing heap (WEB-E2E-014)', async ({
  page,
  browserName,
}) => {
  const streams = Array.from({ length: 16 }, (_, i) => `s${i}`);
  const createRes = await fetch(`${baseUrl}/api/tasks`, {
    method: 'POST',
    headers: {
      Authorization: `Bearer ${BEARER_TOKEN}`,
      'RoDAQ-API-Version': '1.0',
      'Content-Type': 'application/json',
      'Idempotency-Key': 'perf-create-1',
    },
    body: JSON.stringify({
      format_version: '1.0',
      name: 'perf-16-streams',
      segment_policy: { max_duration_ms: 60000, max_physical_bytes: null },
      retention_policy: { mode: 'keep_all', max_duration_ms: null, max_physical_bytes: null },
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
      Authorization: `Bearer ${BEARER_TOKEN}`,
      'RoDAQ-API-Version': '1.0',
      'Content-Type': 'application/json',
      'Idempotency-Key': 'perf-start-1',
    },
    body: JSON.stringify({ kind: 'start', parameters: {} }),
  });
  expect(startRes.status).toBe(202);

  await page.goto(`${baseUrl}/login`);
  await page.fill('input[type="password"]', BEARER_TOKEN);
  await page.click('button[type="submit"]');
  await expect(page).toHaveURL(`${baseUrl}/`);

  await page.click('nav a[href="/tasks"]');
  await page.click('a:has-text("perf-16-streams")');
  await page.click('button[role="tab"]:has-text("Streams")');
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

  const minDom = Math.min(...domCounts);
  const maxDom = Math.max(...domCounts);
  expect(maxDom - minDom).toBeLessThanOrEqual(20);
  expect(maxDom).toBeLessThan(1500);

  if (heapSamples.length >= 6) {
    const first = heapSamples[0];
    const last = heapSamples[heapSamples.length - 1];
    const maxHeap = Math.max(...heapSamples);
    expect(maxHeap).toBeLessThan(64 * 1024 * 1024);
    expect(last).toBeLessThan(Math.max(first * 3, 32 * 1024 * 1024));
  }
});
