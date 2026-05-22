import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    include: [
      'apps/dashboard/src/**/*.test.{ts,tsx}',
      'apps/gateway-service/src/**/*.test.{ts,tsx}',
      'packages/**/*.test.{ts,tsx}',
    ],
    exclude: [
      'build/**',
      'libs/third_party/**',
      'node_modules/**',
      'dist/**',
    ],
    passWithNoTests: true,
  },
});
