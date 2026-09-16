import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { parseDocument } from 'yaml';
import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  persistVramReserve,
  readVramReserve,
  updateVramReserve,
  VramReserveSettings,
} from './VramReserveSettings';

const configDocument = (overrides: Partial<{
  activeYaml: string;
  activeRevision: string;
  runningRevision: string;
}> = {}) => ({
  yaml: 'gateway:\n  vram_safety_margin_mb: 1024\n',
  revision: 'base-revision',
  activeYaml: 'gateway:\n  vram_safety_margin_mb: 1024\nauth:\n  token: __INFERDECK_SECRET__\n',
  activeRevision: 'active-revision',
  runningRevision: 'active-revision',
  hasActiveProfile: true,
  usingActiveProfile: true,
  restartRequired: false,
  ...overrides,
});

const jsonResponse = (body: unknown, status = 200) => new Response(
  JSON.stringify(body),
  { status, headers: { 'Content-Type': 'application/json' } },
);

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('VRAM reserve settings', () => {
  it('shows a zero-capable MiB field with the documented default', () => {
    const html = renderToStaticMarkup(<VramReserveSettings />);
    expect(html).toContain('VRAM reserve');
    expect(html).toContain('Safety margin (MiB)');
    expect(html).toContain('min="0"');
    expect(html).toContain('value="1024"');
    expect(html).toContain('Set 0 for no additional reserve');
    expect(html).toContain('configuration reload');
  });

  it('updates only the reserve and preserves masked secrets and unrelated YAML', () => {
    const yaml = [
      'auth:',
      '  token: __INFERDECK_SECRET__',
      'gateway:',
      '  port: 11434',
      'model_registry:',
      '  - name: test-model',
      '    context_size: 4096',
      '',
    ].join('\n');
    const updated = updateVramReserve(yaml, 0);
    const parsed = parseDocument(updated).toJS() as {
      auth: { token: string };
      gateway: { port: number; vram_safety_margin_mb: number };
      model_registry: Array<{ name: string; context_size: number }>;
    };
    expect(readVramReserve('gateway:\n  port: 11434\n')).toBe(1024);
    expect(readVramReserve(updated)).toBe(0);
    expect(parsed.auth.token).toBe('__INFERDECK_SECRET__');
    expect(parsed.gateway).toEqual({ port: 11434, vram_safety_margin_mb: 0 });
    expect(parsed.model_registry).toEqual([
      { name: 'test-model', context_size: 4096 },
    ]);
  });

  it('saves zero and waits through the reload window for the active revision', async () => {
    const applied = configDocument({
      activeYaml: 'gateway:\n  vram_safety_margin_mb: 0\n',
      activeRevision: 'saved-revision',
      runningRevision: 'saved-revision',
    });
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(jsonResponse({
        ok: true,
        activeRevision: 'saved-revision',
        hasActiveProfile: true,
        restartRequired: false,
        applyScheduled: true,
      }))
      .mockRejectedValueOnce(new TypeError('gateway restarting'))
      .mockResolvedValueOnce(jsonResponse(applied));
    vi.stubGlobal('fetch', fetchMock);
    const onSaved = vi.fn();

    const result = await persistVramReserve(
      configDocument(), 0, 1_000, 1, onSaved,
    );

    expect(result).toEqual({ config: applied, conflict: false });
    expect(onSaved).toHaveBeenCalledOnce();
    expect(fetchMock).toHaveBeenCalledTimes(3);
    const request = fetchMock.mock.calls[0] as unknown as [string, RequestInit];
    const body = JSON.parse(String(request[1].body)) as {
      yaml: string;
      revision: string;
    };
    expect(request[0]).toBe('/api/inferdeck/v1/config/active');
    expect(request[1].method).toBe('PUT');
    expect(body.revision).toBe('active-revision');
    expect(readVramReserve(body.yaml)).toBe(0);
    expect(body.yaml).toContain('__INFERDECK_SECRET__');
  });

  it('reloads the latest document after the exact active-revision conflict', async () => {
    const latest = configDocument({
      activeYaml: 'gateway:\n  vram_safety_margin_mb: 2048\n',
      activeRevision: 'newer-revision',
      runningRevision: 'newer-revision',
    });
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(jsonResponse({
        error: { message: 'active configuration revision conflict' },
      }, 409))
      .mockResolvedValueOnce(jsonResponse(latest));
    vi.stubGlobal('fetch', fetchMock);

    const result = await persistVramReserve(configDocument(), 0, 1_000, 1);

    expect(result).toEqual({ config: latest, conflict: true });
    expect(fetchMock).toHaveBeenCalledTimes(2);
  });
});
