import { defineConfig } from '@playwright/test';

const browserName = (process.env.RODAQ_BROWSER ?? 'chromium') as 'chromium' | 'firefox' | 'webkit';

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
            executablePath: process.env.RODAQ_CHROME_BIN ?? '/usr/bin/google-chrome',
            args: ['--no-sandbox', '--disable-gpu'],
          }
        : {},
  },
});
