import { describe, expect, it } from 'vitest';
import { modelHref, navIdForRoute, parseRoute, routeHref, storeHref } from './routes';

describe('routes', () => {
  it('parses task areas with an AI type switch', () => {
    expect(parseRoute('#models/dictation')).toEqual({ page: 'models', section: 'dictation' });
    expect(parseRoute('#store/image')).toEqual({ page: 'store', section: 'image' });
    expect(parseRoute('#usage/nonsense')).toEqual({ page: 'usage', section: 'llm' });
    expect(parseRoute('#generate/music')).toEqual({ page: 'generate', kind: 'music' });
    expect(parseRoute('')).toEqual({ page: 'home' });
  });

  it('keeps old bookmarks working', () => {
    expect(parseRoute('#llm/settings')).toEqual({ page: 'models', section: 'llm' });
    expect(parseRoute('#music/models')).toEqual({ page: 'store', section: 'music' });
    expect(parseRoute('#dictation/diagnostics')).toEqual({ page: 'health', section: 'dictation' });
    expect(parseRoute('#image/generate')).toEqual({ page: 'generate', kind: 'image' });
    expect(parseRoute('#image')).toEqual({ page: 'generate', kind: 'image' });
    expect(parseRoute('#overview')).toEqual({ page: 'home' });
  });

  it('round-trips model pages, including ids with slashes and a settings tab', () => {
    const href = modelHref('org/qwen3.6-35b', 'optimize');
    expect(parseRoute(href)).toEqual({ page: 'model', id: 'org/qwen3.6-35b', tab: 'optimize' });
    expect(parseRoute(modelHref('fast'))).toEqual({ page: 'model', id: 'fast', tab: undefined });
    expect(routeHref({ page: 'usage', section: 'image' })).toBe('#usage/image');
    expect(parseRoute(storeHref('llm', 'unsloth/Qwen3-GGUF'))).toEqual({ page: 'store', section: 'llm', repo: 'unsloth/Qwen3-GGUF' });
    expect(navIdForRoute(parseRoute('#store/llm'))).toBe('models');
    expect(navIdForRoute(parseRoute(modelHref('fast')))).toBe('models');
  });
});
