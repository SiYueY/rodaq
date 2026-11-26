import { defineConfig, type Plugin } from 'vite';
import vue from '@vitejs/plugin-vue';

function resolveBuildId(): string {
  const env = (globalThis as { process?: { env?: Record<string, string | undefined> } }).process?.env;
  return env?.RODAQ_WEB_BUILD_ID ?? `rodaq-web-1.0.0-${Date.now()}`;
}

function rodaqWebManifestPlugin(): Plugin {
  return {
    name: 'rodaq-web-manifest',
    generateBundle() {
      const manifest = {
        web_build_id: resolveBuildId(),
        api_major: 1,
        api_minor_min: 0,
      };
      this.emitFile({
        type: 'asset',
        fileName: 'web_manifest.json',
        source: JSON.stringify(manifest, null, 2) + '\n',
      });
    },
  };
}

export default defineConfig({
  plugins: [vue(), rodaqWebManifestPlugin()],
  build: {
    target: ['chrome111', 'edge111', 'firefox114', 'safari16.4'],
    outDir: 'dist',
    emptyOutDir: true,
    assetsDir: 'assets',
    sourcemap: false,
  },
  server: {
    host: '127.0.0.1',
    port: 5173,
    proxy: {
      '/api': {
        target: 'http://127.0.0.1:8080',
        changeOrigin: false,
      },
    },
  },
});
