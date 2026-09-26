import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import { ApiSettingsPage } from './ApiSettingsPage';

describe('API settings page', () => {
  it('exposes public access and managed-key controls without exposing secrets', () => {
    const html = renderToStaticMarkup(<ApiSettingsPage />);
    expect(html).toContain('API Settings');
    expect(html).toContain('Allow public API traffic');
    expect(html).toContain('-999999');
    expect(html).toContain('Create API key');
    expect(html).toContain('Managed API keys');
    expect(html).not.toContain('auth.token');
  });
});
