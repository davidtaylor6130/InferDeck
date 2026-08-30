import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import App, { DASHBOARD_PAGES } from './App';
import { INFERDECK_VERSION } from './version';

describe('dashboard boundary', () => {
  it('keeps current administration pages and truthful planned workspaces in one registry', () => {
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
      { id: 'image', label: 'Image', preview: true },
      { id: 'music', label: 'Music', preview: true },
      { id: 'post-training', label: 'Post Training', preview: true },
    ]);
    expect(DASHBOARD_PAGES.filter(page => page.section === 'llm')).toHaveLength(4);
    expect(DASHBOARD_PAGES.filter(page => page.section === 'dictation')).toHaveLength(4);
    expect(DASHBOARD_PAGES.filter(page => page.preview)).toHaveLength(3);
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
    expect(html).toContain('<optgroup label="Planned">');
    expect(html).not.toContain('aria-label="Dashboard sections"');
  });

  it('keeps real settings and health controls visible from the product shell', () => {
    const html = renderToStaticMarkup(<App />);
    expect(html).toContain('<summary class="min-h-10');
    expect(html).toContain('Settings</summary>');
    expect(html).toContain('LLM settings');
    expect(html).toContain('Dictation settings');
    expect(html).toContain('Configuration &amp; recovery');
    expect(html).toContain('Open Health and alerts');
  });
});
