import { test, expect } from '@playwright/test';
import { spawn, type ChildProcess } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(HERE, '../../..');
const WEB_DIST = path.resolve(HERE, '../../dist');
const SERVER_BIN = process.env.RODAQ_SERVER_BIN ?? path.join(REPO_ROOT, 'build/s1/server/rodaq');
const FAKE_STO_PKG = path.join(REPO_ROOT, 'build/s1/plugins/fake_storage/rodaq-plugins/rodaq.fake.storage');
const FAKE_ACQ_PKG = path.join(REPO_ROOT, 'build/s1/plugins/fake_acquisition/rodaq-plugins/rodaq.fake.acquisition');

const BEARER_TOKEN = 'rodaq-e2e-local-bearer-secret-0123456789abcdef';

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
        headers: {
          Authorization: `Bearer ${token}`,
          'RoDAQ-API-Version': '1.0',
        },
      });
      if (res.ok) {
        const body = (await res.json()) as { data?: { server_phase?: string } };
        if (body.data?.server_phase === 'ready') {
          return;
        }
      }
    } catch {
      // Server still booting.
    }
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error(
    `rodaq server at ${baseUrl} did not reach ready phase in time. exit=${daemon?.exitCode}\nlogs:\n${daemonLogs}`,
  );
}

let tmpRoot = '';
let baseUrl = '';
let daemon: ChildProcess | null = null;
let daemonLogs = '';

test.beforeAll(async () => {
  expect(fs.existsSync(SERVER_BIN), `Missing server binary at ${SERVER_BIN}`).toBe(true);
  expect(fs.existsSync(path.join(WEB_DIST, 'index.html')), `Missing web dist at ${WEB_DIST}`).toBe(true);
  expect(fs.existsSync(path.join(WEB_DIST, 'web_manifest.json'))).toBe(true);

  tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'rodaq-web-e2e-'));
  const pkgRoot = path.join(tmpRoot, 'packages');
  fs.mkdirSync(pkgRoot, { recursive: true });
  fs.mkdirSync(path.join(tmpRoot, 'state'), { recursive: true });
  fs.mkdirSync(path.join(tmpRoot, 'volume'), { recursive: true });
  fs.cpSync(FAKE_STO_PKG, path.join(pkgRoot, 'rodaq.fake.storage'), { recursive: true });
  fs.cpSync(FAKE_ACQ_PKG, path.join(pkgRoot, 'rodaq.fake.acquisition'), { recursive: true });
  chmodRecursiveReadOnly(pkgRoot);

  const tokenFile = path.join(tmpRoot, 'server.token');
  fs.writeFileSync(tokenFile, `${BEARER_TOKEN}\n`);
  fs.chmodSync(tokenFile, 0o600);

  const port = await getFreePort();
  baseUrl = `http://127.0.0.1:${port}`;
  const depFile = path.join(tmpRoot, 'deployment.json');
  fs.writeFileSync(
    depFile,
    JSON.stringify(
      {
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
          qualified_runtime_profile_id: 'web-e2e',
          max_tasks: 4,
          max_pipelines_per_task: 2,
          volume: { name: 'local' },
          profiles: [
            {
              name: 'default',
              payload_pool_bytes: 65536,
              max_payload_bytes: 4096,
              max_logical_bytes: 65536,
              max_segment_physical_bytes: 1048576,
              max_descriptors: 16,
              max_prepared_descriptors: 16,
              max_streams: 8,
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
      },
      null,
      2,
    ),
  );

  daemon = spawn(SERVER_BIN, ['--config', depFile], {
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  daemon.stdout?.on('data', (d: Buffer) => {
    daemonLogs += d.toString();
  });
  daemon.stderr?.on('data', (d: Buffer) => {
    daemonLogs += d.toString();
  });

  await waitForServerReady(baseUrl, BEARER_TOKEN);
});

test.afterAll(async () => {
  if (daemon && daemon.exitCode === null) {
    const exitPromise = new Promise<number | null>((resolve) => {
      daemon?.once('exit', (code) => resolve(code));
    });
    daemon.kill('SIGTERM');
    const code = await Promise.race([
      exitPromise,
      new Promise<number | null>((r) => setTimeout(() => r(-99), 8_000)),
    ]);
    if (code === -99) {
      daemon.kill('SIGKILL');
    } else {
      expect(code, `rodaq server clean shutdown failed:\n${daemonLogs}`).toBe(0);
    }
  }
  if (tmpRoot && fs.existsSync(tmpRoot)) {
    chmodRecursiveWritable(path.join(tmpRoot, 'packages'));
    fs.rmSync(tmpRoot, { recursive: true, force: true });
  }
});

test('RoDAQ Web Console end-to-end operator workflow with memory-only Bearer (WEB-E2E-001..011)', async ({ page }) => {
  const pageErrors: string[] = [];
  const consoleErrors: string[] = [];
  page.on('pageerror', (err) => {
    pageErrors.push(err.message);
  });
  page.on('console', (msg) => {
    if (msg.type() === 'error') {
      const text = msg.text();
      if (
        !text.includes('status of 400') &&
        !text.includes('status of 401') &&
        !text.includes('status of 404')
      ) {
        consoleErrors.push(text);
      }
    }
  });

  // 1. Deep-link redirect when unauthenticated -> /login?next=%2Ftasks
  await page.goto(`${baseUrl}/tasks`);
  await expect(page).toHaveURL(/\/login\?next=(?:%2F|\/)tasks/);

  // 2. Wrong Bearer token fails closed with 401
  await page.fill('input[type="password"]', 'wrong-bearer-token-0123456789abcdef012345');
  await page.click('button[type="submit"]');
  await expect(page.locator('[role="alert"]')).toContainText('401');

  // 3. Valid Bearer token -> redirects back to /tasks; ZERO cookie/localStorage/sessionStorage leakage
  await page.fill('input[type="password"]', BEARER_TOKEN);
  await page.click('button[type="submit"]');
  await expect(page).toHaveURL(`${baseUrl}/tasks`);

  const storageSnapshot = await page.evaluate(() => ({
    local: JSON.stringify(window.localStorage),
    session: JSON.stringify(window.sessionStorage),
    cookies: document.cookie,
  }));
  expect(storageSnapshot.local).not.toContain(BEARER_TOKEN);
  expect(storageSnapshot.session).not.toContain(BEARER_TOKEN);
  expect(storageSnapshot.cookies).toBe('');

  // 4. Reload (F5) clears memory-only Bearer and returns to /login (WEB-E2E-001)
  await page.reload();
  await expect(page).toHaveURL(/\/login/);
  await page.fill('input[type="password"]', BEARER_TOKEN);
  await page.click('button[type="submit"]');
  await expect(page).toHaveURL(`${baseUrl}/tasks`);

  // 5. Overview zero-task healthy state & System Capabilities matrix
  await page.click('nav a[href="/"]');
  await expect(page).toHaveURL(`${baseUrl}/`);
  await expect(page.locator('main')).toContainText('ready');
  await expect(page.locator('[data-testid="zero-task-state"]')).toContainText('No tasks are configured yet');

  await page.click('nav a[href="/system"]');
  await expect(page).toHaveURL(`${baseUrl}/system`);
  await expect(page.locator('main')).toContainText('rodaq.fake.acquisition');
  await expect(page.locator('main')).toContainText('rodaq.fake.storage');
  await expect(page.locator('main')).toContainText('qualified');

  // 6. Create Task via simplified Portable TaskConfig UI editor -> navigates to Task Detail
  await page.click('nav a[href="/tasks"]');
  await page.click('a:has-text("Create Task")');
  await expect(page).toHaveURL(`${baseUrl}/tasks/new`);
  await page.fill('#task-name', 'e2e-demo-task');
  await page.click('button:has-text("Create Task")');
  await expect(page).toHaveURL(/\/tasks\/[0-9a-f-]+$/);
  await expect(page.locator('main')).toContainText('e2e-demo-task');
  await expect(page.locator('main')).toContainText('stopped');
  await expect(page.locator('main')).toContainText('idle');

  // 7. Start -> Active -> Pause -> Idle -> Resume -> Active -> Stop -> Stopped/Idle
  await page.click('button:has-text("Start")');
  await expect(page.locator('main')).toContainText('running', { timeout: 10_000 });
  await expect(page.locator('main')).toContainText('active', { timeout: 10_000 });

  await page.click('button[role="tab"]:has-text("Streams")');
  await expect(page.locator('main')).toContainText('s0', { timeout: 10_000 });

  await page.waitForTimeout(200);
  await page.click('button:has-text("Pause")');
  await expect(page.locator('main')).toContainText('paused', { timeout: 10_000 });
  await expect(page.locator('main')).toContainText('idle', { timeout: 10_000 });

  await page.click('button:has-text("Resume")');
  await expect(page.locator('main')).toContainText('running', { timeout: 10_000 });
  await expect(page.locator('main')).toContainText('active', { timeout: 10_000 });

  await page.waitForTimeout(200);
  await page.click('button:has-text("Stop")');
  await expect(page.locator('main')).toContainText('stopped', { timeout: 10_000 });
  await expect(page.locator('main')).toContainText('idle', { timeout: 10_000 });

  // 8. Operations history & 404 lookup "Not found or expired"
  await page.click('button[role="tab"]:has-text("Operations")');
  await expect(page.locator('main')).toContainText('start');
  await expect(page.locator('main')).toContainText('pause');
  await expect(page.locator('main')).toContainText('resume');
  await expect(page.locator('main')).toContainText('stop');
  await page.fill('#op-lookup-input', '00000000-0000-4000-8000-000000000000');
  await page.click('button:has-text("Lookup")');
  await expect(page.locator('main')).toContainText('Not found or expired');

  // 9. Artifacts page -> Detail -> Bearer authenticated download
  await page.click('nav a[href="/artifacts"]');
  await expect(page).toHaveURL(`${baseUrl}/artifacts`);
  await expect(page.locator('table tbody tr').first()).toContainText('fake', { timeout: 10_000 });
  await page.locator('table tbody tr td button').first().click();
  await expect(page.locator('main')).toContainText('Committed Files');

  const downloadPromise = page.waitForEvent('download');
  await page.click('button:has-text("Download")');
  const download = await downloadPromise;
  const dlPath = await download.path();
  expect(dlPath).not.toBeNull();
  const stat = fs.statSync(dlPath!);
  expect(stat.size).toBeGreaterThan(0);

  // 10. Configure Task -> Identity Diff -> Save
  await page.click('nav a[href="/tasks"]');
  await page.click('a:has-text("e2e-demo-task")');
  await page.click('a:has-text("Configure")');
  await expect(page).toHaveURL(/\/config$/);
  await expect(page.locator('main')).toContainText('Preserved pipelines');
  await page.fill('#task-name', 'e2e-demo-task-v2');
  await page.click('button:has-text("Submit Configure")');
  await expect(page).toHaveURL(/\/tasks\/[0-9a-f-]+$/);
  await expect(page.locator('main')).toContainText('e2e-demo-task-v2', { timeout: 10_000 });

  // 11. Reset Task ConfirmDialog: Escape cancels, Confirm executes
  await page.click('button:has-text("Reset Task")');
  await expect(page.locator('[role="dialog"]')).toContainText('Confirm Reset Task');
  await page.keyboard.press('Escape');
  await expect(page.locator('[role="dialog"]')).toHaveCount(0);

  await page.click('button:has-text("Reset Task")');
  await page.locator('[role="dialog"] button:has-text("Reset Task")').click();
  await expect(page.locator('main')).toContainText('reset', { timeout: 10_000 });

  // 12. Sign Out clears memory Bearer -> returns to /login
  await page.click('button:has-text("Sign Out")');
  await expect(page).toHaveURL(/\/login/);
  await page.goto(`${baseUrl}/tasks`);
  await expect(page).toHaveURL(/\/login\?next=(?:%2F|\/)tasks/);

  expect(pageErrors, `Unexpected uncaught page errors: ${pageErrors.join('; ')}`).toEqual([]);
  expect(consoleErrors, `Unexpected console errors: ${consoleErrors.join('; ')}`).toEqual([]);
});
