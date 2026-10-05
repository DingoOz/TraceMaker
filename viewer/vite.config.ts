// SPDX-License-Identifier: GPL-3.0-or-later
import { defineConfig } from 'vite';

// `npm run dev` proxies the event stream to a running tracemaker-view / tracemaker server.
const engine = process.env.TM_ENGINE ?? 'http://127.0.0.1:8766';

export default defineConfig({
  base: './',
  server: {
    host: true,
    proxy: { '/ws': { target: engine.replace(/^http/, 'ws'), ws: true } },
  },
  build: { target: 'es2022', outDir: 'dist', emptyOutDir: true, sourcemap: true },
});
