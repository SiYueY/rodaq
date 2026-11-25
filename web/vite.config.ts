import { defineConfig, type Plugin } from 'vite'
import vue from '@vitejs/plugin-vue'
import { writeFileSync } from 'node:fs'
import { resolve } from 'node:path'

function rodaqWebManifestPlugin(): Plugin {
  return {
    name: 'rodaq-web-manifest',
    closeBundle() {
      const manifest = {
        web_build_id: `rodaq-web-${new Date().toISOString().replace(/[:.]/g, '-')}`,
        api_major: 1,
        api_minor_min: 0
      }
      writeFileSync(
        resolve(__dirname, 'dist', 'web_manifest.json'),
        JSON.stringify(manifest, null, 2) + '\n',
        'utf-8'
      )
    }
  }
}

export default defineConfig({
  plugins: [vue(), rodaqWebManifestPlugin()],
  build: {
    target: ['chrome111', 'edge111', 'firefox114', 'safari16.4'],
    outDir: 'dist',
    emptyOutDir: true
  },
  server: {
    host: '127.0.0.1',
    port: 5173,
    fs: {
      allow: ['..']
    },
    proxy: {
      '/api': 'http://127.0.0.1:8080'
    }
  },
  test: {
    environment: 'happy-dom',
    globals: true
  }
})
