import { defineConfig } from 'vite';

export default defineConfig({
  // Relative base so the built dist/ works from any sub-folder of a personal
  // site (e.g. example.com/games/imu-racer/) without rewriting asset URLs.
  base: './',
  server: { port: 5180 },
  build: {
    outDir: 'dist',
    assetsInlineLimit: 0,
  },
});
