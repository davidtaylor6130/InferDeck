import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import App from './App';
import { NAV_ITEMS } from './routes';
import { INFERDECK_VERSION } from './version';

describe('dashboard shell', () => {
  it('navigates by task, with the AI type chosen inside each area', () => {
    expect(NAV_ITEMS.map(item => item.label)).toEqual(['Home', 'Models', 'Generate', 'Requests', 'Usage', 'Health']);
    const html = renderToStaticMarkup(<App />);
    for (const item of NAV_ITEMS) expect(html).toContain(`href="${item.href}"`);
    expect(html).toContain('href="#settings"');
    expect(html).toContain('API settings');
    expect(html).not.toContain('aria-expanded');
  });

  it('renders the product version in the bottom sidebar footer', () => {
    const html = renderToStaticMarkup(<App />);
    expect(html).toContain(`InferDeck v${INFERDECK_VERSION}`);
    expect(html.indexOf(`InferDeck v${INFERDECK_VERSION}`)).toBeLessThan(html.indexOf('<main'));
  });

  it('gives phones a bottom tab bar and a theme switch', () => {
    const html = renderToStaticMarkup(<App />);
    expect(html).toContain('aria-label="Sections"');
    expect(html).not.toContain('<select aria-label="Dashboard page"');
    expect(html).toContain('aria-label="Appearance"');
    expect(html).toContain('aria-label="Match system"');
  });

  it('keeps health reachable and mobile controls inside the phone viewport', () => {
    const html = renderToStaticMarkup(<App />);
    expect(html).toContain('Open Health and alerts');
    expect(html).toContain('sticky top-0 z-20');
    expect(html).toContain('min-h-11 min-w-11');
    expect(html).toContain('aria-label="API settings"');
  });
});
