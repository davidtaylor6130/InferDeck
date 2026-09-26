import { beforeEach, describe, expect, it } from 'vitest';
import { renderToStaticMarkup } from 'react-dom/server';
import { SubscriptionSavingsPanel } from './SubscriptionSavingsPanel';

const storage = new Map<string, string>();

describe('SubscriptionSavingsPanel', () => {
  beforeEach(() => {
    storage.clear();
    Object.defineProperty(globalThis, 'window', { configurable: true, value: {
      localStorage: {
        getItem: (key: string) => storage.get(key) ?? null,
        setItem: (key: string, value: string) => storage.set(key, value),
      },
    } });
  });

  it('survives localStorage read failures', () => {
    Object.defineProperty(globalThis, 'window', { configurable: true, value: { localStorage: { getItem: () => { throw new Error('blocked'); } } } });
    expect(() => renderToStaticMarkup(<SubscriptionSavingsPanel />)).not.toThrow();
  });

  it('renders the empty state and explicit API-cost inclusion choice', () => {
    const html = renderToStaticMarkup(<SubscriptionSavingsPanel apiCostsCents={1234} />);
    expect(html).toContain('Add cancelled subscriptions to track avoided payments.');
    expect(html).toContain('Count API-equivalent value toward this break-even target');
    expect(html).toContain('$12');
    expect(html).toContain('checked=""');
  });

  it('renders persisted breakdown rows and reuses the portfolio target', () => {
    storage.set('inferdeck:subscription-savings', JSON.stringify([{ id: 'one', name: 'ChatGPT Plus', monthlyCents: 2000, startDate: '2026-01-31' }]));
    storage.set('inferdeck:model-token-costs', JSON.stringify({
      'All tracked models': { breakEvenTarget: 2500 },
    }));
    const html = renderToStaticMarkup(<SubscriptionSavingsPanel />);
    expect(html).toContain('ChatGPT Plus');
    expect(html).toContain('8 avoided payments');
    expect(html).toContain('Savings target');
    expect(html).toContain('value="2500"');
    expect(storage.has('inferdeck:subscription-savings-target')).toBe(false);
  });
});
