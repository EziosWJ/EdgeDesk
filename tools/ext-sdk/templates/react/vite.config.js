import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// Relative base is required: extension assets are served from
// /apps/{appId}/{version}/{revisionId}/ and must not use absolute /assets URLs.
export default defineConfig({
  base: './',
  plugins: [react()],
  build: { outDir: 'dist', emptyOutDir: true },
});
