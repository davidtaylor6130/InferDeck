import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import path from 'path'

export default defineConfig({
  plugins: [react()],
  resolve: {
    extensions: ['.tsx', '.ts', '.js']
  },
  server: {
    allowedHosts: ['ai.homelab.com'],
    proxy: {
      '/api': 'http://127.0.0.1:11434',
      '/v1': 'http://127.0.0.1:11434',
      '/health': 'http://127.0.0.1:11434',
      '/status': 'http://127.0.0.1:11434',
      '/queue': 'http://127.0.0.1:11434',
      '/jobs': 'http://127.0.0.1:11434',
      '/models': 'http://127.0.0.1:11434',
      '/services': 'http://127.0.0.1:11434',
      '/modes': 'http://127.0.0.1:11434',
      '/metrics': 'http://127.0.0.1:11434',
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
