import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import Icons from 'unplugin-icons/vite';

export default defineConfig({
  plugins: [
    react(),
    // File-type icons are compiled on demand from the local icon collection:
    // no runtime CDN, no auto-install, only used icons end up in the bundle.
    Icons({ compiler: 'jsx', jsx: 'react', autoInstall: false }),
  ],
  base: '/',
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
  server: {
    host: '127.0.0.1',
    port: 5173,
    strictPort: true,
    proxy: {
      '/api': { target: 'http://127.0.0.1:8080', changeOrigin: false },
      '/ws': { target: 'ws://127.0.0.1:8080', ws: true },
    },
  },
});
