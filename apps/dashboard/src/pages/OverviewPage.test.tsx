import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';
import { OverviewPage } from './OverviewPage';
import type { DashboardActions, DashboardState } from '../types';

describe('OverviewPage', () => {
  it('renders the icon-led dashboard with hot telemetry and compact controls', () => {
    const html = renderToStaticMarkup(<OverviewPage state={state} actions={actions} />);

    expect(html).toContain('InferDeck Overview');
    expect(html).toContain('API healthy');
    expect(html).toContain('llama.cpp ready');
    expect(html).toContain('18 queued');
    expect(html).toContain('96%');
    expect(html).toContain('84%');
    expect(html).toContain('97%');
    expect(html).toContain('14.7 GB / 16.0 GB');
    expect(html).toContain('88 C');
    expect(html).toContain('384.2K');
    expect(html).toContain('aria-label="Start llama.cpp"');
    expect(html).toContain('aria-label="Stop llama.cpp"');
    expect(html).toContain('aria-label="Restart llama.cpp"');
    expect(html).toContain('aria-label="Rescan models"');
    expect(html).toContain('aria-label="View logs"');
    expect(html).toContain('text-danger-rose');
    expect(html).toContain('text-warning-amber');
    expect(html).toContain('text-success-green');
  });
});

const state: DashboardState = {
  healthData: { status: 'healthy', version: '1.0.0', uptime: 8421 },
  statusData: {
    hardware: {
      timestamp: new Date().toISOString(),
      system: {
        cpuPercent: 96,
        memoryUsed: 27 * 1024 ** 3,
        memoryTotal: 32 * 1024 ** 3,
        memoryPercent: 84,
      },
      gpu: {
        name: 'AMD Radeon RX 9700 XT',
        backend: 'Vulkan',
        utilization: 97,
        memoryUsed: 15100 * 1024 ** 2,
        memoryTotal: 16384 * 1024 ** 2,
        memoryPercent: 92,
        temperature: 88,
      },
    },
    storage: {
      freeSpace: 190 * 1024 ** 3,
      totalSpace: 1000 * 1024 ** 3,
      usedPercent: 81,
    },
    queue: {
      queued: 18,
      running: 1,
      failed: 2,
      gpuLocked: true,
    },
    summary: {
      jobsToday: 27,
      succeededToday: 24,
      failedToday: 2,
      totalTokens: 384220,
      promptTokens: 144800,
      completionTokens: 239420,
      warningCount: 2,
    },
    metrics: {},
  },
  jobsList: [{
    id: 'chatcmpl-live',
    type: 'chat.completion',
    status: 'running',
    priority: 50,
    model: 'Qwen3-32B-Q4_K_M.gguf',
  }],
  modelsList: [{ name: 'Qwen3-32B-Q4_K_M.gguf', loaded: true }],
  runningModels: [{ name: 'Qwen3-32B-Q4_K_M.gguf', loaded: true }],
  servicesList: [
    { id: 'gateway', name: 'Gateway', kind: 'gateway', status: 'running', baseUrl: 'http://127.0.0.1:11434' },
    { id: 'llama-server', name: 'llama.cpp', kind: 'llama_cpp', status: 'running', baseUrl: 'http://127.0.0.1:18080' },
  ],
  errors: {},
  loading: false,
  connected: true,
  lastUpdatedAt: new Date(),
};

const actions: DashboardActions = {
  refreshAll: vi.fn(),
  changeMode: vi.fn(),
  pullModel: vi.fn(),
  loadModel: vi.fn(),
  unloadModel: vi.fn(),
  rescanModels: vi.fn(),
  deleteModel: vi.fn(),
  cancelJob: vi.fn(),
  retryJob: vi.fn(),
  reprioritizeJob: vi.fn(),
  restartBackend: vi.fn(),
  startService: vi.fn(),
  stopService: vi.fn(),
  restartService: vi.fn(),
  pauseQueue: vi.fn(),
  resumeQueue: vi.fn(),
  clearFailedJobs: vi.fn(),
  unloadModels: vi.fn(),
  toast: vi.fn(),
};
