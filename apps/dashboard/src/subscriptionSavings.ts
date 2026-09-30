export interface SubscriptionRateChange {
  effectiveDate: string;
  monthlyCents: number;
}

export interface CancelledSubscription {
  id: string;
  name: string;
  monthlyCents: number;
  startDate: string;
  endDate?: string;
  rateChanges?: SubscriptionRateChange[];
}

export interface SubscriptionPayment {
  date: string;
  amountCents: number;
}

export interface SubscriptionSavings {
  id: string;
  name: string;
  monthlyCents: number;
  payments: SubscriptionPayment[];
  totalCents: number;
}

export interface SavingsOptions {
  today?: string | Date;
  apiCostsCents?: number;
  includeApiCosts?: boolean;
  targetCents?: number;
}

export interface SavingsSummary {
  subscriptions: SubscriptionSavings[];
  subscriptionCents: number;
  apiCostsCents: number;
  totalCents: number;
  includedApiCosts: boolean;
  remainingCents: number | null;
  progressRatio: number | null;
}

export const SUBSCRIPTION_SAVINGS_STORAGE_KEY = 'inferdeck:subscription-savings';
export const SUBSCRIPTION_SAVINGS_INCLUDE_API_KEY = 'inferdeck:subscription-savings-include-api';
export const SUBSCRIPTION_SAVINGS_CHANGED_EVENT = 'inferdeck:subscription-savings-changed';
/** Prevent unbounded payment totals while allowing realistic $1m monthly plans. */
const MAX_MONTHLY_CENTS = 100_000_000;
const DATE_RE = /^\d{4}-\d{2}-\d{2}$/;
const MIN_YEAR = 1970;
const MAX_YEAR = 2100;

function parseDate(value: string): Date | null {
  if (!DATE_RE.test(value)) return null;
  const [year, month, day] = value.split('-').map(Number);
  if (year < MIN_YEAR || year > MAX_YEAR) return null;
  const date = new Date(Date.UTC(year, month - 1, day));
  return Number.isFinite(date.getTime()) && date.getUTCFullYear() === year
    && date.getUTCMonth() === month - 1 && date.getUTCDate() === day ? date : null;
}

function dateKey(date: Date): string {
  return date.toISOString().slice(0, 10);
}

function daysInMonth(year: number, month: number): number {
  return new Date(Date.UTC(year, month + 1, 0)).getUTCDate();
}

function anniversary(start: Date, offset: number): Date {
  const rawMonth = start.getUTCMonth() + offset;
  const year = start.getUTCFullYear() + Math.floor(rawMonth / 12);
  const month = ((rawMonth % 12) + 12) % 12;
  return new Date(Date.UTC(year, month, Math.min(start.getUTCDate(), daysInMonth(year, month))));
}

export function validateSubscription(value: unknown): string[] {
  if (!value || typeof value !== 'object') return ['subscription must be an object'];
  const subscription = value as Partial<CancelledSubscription>;
  const errors: string[] = [];
  if (typeof subscription.id !== 'string' || !subscription.id.trim()) errors.push('id is required');
  if (typeof subscription.name !== 'string' || !subscription.name.trim()) errors.push('name is required');
  if (!Number.isSafeInteger(subscription.monthlyCents) || (subscription.monthlyCents ?? -1) < 0 || subscription.monthlyCents! > MAX_MONTHLY_CENTS) errors.push('monthlyCents must be between 0 and $1,000,000');
  const start = typeof subscription.startDate === 'string' ? parseDate(subscription.startDate) : null;
  if (!start) errors.push('startDate must be a valid bounded YYYY-MM-DD date');
  const end = subscription.endDate === undefined ? null : typeof subscription.endDate === 'string' ? parseDate(subscription.endDate) : null;
  if (subscription.endDate !== undefined && !end) errors.push('endDate must be a valid bounded YYYY-MM-DD date');
  if (start && end && end < start) errors.push('endDate must be on or after startDate');
  const changes = subscription.rateChanges ?? [];
  if (!Array.isArray(changes)) errors.push('rateChanges must be an array');
  else {
    let previous = '';
    for (const change of changes) {
      const effective = change && typeof change.effectiveDate === 'string' ? parseDate(change.effectiveDate) : null;
      if (!effective) errors.push('each rate change needs a valid effectiveDate');
      if (typeof change?.effectiveDate === 'string' && change.effectiveDate <= previous) errors.push('rate changes must have unique ascending dates');
      if (typeof change?.effectiveDate === 'string') previous = change.effectiveDate;
      if (!Number.isSafeInteger(change?.monthlyCents) || (change?.monthlyCents ?? -1) < 0 || change!.monthlyCents > MAX_MONTHLY_CENTS) errors.push('rate change monthlyCents must be between 0 and $1,000,000');
      if (start && effective && effective < start) errors.push('rate changes cannot predate startDate');
    }
  }
  return errors;
}

function amountForDate(subscription: CancelledSubscription, date: string): number {
  let amount = subscription.monthlyCents;
  for (const change of subscription.rateChanges ?? []) {
    if (change.effectiveDate > date) break;
    amount = change.monthlyCents;
  }
  return amount;
}

export function calculateSubscriptionSavings(subscription: CancelledSubscription, options: SavingsOptions = {}): SubscriptionSavings {
  const errors = validateSubscription(subscription);
  if (errors.length) throw new Error(`Invalid subscription: ${errors.join('; ')}`);
  const start = parseDate(subscription.startDate)!;
  const value = options.today ?? new Date();
  const today = typeof value === 'string'
    ? parseDate(value)
    : value instanceof Date && Number.isFinite(value.getTime())
      ? parseDate(new Date(Date.UTC(value.getFullYear(), value.getMonth(), value.getDate())).toISOString().slice(0, 10))
      : null;
  if (!today) throw new Error('today must be a valid bounded YYYY-MM-DD date');
  const end = subscription.endDate ? parseDate(subscription.endDate)! : today;
  const cutoff = end < today ? end : today;
  const payments: SubscriptionPayment[] = [];
  for (let offset = 0; ; offset += 1) {
    const date = anniversary(start, offset);
    if (!Number.isFinite(date.getTime()) || date > cutoff) break;
    const key = dateKey(date);
    payments.push({ date: key, amountCents: amountForDate(subscription, key) });
  }
  const totalCents = payments.reduce((sum, payment) => {
    const next = sum + payment.amountCents;
    if (!Number.isSafeInteger(next)) throw new Error('subscription savings total exceeds safe integer range');
    return next;
  }, 0);
  return { id: subscription.id, name: subscription.name, monthlyCents: subscription.monthlyCents, payments, totalCents };
}

export function calculateSavings(subscriptions: CancelledSubscription[], options: SavingsOptions = {}): SavingsSummary {
  const ids = new Set<string>();
  for (const subscription of subscriptions) {
    if (ids.has(subscription.id)) throw new Error(`Duplicate subscription id: ${subscription.id}`);
    ids.add(subscription.id);
  }
  const results = subscriptions.map((subscription) => calculateSubscriptionSavings(subscription, options));
  const subscriptionCents = results.reduce((sum, result) => { const next = sum + result.totalCents; if (!Number.isSafeInteger(next)) throw new Error('subscription savings total exceeds safe integer range'); return next; }, 0);
  const apiCostsCents = Number.isSafeInteger(options.apiCostsCents) && options.apiCostsCents! >= 0 ? options.apiCostsCents! : 0;
  const includedApiCosts = options.includeApiCosts === true;
  const totalCents = subscriptionCents + (includedApiCosts ? apiCostsCents : 0);
  if (!Number.isSafeInteger(totalCents)) throw new Error('savings total exceeds safe integer range');
  const target = Number.isSafeInteger(options.targetCents) && options.targetCents! > 0 ? options.targetCents! : null;
  return {
    subscriptions: results, subscriptionCents, apiCostsCents, totalCents, includedApiCosts,
    remainingCents: target === null ? null : Math.max(0, target - totalCents),
    progressRatio: target === null ? null : Math.min(1, totalCents / target),
  };
}

export function loadCancelledSubscriptions(): CancelledSubscription[] {
  if (typeof window === 'undefined') return [];
  try {
    const value: unknown = JSON.parse(window.localStorage.getItem(SUBSCRIPTION_SAVINGS_STORAGE_KEY) || '[]');
    if (!Array.isArray(value)) return [];
    const valid: CancelledSubscription[] = [];
    const ids = new Set<string>();
    for (const item of value) {
      if (validateSubscription(item).length === 0) {
        const subscription = item as CancelledSubscription;
        if (!ids.has(subscription.id)) { ids.add(subscription.id); valid.push(subscription); }
      }
    }
    return valid;
  } catch { return []; }
}

export function saveCancelledSubscriptions(subscriptions: CancelledSubscription[]): void {
  const ids = new Set<string>();
  for (const subscription of subscriptions) {
    const errors = validateSubscription(subscription);
    if (errors.length) throw new Error(`Invalid subscription: ${errors.join('; ')}`);
    if (ids.has(subscription.id)) throw new Error(`Duplicate subscription id: ${subscription.id}`);
    ids.add(subscription.id);
  }
  if (typeof window !== 'undefined') {
    window.localStorage.setItem(SUBSCRIPTION_SAVINGS_STORAGE_KEY, JSON.stringify(subscriptions));
    window.dispatchEvent?.(new Event(SUBSCRIPTION_SAVINGS_CHANGED_EVENT));
  }
}

export function loadIncludeApiCosts(): boolean {
  if (typeof window === 'undefined') return true;
  try {
    const saved = window.localStorage.getItem(SUBSCRIPTION_SAVINGS_INCLUDE_API_KEY);
    return saved === null ? true : saved === 'true';
  } catch {
    return true;
  }
}

export function saveIncludeApiCosts(value: boolean): void {
  if (typeof window === 'undefined') return;
  window.localStorage.setItem(SUBSCRIPTION_SAVINGS_INCLUDE_API_KEY, String(value));
  window.dispatchEvent?.(new Event(SUBSCRIPTION_SAVINGS_CHANGED_EVENT));
}
