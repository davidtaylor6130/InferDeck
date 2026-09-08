import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { calculateSavings, calculateSubscriptionSavings, loadCancelledSubscriptions, loadIncludeApiCosts, saveCancelledSubscriptions, saveIncludeApiCosts, validateSubscription } from './subscriptionSavings';
const base = { id: 'chatgpt', name: 'ChatGPT', monthlyCents: 2000, startDate: '2026-01-31' };
describe('subscription savings', () => {
  it('rejects invalid dates and duplicate IDs without looping or double counting', () => {
    expect(() => calculateSubscriptionSavings(base, { today: new Date(NaN) })).toThrow();
    expect(() => calculateSubscriptionSavings(base, { today: new Date('2200-01-01') })).toThrow();
    expect(() => calculateSavings([base, base], { today: '2026-02-01' })).toThrow('Duplicate');
    expect(calculateSubscriptionSavings(base, { today: '2026-01-30' }).totalCents).toBe(0);
    expect(calculateSavings([], { targetCents: 10000 }).remainingCents).toBe(10000);
    expect(() => calculateSavings([base], { today: '2026-02-01', apiCostsCents: Number.MAX_SAFE_INTEGER, includeApiCosts: true })).toThrow();
  });

  it('counts anniversary payments and clamps month ends', () => {
    const result = calculateSubscriptionSavings(base, { today: '2026-04-30' });
    expect(result.payments).toEqual([{ date: '2026-01-31', amountCents: 2000 }, { date: '2026-02-28', amountCents: 2000 }, { date: '2026-03-31', amountCents: 2000 }, { date: '2026-04-30', amountCents: 2000 }]);
    expect(result.totalCents).toBe(8000);
  });
  it('stops at end date and applies effective dated rates', () => {
    const result = calculateSubscriptionSavings({ ...base, endDate: '2026-03-15', rateChanges: [{ effectiveDate: '2026-02-01', monthlyCents: 2500 }] }, { today: '2026-12-01' });
    expect(result.payments).toEqual([{ date: '2026-01-31', amountCents: 2000 }, { date: '2026-02-28', amountCents: 2500 }]);
  });
  it('excludes API costs unless explicitly included and calculates target progress', () => {
    const excluded = calculateSavings([base], { today: '2026-02-01', apiCostsCents: 999, targetCents: 5000 });
    expect(excluded.totalCents).toBe(2000); expect(excluded.remainingCents).toBe(3000); expect(excluded.progressRatio).toBe(0.4);
    expect(calculateSavings([base], { today: '2026-02-01', apiCostsCents: 999, includeApiCosts: true }).totalCents).toBe(2999);
  });
  it('rejects invalid, duplicate, unsafe, and malformed entries', () => {
    expect(validateSubscription({ ...base, endDate: '2025-01-01' }).length).toBeGreaterThan(0); expect(validateSubscription({ ...base, monthlyCents: Number.MAX_SAFE_INTEGER + 1 }).length).toBeGreaterThan(0); expect(validateSubscription(null).length).toBeGreaterThan(0);
    expect(validateSubscription({ ...base, rateChanges: [{ effectiveDate: '2026-02-01', monthlyCents: 1 }, { effectiveDate: '2026-02-01', monthlyCents: 2 }] }).length).toBeGreaterThan(0);
  });
  describe('localStorage', () => {
    const originalWindow = Object.getOwnPropertyDescriptor(globalThis, 'window');
    afterEach(() => {
      if (originalWindow) Object.defineProperty(globalThis, 'window', originalWindow);
      else Reflect.deleteProperty(globalThis, 'window');
    });
    beforeEach(() => {
      const values = new Map<string, string>();
      Object.defineProperty(globalThis, 'window', { configurable: true, value: { localStorage: {
        clear: () => values.clear(), getItem: (key: string) => values.get(key) ?? null,
        setItem: (key: string, value: string) => values.set(key, value),
      } } });
    });
    it('round trips valid entries and drops malformed entries', () => {
      saveCancelledSubscriptions([base]); window.localStorage.setItem('inferdeck:subscription-savings', JSON.stringify([base, { nope: true }]));
      expect(loadCancelledSubscriptions()).toEqual([base]); window.localStorage.setItem('inferdeck:subscription-savings', JSON.stringify([null, base])); expect(loadCancelledSubscriptions()).toEqual([base]);
    });
    it('persists the explicit API-equivalent inclusion choice', () => {
      expect(loadIncludeApiCosts()).toBe(true);
      saveIncludeApiCosts(false);
      expect(loadIncludeApiCosts()).toBe(false);
      saveIncludeApiCosts(true);
      expect(loadIncludeApiCosts()).toBe(true);
    });
    it('preserves legacy API-only break-even progress without a preference', () => {
      const included = loadIncludeApiCosts();
      const summary = calculateSavings([], {
        apiCostsCents: 2500,
        includeApiCosts: included,
        targetCents: 10000,
      });
      expect(summary.subscriptionCents).toBe(0);
      expect(summary.apiCostsCents).toBe(2500);
      expect(summary.totalCents).toBe(2500);
      expect(summary.progressRatio).toBe(0.25);
    });
  });
});
