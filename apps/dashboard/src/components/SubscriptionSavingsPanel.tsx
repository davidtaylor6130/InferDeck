import React, { useMemo, useState } from 'react';
import { Button, Panel, ProgressBar, SectionTitle } from './ui';
import {
  calculateSavings,
  loadCancelledSubscriptions,
  loadIncludeApiCosts,
  saveCancelledSubscriptions,
  saveIncludeApiCosts,
  type CancelledSubscription,
} from '../subscriptionSavings';
import {
  ALL_MODELS,
  DEFAULT_COST_CONFIG,
  MODEL_COST_DEFAULTS_VERSION,
  getCostConfigForModel,
  loadCostConfig,
  saveCostConfig,
  type ModelCostConfig,
} from '../cost';
import { formatCurrency } from '../utils';

const today = () => new Date().toISOString().slice(0, 10);
const newId = () => typeof crypto?.randomUUID === 'function'
  ? crypto.randomUUID()
  : `subscription-${Date.now()}-${Math.random().toString(36).slice(2)}`;
const empty = (): CancelledSubscription => ({ id: newId(), name: '', monthlyCents: 0, startDate: today() });
type Draft = CancelledSubscription & { monthlyText: string };
const toDraft = (value: CancelledSubscription): Draft => ({ ...value, monthlyText: (value.monthlyCents / 100).toFixed(2) });
export const SubscriptionSavingsPanel: React.FC<{ apiCostsCents?: number }> = ({ apiCostsCents = 0 }) => {
  const [subscriptions, setSubscriptions] = useState(() => loadCancelledSubscriptions());
  const [draft, setDraft] = useState<Draft | null>(null);
  const [includeApiCosts, setIncludeApiCosts] = useState(() => loadIncludeApiCosts());
  const [savedCosts, setSavedCosts] = useState<Record<string, ModelCostConfig>>(
    () => loadCostConfig({}, DEFAULT_COST_CONFIG),
  );
  const [storageError, setStorageError] = useState('');
  const portfolio = getCostConfigForModel(
    ALL_MODELS, savedCosts, {}, DEFAULT_COST_CONFIG,
  );
  const targetCents = Math.max(0, Math.round(portfolio.breakEvenTarget * 100));
  const summary = useMemo(() => calculateSavings(subscriptions, { apiCostsCents, includeApiCosts, targetCents }), [subscriptions, apiCostsCents, includeApiCosts, targetCents]);

  const persist = (next: CancelledSubscription[]) => {
    try { saveCancelledSubscriptions(next); setSubscriptions(next); setStorageError(''); return true; }
    catch (error) { setStorageError(error instanceof Error ? error.message : 'Unable to save subscription changes.'); return false; }
  };
  const saveDraft = () => {
    if (!draft) return;
    const monthly = Number(draft.monthlyText);
    if (!Number.isFinite(monthly) || monthly < 0) { setStorageError('Monthly price must be a valid non-negative amount.'); return; }
    const next: CancelledSubscription = { ...draft, monthlyCents: Math.round(monthly * 100) };
    delete (next as Partial<Draft>).monthlyText;
    if (persist([...subscriptions.filter(item => item.id !== next.id), next])) setDraft(null);
  };
  const updateInclude = (value: boolean) => {
    try { saveIncludeApiCosts(value); setIncludeApiCosts(value); setStorageError(''); }
    catch { setStorageError('Unable to save savings preferences.'); }
  };
  const updateTarget = (value: number) => {
    const target = Number.isFinite(value) && value >= 0 ? value : 0;
    const merged = {
      ...savedCosts,
      [ALL_MODELS]: {
        ...portfolio,
        breakEvenTarget: target,
        defaultsVersion: MODEL_COST_DEFAULTS_VERSION,
      },
    };
    try { saveCostConfig(merged); setSavedCosts(merged); setStorageError(''); }
    catch { setStorageError('Unable to save the break-even target.'); }
  };

  return (
    <Panel>
      <SectionTitle title="Cancelled subscriptions" aside={`${formatCurrency(summary.subscriptionCents / 100)} avoided`} />
      <div className="mt-3 grid gap-4 lg:grid-cols-[minmax(0,1fr)_minmax(220px,0.65fr)]">
        <div>
          {summary.subscriptions.length === 0 ? (
            <p className="py-4 text-sm text-text-muted">Add cancelled subscriptions to track avoided payments.</p>
          ) : (
            <div className="divide-y divide-white/10">
              {summary.subscriptions.map(item => (
                <div key={item.id} className="flex items-center justify-between gap-3 py-3">
                  <div className="min-w-0"><div className="truncate text-sm text-text-primary">{item.name}</div><div className="text-xs text-text-muted">{item.payments.length} avoided payment{item.payments.length === 1 ? '' : 's'} - {formatCurrency(item.monthlyCents / 100)} monthly</div></div>
                  <div className="flex shrink-0 items-center gap-3"><span className="text-sm text-success-green">{formatCurrency(item.totalCents / 100)}</span><Button onClick={() => setDraft(toDraft(subscriptions.find(value => value.id === item.id)!))}>Edit</Button><Button onClick={() => persist(subscriptions.filter(value => value.id !== item.id))}>Delete</Button></div>
                </div>
              ))}
            </div>
          )}
          <Button onClick={() => setDraft(toDraft(empty()))}>Add subscription</Button>
        </div>
        <div className="border-l border-white/10 pl-4">
          <div className="text-xs text-text-muted">Savings target</div>
          <input aria-label="Savings target in USD" className="mt-1 h-9 w-full border border-white/10 bg-[#0b1626] px-2 text-sm text-text-primary" type="number" min="0" step="0.01" value={portfolio.breakEvenTarget} onChange={event => updateTarget(Number(event.target.value))} />
          {summary.progressRatio !== null && <><div className="mt-3 flex justify-between text-xs"><span className="text-queue-blue">{Math.round(summary.progressRatio * 100)}% reached</span><span className="text-text-muted">{formatCurrency((summary.remainingCents ?? 0) / 100)} remaining</span></div><ProgressBar percent={summary.progressRatio * 100} tone="info" /></>}
          <label className="mt-4 flex items-center gap-2 text-xs text-text-muted"><input type="checkbox" checked={includeApiCosts} onChange={event => updateInclude(event.target.checked)} /> Count API-equivalent value toward this break-even target ({formatCurrency(apiCostsCents / 100)})</label>
        </div>
      </div>
      {draft && <form className="mt-4 grid gap-3 border-t border-white/10 pt-4 sm:grid-cols-4" onSubmit={event => { event.preventDefault(); saveDraft(); }}><label className="text-xs text-text-muted">Name<input required className="mt-1 h-9 w-full border border-white/10 bg-[#0b1626] px-2 text-sm text-text-primary" value={draft.name} onChange={event => setDraft({ ...draft, name: event.target.value })} /></label><label className="text-xs text-text-muted">Monthly USD<input required min="0" step="0.01" type="number" className="mt-1 h-9 w-full border border-white/10 bg-[#0b1626] px-2 text-sm text-text-primary" value={draft.monthlyText} onChange={event => setDraft({ ...draft, monthlyText: event.target.value })} /></label><label className="text-xs text-text-muted">First avoided billing date<input required type="date" className="mt-1 h-9 w-full border border-white/10 bg-[#0b1626] px-2 text-sm text-text-primary" value={draft.startDate} onChange={event => setDraft({ ...draft, startDate: event.target.value })} /></label><label className="text-xs text-text-muted">Count through<input type="date" className="mt-1 h-9 w-full border border-white/10 bg-[#0b1626] px-2 text-sm text-text-primary" value={draft.endDate ?? ''} onChange={event => setDraft({ ...draft, endDate: event.target.value || undefined })} /></label><div className="flex gap-2 sm:col-span-4"><Button type="submit">Save</Button><Button type="button" onClick={() => setDraft(null)}>Cancel</Button></div></form>}
      {storageError && <p role="alert" className="mt-3 text-xs text-danger-rose">{storageError}</p>}
    </Panel>
  );
};
