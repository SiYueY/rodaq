import fs from 'node:fs';
import { defineConfig } from '@playwright/test';

const browserName = (process.env.RODAQ_BROWSER ?? 'chromium') as 'chromium' | 'firefox' | 'webkit';
const defaultChrome = fs.existsSync('/usr/bin/google-chrome') ? '/usr/bin/google-chrome' : undefined;
const chromeExecutable = process.env.RODAQ_CHROME_BIN ?? defaultChrome;

export default defineConfig({
  testDir: './tests/e2e',
  timeout: 45_000,
  fullyParallel: false,
  workers: 1,
  reporter: [['list']],
  use: {
    browserName,
    viewport: { width: 1280, height: 720 },
    headless: true,
    launchOptions:
      browserName === 'chromium'
        ? {
            ...(chromeExecutable ? { executablePath: chromeExecutable } : {}),
            args: ['--no-sandbox', '--disable-gpu'],
          }
        : {},
  },
});
