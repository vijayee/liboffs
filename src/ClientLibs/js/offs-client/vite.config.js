import { defineConfig } from 'vite';

export default defineConfig({
  build: {
    lib: {
      entry: 'src/index.js',
      name: 'OffsClient',
      fileName: (format) => `offs-client.${format}.js`,
      formats: ['esm', 'umd']
    },
    rollupOptions: {
      external: [],
      output: {
        globals: {}
      }
    },
    sourcemap: true
  },
  test: {
    environment: 'node'
  }
});
