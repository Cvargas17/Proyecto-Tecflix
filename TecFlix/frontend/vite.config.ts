import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// `npm run dev` proxies the C++ server (tecflix.exe on :8080); `npm run build` writes ../web/dist.
const backend = 'http://localhost:8080';

export default defineConfig({
  plugins: [react()],
  build: { outDir: '../web/dist', emptyOutDir: true },
  server: {
    port: 5173,
    proxy: { '/api': backend, '/video': backend, '/dash': backend },
  },
});
