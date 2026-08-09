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
        globals: {},
        footer: 'var _oc = globalThis.OffsClient; if (_oc && _oc.OffsClient) { globalThis.OffsClient = _oc.OffsClient; }'
      }
    },
    sourcemap: true
  },
  test: {
    environment: 'node'
  }
});
