import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import App, { DASHBOARD_PAGES } from './App';
import { INFERDECK_VERSION } from './version';

describe('dashboard boundary', () => {
  it('exposes Image and Music as complete dashboard divisions', () => {
    expect(DASHBOARD_PAGES).toEqual([
      { id: 'home', label: 'Home' },
      { id: 'llm/settings', label: 'Model Settings', section: 'llm' },
      { id: 'llm/models', label: 'Model Store', section: 'llm' },
      { id: 'llm/usage', label: 'Usage', section: 'llm' },
      { id: 'llm/diagnostics', label: 'Health & alerts', section: 'llm' },
      { id: 'dictation/settings', label: 'Model Settings', section: 'dictation' },
      { id: 'dictation/models', label: 'Model Store', section: 'dictation' },
      { id: 'dictation/usage', label: 'Usage', section: 'dictation' },
      { id: 'dictation/diagnostics', label: 'Health & alerts', section: 'dictation' },
      { id: 'image/generate', label: 'Generate', section: 'image' },
      { id: 'image/settings', label: 'Model Settings', section: 'image' },
      { id: 'image/models', label: 'Model Store', section: 'image' },
      { id: 'image/usage', label: 'Usage', section: 'image' },
      { id: 'image/diagnostics', label: 'Health & alerts', section: 'image' },
      { id: 'music/generate', label: 'Generate', section: 'music' },
      { id: 'music/settings', label: 'Model Settings', section: 'music' },
      { id: 'music/models', label: 'Model Store', section: 'music' },
      { id: 'music/usage', label: 'Usage', section: 'music' },
      { id: 'music/diagnostics', label: 'Health & alerts', section: 'music' },
      { id: 'post-training', label: 'Post Training', preview: true },
    ]);
    expect(DASHBOARD_PAGES.filter(page => page.section === 'llm')).toHaveLength(4);
    expect(DASHBOARD_PAGES.filter(page => page.section === 'dictation')).toHaveLength(4);
    expect(DASHBOARD_PAGES.filter(page => page.section === 'image')).toHaveLength(5);
    expect(DASHBOARD_PAGES.filter(page => page.section === 'music')).toHaveLength(5);
    expect(DASHBOARD_PAGES.filter(page => page.preview)).toHaveLength(1);
  });

  it('renders the product version in the bottom sidebar footer', () => {
    const html = renderToStaticMarkup(<App />);
    expect(html).toContain(`InferDeck v${INFERDECK_VERSION}`);
    expect(html.indexOf(`InferDeck v${INFERDECK_VERSION}`)).toBeLessThan(html.indexOf('<main'));
  });

  it('renders one grouped mobile page selector instead of scrolling tab rows', () => {
    const html = renderToStaticMarkup(<App />);
    expect(html).toContain('aria-label="Dashboard page"');
    expect(html).toContain('<optgroup label="LLM">');
    expect(html).toContain('<optgroup label="Dictation">');
    expect(html).toContain('<optgroup label="Image">');
    expect(html).toContain('<optgroup label="Music">');
    expect(html).not.toContain('<optgroup label="Create">');
    expect(html).toContain('<optgroup label="Planned">');
    expect(html).not.toContain('aria-label="Dashboard sections"');
  });

  it('keeps real settings and health controls visible from the product shell', () => {
    const html = renderToStaticMarkup(<App />);
    expect(html).toContain('<summary class="flex min-h-11');
    expect(html).toContain('Settings</summary>');
    expect(html).toContain('LLM settings');
    expect(html).toContain('Dictation settings');
    expect(html).toContain('Image settings');
    expect(html).toContain('Music settings');
    expect(html).toContain('Configuration &amp; recovery');
    expect(html).toContain('Open Health and alerts');
  });

  it('keeps mobile navigation reachable and settings inside the phone viewport', () => {
    const html = renderToStaticMarkup(<App />);
    expect(html).toContain('<header class="sticky top-0 z-20');
    expect(html).toContain('min-h-11 min-w-11');
    expect(html).toContain('w-[min(18rem,calc(100vw-2rem))]');
    expect(html).toContain('class="min-h-11 w-full');
  });
});
