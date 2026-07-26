import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import App, { DASHBOARD_PAGES } from './App';

describe('dashboard boundary', () => {
  it('keeps a global home plus complete LLM and dictation administration sections', () => {
    expect(DASHBOARD_PAGES).toEqual([
      { id: 'home', label: 'Home' },
      { id: 'llm/operate', label: 'Operate', section: 'llm' },
      { id: 'llm/models', label: 'Models', section: 'llm' },
      { id: 'llm/usage', label: 'Usage', section: 'llm' },
      { id: 'llm/diagnostics', label: 'Diagnostics', section: 'llm' },
      { id: 'dictation/operate', label: 'Operate', section: 'dictation' },
      { id: 'dictation/models', label: 'Models', section: 'dictation' },
      { id: 'dictation/usage', label: 'Usage', section: 'dictation' },
      { id: 'dictation/diagnostics', label: 'Diagnostics', section: 'dictation' },
    ]);
    expect(DASHBOARD_PAGES.filter(page => page.section === 'llm')).toHaveLength(4);
    expect(DASHBOARD_PAGES.filter(page => page.section === 'dictation')).toHaveLength(4);
  });

  it('renders the product version in the bottom sidebar footer', () => {
    const html = renderToStaticMarkup(<App />);
    expect(html).toContain('InferDeck v0.6.0');
    expect(html.indexOf('InferDeck v0.6.0')).toBeLessThan(html.indexOf('<main'));
  });
});
