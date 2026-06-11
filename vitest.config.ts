import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    include: ['apps/dashboard/src/**/*.test.{ts,tsx}'],
    exclude: ['build/**', 'libs/third_party/**', 'node_modules/**'],
    passWithNoTests: true,
  },
});
