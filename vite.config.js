import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// https://vitejs.dev/config/
export default defineConfig({
  plugins: [react()],
  server: {
    port: 3000,
    host: true,       // needed for containerized previews (0.0.0.0)
    open: false,      // no browser auto-open in CI/Docker
    allowedHosts: true, // accept tunneled preview hostnames
    proxy: {
      // The native backend (c_backend/nova_server) is proxied so the UI can
      // use a relative URL — no hardcoded localhost:8080, no CORS needed.
      '/api': {
        target: process.env.NOVA_BACKEND_URL || 'http://127.0.0.1:8080',
        changeOrigin: true
      }
    }
  }
})
