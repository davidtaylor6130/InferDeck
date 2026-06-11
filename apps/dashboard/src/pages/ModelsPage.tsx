import React, { useEffect, useState } from 'react';
import { getSwapHistory } from '../api';
import { Badge, Button, EmptyState, Panel, SectionTitle } from '../components/ui';
import { useGateway } from '../gateway';
import type { SwapHistoryRow } from '../types';
import { compactModel, formatDate, formatDuration, formatMb, formatTokenCount } from '../utils';

export const ModelsPage: React.FC = () => {
  const { models, swap, swapTo, cancelSwap, unload } = useGateway();
  const [swaps, setSwaps] = useState<SwapHistoryRow[]>([]);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let active = true;
    getSwapHistory().then(rows => { if (active) setSwaps(rows); }).catch(() => {});
    return () => { active = false; };
  }, [swap.swapping]);

  const handleLoad = async (model: string) => {
    setError(null);
    const failure = await swapTo(model);
    if (failure) setError(failure);
  };

  return (
    <div className="space-y-4">
      {error && (
        <div className="rounded-lg border border-danger-rose/40 bg-danger-rose/10 px-4 py-2 text-sm text-danger-rose">{error}</div>
      )}
      {swap.lastError && !swap.swapping && (
        <div className="rounded-lg border border-warning-amber/40 bg-warning-amber/10 px-4 py-2 text-sm text-warning-amber">
          Last swap failed: {swap.lastError}
        </div>
      )}

      <Panel>
        <SectionTitle title="Registered models" aside={`${models.length}`} />
        {models.length === 0 ? (
          <div className="mt-3"><EmptyState title="No models registered" detail="Add models to config/gateway.yml and restart the gateway." /></div>
        ) : (
          <div className="mt-3 overflow-x-auto">
            <table className="w-full min-w-[640px] text-left text-sm">
              <thead>
                <tr className="border-b border-white/10 text-xs uppercase tracking-wide text-text-muted">
                  <th className="py-2 pr-4 font-medium">Model</th>
                  <th className="py-2 pr-4 font-medium">Family</th>
                  <th className="py-2 pr-4 font-medium">Context</th>
                  <th className="py-2 pr-4 font-medium">VRAM</th>
                  <th className="py-2 pr-4 font-medium">Slots</th>
                  <th className="py-2 pr-4 font-medium">State</th>
                  <th className="py-2 font-medium"></th>
                </tr>
              </thead>
              <tbody className="divide-y divide-white/5">
                {models.map(model => {
                  const isTarget = swap.swapping && swap.target === model.id;
                  return (
                    <tr key={model.id}>
                      <td className="py-2.5 pr-4">
                        <div className="flex min-w-0 items-center gap-2">
                          <span className="truncate font-mono text-text-primary" title={model.id}>{compactModel(model.id)}</span>
                          {model.has_vision && <Badge label="Vision" tone="violet" />}
                        </div>
                      </td>
                      <td className="py-2.5 pr-4 text-text-secondary">{model.family || '—'}</td>
                      <td className="py-2.5 pr-4 text-text-secondary">{formatTokenCount(model.context_size)}</td>
                      <td className="py-2.5 pr-4 text-text-secondary">{formatMb(model.vram_required_mb)}</td>
                      <td className="py-2.5 pr-4 text-text-secondary">{model.n_slots}</td>
                      <td className="py-2.5 pr-4">
                        {model.loaded
                          ? <Badge label="Loaded" tone="good" />
                          : isTarget
                            ? <Badge label="Swapping…" tone="info" />
                            : <Badge label="On disk" tone="idle" />}
                      </td>
                      <td className="py-2.5 text-right">
                        {model.loaded ? (
                          <Button onClick={() => void unload()}>Unload</Button>
                        ) : isTarget ? (
                          <Button tone="danger" onClick={() => void cancelSwap()}>Cancel</Button>
                        ) : (
                          <Button tone="blue" disabled={swap.swapping} onClick={() => void handleLoad(model.id)}>Load</Button>
                        )}
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        )}
      </Panel>

      <Panel>
        <SectionTitle title="Swap history" aside="latest 100" />
        {swaps.length === 0 ? (
          <div className="mt-3"><EmptyState title="No swaps recorded" /></div>
        ) : (
          <div className="mt-3 overflow-x-auto">
            <table className="w-full min-w-[560px] text-left text-sm">
              <thead>
                <tr className="border-b border-white/10 text-xs uppercase tracking-wide text-text-muted">
                  <th className="py-2 pr-4 font-medium">When</th>
                  <th className="py-2 pr-4 font-medium">From</th>
                  <th className="py-2 pr-4 font-medium">To</th>
                  <th className="py-2 pr-4 font-medium">Duration</th>
                  <th className="py-2 font-medium">Result</th>
                </tr>
              </thead>
              <tbody className="divide-y divide-white/5">
                {swaps.slice(0, 25).map(row => (
                  <tr key={`${row.timestamp_unix_ms}-${row.to_model}`}>
                    <td className="py-2 pr-4 text-text-secondary">{formatDate(row.timestamp_unix_ms)}</td>
                    <td className="py-2 pr-4 font-mono text-text-secondary">{row.from_model ? compactModel(row.from_model) : '—'}</td>
                    <td className="py-2 pr-4 font-mono text-text-primary">{compactModel(row.to_model)}</td>
                    <td className="py-2 pr-4 text-text-secondary">{formatDuration(row.duration_ms)}</td>
                    <td className="py-2">
                      {row.success
                        ? <Badge label="OK" tone="good" />
                        : <span className="inline-flex items-center gap-2"><Badge label="Failed" tone="critical" /><span className="max-w-[260px] truncate text-xs text-text-muted" title={row.error}>{row.error}</span></span>}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </Panel>
    </div>
  );
};
