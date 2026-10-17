import { defineConfig } from 'vitest/config';
import Icons from 'unplugin-icons/vite';

export default defineConfig({
  // Tests import the same `~icons/*` components as the app; the plugin must
  // be registered here too (this config does not extend vite.config.ts).
  plugins: [Icons({ compiler: 'jsx', jsx: 'react', autoInstall: false })],
  test: {
    environment: 'node',
    include: ['src/**/*.test.ts', 'src/**/*.test.tsx'],
    setupFiles: ['src/test/setup.ts'],
  },
});
