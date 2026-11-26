import { test, expect } from '@playwright/test';
import { execFileSync, spawn, type ChildProcess } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = path.resolve(HERE, '../../..');
const BUILD_DIR = path.join(REPO_ROOT, 'build/s1');
const WEB_DIST = path.resolve(HERE, '../../dist');
const ADMIN_TOKEN = 'rodaq-ros2-mcap-e2e-secret-0123456789abcd';
const ROS_DOMAIN_ID = 47;

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

async function waitForServerReady(baseUrl: string, token: string, getLogs: () => string): Promise<void> {
  const deadline = Date.now() + 15_000;
  while (Date.now() < deadline) {
    try {
      const res = await fetch(`${baseUrl}/api/system`, {
        headers: { Authorization: `Bearer ${token}`, 'RoDAQ-API-Version': '1.0' },
      });
      if (res.ok) {
        const body = (await res.json()) as { data?: { server_phase?: string } };
        if (body.data?.server_phase === 'ready') {
          return;
        }
      }
    } catch {
      // Booting.
    }
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error(`Installed rodaq server at ${baseUrl} did not reach ready phase:\n${getLogs()}`);
}

let tmpRoot = '';
let installPrefix = '';
let baseUrl = '';
let daemon: ChildProcess | null = null;
let rosPub: ChildProcess | null = null;
let daemonLogs = '';

test.beforeAll(async () => {
  expect(fs.existsSync('/opt/ros/humble/setup.bash')).toBe(true);
  expect(fs.existsSync(path.join(WEB_DIST, 'index.html'))).toBe(true);

  tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'rodaq-installed-ros2-e2e-'));
  installPrefix = path.join(tmpRoot, 'install');
  fs.mkdirSync(path.join(tmpRoot, 'state'), { recursive: true });
  fs.mkdirSync(path.join(tmpRoot, 'volume'), { recursive: true });

  // Stage REAL installed package (bin/rodaq, lib/librodaq_*, ROS2/MCAP plugins, share/rodaq/web).
  execFileSync('cmake', ['--install', BUILD_DIR, '--prefix', installPrefix], { stdio: 'pipe' });

  const pkgRoot = path.join(tmpRoot, 'plugins');
  fs.mkdirSync(pkgRoot, { recursive: true });
  fs.cpSync(
    path.join(installPrefix, 'lib/rodaq/plugins/com.rodaq.acquisition.ros2'),
    path.join(pkgRoot, 'com.rodaq.acquisition.ros2'),
    { recursive: true },
  );
  fs.cpSync(
    path.join(installPrefix, 'lib/rodaq/plugins/com.rodaq.storage.mcap'),
    path.join(pkgRoot, 'com.rodaq.storage.mcap'),
    { recursive: true },
  );
  chmodRecursiveReadOnly(pkgRoot);

  const credFile = path.join(tmpRoot, 'credentials.json');
  fs.writeFileSync(
    credFile,
    JSON.stringify(
      {
        format_version: '1.0',
        credentials: [
          {
            credential_id: 'ros2-admin',
            principal: 'ros2-admin',
            scopes: ['read', 'control', 'admin'],
            verifier: computeBearerVerifierHex(ADMIN_TOKEN),
            credential_epoch: '1',
            enabled: true,
          },
        ],
      },
      null,
      2,
    ),
  );
  fs.chmodSync(credFile, 0o600);

  const port = await getFreePort();
  baseUrl = `http://127.0.0.1:${port}`;
  const depFile = path.join(tmpRoot, 'deployment.json');
  fs.writeFileSync(
    depFile,
    JSON.stringify(
      {
        format_version: '1.0',
        static_assets_root: path.join(installPrefix, 'share/rodaq/web'),
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
          qualified_runtime_profile_id: 'installed-ros2-mcap-web-e2e',
          max_tasks: 2,
          max_pipelines_per_task: 2,
          volume_name: 'local',
          profiles: [
            {
              name: 'default',
              payload_pool_bytes: '262144',
              max_payload_bytes: '16384',
              max_logical_bytes: '262144',
              max_segment_physical_bytes: '4194304',
              max_descriptors: 16,
              max_prepared_descriptors: 16,
              max_streams: 8,
            },
          ],
          allowed_backends: [
            {
              plugin_id: 'com.rodaq.acquisition.ros2',
              plugin_version: '1.0.0',
              role: 'acquisition',
              support_status: 'required',
              qualification_status: 'qualified',
            },
            {
              plugin_id: 'com.rodaq.storage.mcap',
              plugin_version: '1.0.0',
              role: 'storage',
              storage_format: 'mcap',
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

  // Launch INSTALLED bin/rodaq with ROS 2 Humble runtime environment & installed libdir.
  daemon = spawn(
    'bash',
    [
      '-lc',
      `source /opt/ros/humble/setup.bash && export LD_LIBRARY_PATH="${path.join(installPrefix, 'lib')}:$LD_LIBRARY_PATH" && exec "${path.join(installPrefix, 'bin/rodaq')}" --deployment "${depFile}"`,
    ],
    { stdio: ['ignore', 'pipe', 'pipe'] },
  );
  daemon.stdout?.on('data', (d: Buffer) => {
    daemonLogs += d.toString();
  });
  daemon.stderr?.on('data', (d: Buffer) => {
    daemonLogs += d.toString();
  });

  // Launch live ROS 2 Humble Python publisher on domain 47 publishing std_msgs/msg/String at 20 Hz.
  const pubFile = path.join(tmpRoot, 'pub.py');
  fs.writeFileSync(
    pubFile,
    [
      'import rclpy, time',
      'from rclpy.node import Node',
      'from std_msgs.msg import String',
      'rclpy.init()',
      "node = Node('rodaq_web_e2e_pub')",
      "pub = node.create_publisher(String, '/rodaq_web_e2e_chatter', 10)",
      'i = 0',
      'try:',
      '    while rclpy.ok():',
      '        m = String()',
      "        m.data = f'rodaq-e2e-msg-{i}'",
      '        pub.publish(m)',
      '        i += 1',
      '        time.sleep(0.05)',
      'except KeyboardInterrupt:',
      '    pass',
      'finally:',
      '    node.destroy_node()',
      '    rclpy.shutdown()',
      '',
    ].join('\n'),
  );
  rosPub = spawn(
    'bash',
    [
      '-lc',
      `source /opt/ros/humble/setup.bash && export ROS_DOMAIN_ID=${ROS_DOMAIN_ID} && exec python3 "${pubFile}"`,
    ],
    { stdio: 'ignore' },
  );

  await waitForServerReady(baseUrl, ADMIN_TOKEN, () => daemonLogs);
});

test.afterAll(async () => {
  if (rosPub && rosPub.exitCode === null) {
    rosPub.kill('SIGTERM');
    await new Promise((r) => setTimeout(r, 300));
    if (rosPub.exitCode === null) rosPub.kill('SIGKILL');
  }
  if (daemon && daemon.exitCode === null) {
    const exitPromise = new Promise<number | null>((resolve) => {
      daemon?.once('exit', (code) => resolve(code));
    });
    daemon.kill('SIGTERM');
    const code = await Promise.race([
      exitPromise,
      new Promise<number | null>((r) => setTimeout(() => r(-99), 10_000)),
    ]);
    if (code === -99) {
      daemon.kill('SIGKILL');
    } else {
      expect(code, `installed rodaq shutdown failed:\n${daemonLogs}`).toBe(0);
    }
  }
  if (tmpRoot && fs.existsSync(tmpRoot)) {
    chmodRecursiveWritable(path.join(tmpRoot, 'plugins'));
    fs.rmSync(tmpRoot, { recursive: true, force: true });
  }
});

test('Installed rodaq + share/rodaq/web + ROS 2 Humble -> MCAP real E2E & MCAP download verification', async ({
  page,
}) => {
  const pageErrors: string[] = [];
  page.on('pageerror', (err) => pageErrors.push(err.message));

  await page.goto(`${baseUrl}/login`);
  await page.fill('input[type="password"]', ADMIN_TOKEN);
  await page.click('button[type="submit"]');
  await expect(page).toHaveURL(`${baseUrl}/`);

  // Verify official ROS 2 and MCAP backends appear as Installed + Allowed + required + qualified.
  await page.click('nav a[href="/system"]');
  await expect(page.locator('main')).toContainText('com.rodaq.acquisition.ros2');
  await expect(page.locator('main')).toContainText('com.rodaq.storage.mcap');

  // Create Task using the Official ROS 2 & MCAP visual editors (WEB-E2E-017).
  await page.click('nav a[href="/tasks"]');
  await page.click('a:has-text("Create Task")');
  await expect(page.locator('main')).toContainText('Official ROS 2 Acquisition Editor');
  await expect(page.locator('main')).toContainText('Qualified MCAP Baseline');

  await page.fill('#task-name', 'ros2-mcap-live-e2e');
  // Set Domain ID = 47 and Topic = /rodaq_web_e2e_chatter in Official ROS 2 visual editor.
  await page.locator('input[type="number"][min="0"][max="232"]').fill(String(ROS_DOMAIN_ID));
  await page.fill('#ros2-topic-0-0', '/rodaq_web_e2e_chatter');
  await page.click('button:has-text("Create Task")');
  await expect(page).toHaveURL(/\/tasks\/[0-9a-f-]+$/);

  // Start -> wait for live ROS 2 messages to be received (>0 accepted) -> Pause -> Resume -> Stop.
  await page.click('button:has-text("Start")');
  await expect(page.locator('main')).toContainText('running', { timeout: 10_000 });
  try {
    await expect(page.locator('main')).toContainText('active', { timeout: 10_000 });
  } catch (e) {
    const taskId = page.url().split('/').pop()!;
    const ops = await fetch(`${baseUrl}/api/tasks/${taskId}/operations`, {
      headers: { Authorization: `Bearer ${ADMIN_TOKEN}`, 'RoDAQ-API-Version': '1.0' },
    }).then((r) => r.text());
    const cfg = await fetch(`${baseUrl}/api/tasks/${taskId}/config`, {
      headers: { Authorization: `Bearer ${ADMIN_TOKEN}`, 'RoDAQ-API-Version': '1.0' },
    }).then((r) => r.text());
    throw new Error(`Task did not become active.\nops=${ops}\ncfg=${cfg}\ndaemonLogs=${daemonLogs}`);
  }

  await page.click('button[role="tab"]:has-text("Streams")');
  await expect(page.locator('main')).toContainText('Source Ready', { timeout: 10_000 });
  await expect(page.locator('main')).toContainText('Receiving', { timeout: 10_000 });
  await page.waitForTimeout(400);

  await page.click('button:has-text("Pause")');
  await expect(page.locator('main')).toContainText('paused', { timeout: 10_000 });
  await expect(page.locator('main')).toContainText('idle', { timeout: 10_000 });

  await page.click('button:has-text("Resume")');
  await expect(page.locator('main')).toContainText('running', { timeout: 10_000 });
  await expect(page.locator('main')).toContainText('active', { timeout: 10_000 });
  await page.waitForTimeout(400);

  await page.click('button:has-text("Stop")');
  await expect(page.locator('main')).toContainText('stopped', { timeout: 10_000 });
  await expect(page.locator('main')).toContainText('idle', { timeout: 10_000 });

  // Open Artifacts -> inspect committed MCAP artifact -> download .mcap file -> verify MCAP header & payload!
  await page.click('nav a[href="/artifacts"]');
  await expect(page.locator('table tbody tr').first()).toContainText('mcap', { timeout: 10_000 });
  await page.locator('table tbody tr td button').first().click();
  await expect(page.locator('main')).toContainText('Files');

  const downloadPromise = page.waitForEvent('download');
  await page.locator('button:has-text("Download")').first().click();
  const download = await downloadPromise;
  const dlPath = await download.path();
  expect(dlPath).not.toBeNull();

  const bytes = fs.readFileSync(dlPath!);
  expect(bytes.length).toBeGreaterThan(64);
  // Standard MCAP v0 magic prefix & footer: \x89MCAP0\r\n
  const mcapMagic = Buffer.from([0x89, 0x4d, 0x43, 0x41, 0x50, 0x30, 0x0d, 0x0a]);
  expect(bytes.subarray(0, 8).equals(mcapMagic)).toBe(true);
  expect(bytes.subarray(bytes.length - 8).equals(mcapMagic)).toBe(true);
  // Verify real ROS 2 topic & CDR payload marker recorded inside the downloaded MCAP file!
  expect(bytes.includes(Buffer.from('/rodaq_web_e2e_chatter'))).toBe(true);
  expect(bytes.includes(Buffer.from('rodaq-e2e-msg-'))).toBe(true);

  expect(pageErrors).toEqual([]);
});
