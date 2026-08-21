import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'

const productVersion = readFileSync(resolve(__dirname, '../../VERSION'), 'utf8').trim()

export default defineConfig({
  define: {
    __INFERDECK_VERSION__: JSON.stringify(productVersion),
  },
  plugins: [react()],
  resolve: {
    extensions: ['.tsx', '.ts', '.js']
  },
  server: {
    allowedHosts: ['ai.homelab.com'],
    proxy: {
      '/api': 'http://127.0.0.1:11434',
      '/v1': 'http://127.0.0.1:11434',
    },
    port: 3000,
    strictPort: true,
    host: '0.0.0.0',
  },
  build: {
    outDir: '../inferdeck-gateway/static',
    emptyOutDir: true,
  },
})
