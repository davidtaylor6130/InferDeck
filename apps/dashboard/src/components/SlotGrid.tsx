import React from 'react';
import type { LiveRequest, ModelInfo } from '../types';
import { formatDuration, formatTokenCount } from '../utils';

const PHASE_LABEL: Record<LiveRequest['phase'], string> = {
  waiting: 'Waiting',
  loading: 'Loading model',
  prefill: 'Reading prompt',
  generating: 'Writing',
};

export const clientName = (request: Pick<LiveRequest, 'apiKeyName'>) => request.apiKeyName || 'Shared API';

const SHORT_PHASE: Record<LiveRequest['phase'], string> = {
  waiting: 'Waiting',
  loading: 'Loading',
  prefill: 'Reading',
  generating: 'Writing',
};

export const SlotStrip: React.FC<{ model: ModelInfo; requests: LiveRequest[]; wide?: boolean }> = ({ model, requests, wide }) => {
  const count = Math.max(1, Math.min(model.n_slots, 32));
  const labelled = count <= 8;
  const busy = requests.length;
  return (
    <div className="min-w-0">
      <div className={`flex gap-1 ${wide ? '' : 'justify-end'}`} aria-label={`${busy} of ${model.n_slots} slots busy`}>
        {Array.from({ length: count }, (_, index) => {
          const request = requests.find(entry => entry.slotId === index);
          if (!request) {
            return <div key={`idle-${index}`} className={`transition-colors duration-300 ${labelled ? (wide ? 'h-11' : 'h-9 max-w-[112px]') : 'h-5'} min-w-0 flex-1 rounded-sm bg-elevated-slate`} title={`Slot ${index}: idle`} />;
          }
          const prefill = request.phase === 'prefill' && request.promptTokens > 0;
          const percent = prefill ? Math.min(100, request.processedTokens / request.promptTokens * 100) : 100;
          const rate = request.phase === 'generating' ? request.tokensPerSecond : request.promptTokensPerSecond;
          const detail = [
            SHORT_PHASE[request.phase],
            rate != null ? `${rate.toFixed(rate >= 100 ? 0 : 1)} t/s` : '',
          ].filter(Boolean).join(' · ');
          const title = `Slot ${index}: ${clientName(request)}, ${PHASE_LABEL[request.phase].toLowerCase()}${request.phase === 'generating' ? `, ${formatTokenCount(request.completionTokens)} tokens written` : ''}${prefill ? `, ${Math.round(percent)}% of prompt read` : ''}, ${formatDuration(request.elapsedMs)}`;
          return (
            <div key={request.id} title={title} className={`relative animate-fade-in ${labelled ? (wide ? 'h-11' : 'h-9 max-w-[112px]') : 'h-5'} min-w-0 flex-1 overflow-hidden rounded-sm border-l-2 border-queue-blue bg-queue-blue/10`}>
              <div className="absolute inset-y-0 left-0 bg-queue-blue/15 transition-[width] duration-700 ease-out" style={{ width: `${percent}%` }} />
              {labelled && (
                <div className="relative flex h-full flex-col justify-center px-2 leading-tight">
                  <span className="truncate text-xs text-text-primary">{clientName(request)}</span>
                  <span className="tabular truncate text-2xs text-text-muted">{detail}</span>
                </div>
              )}
            </div>
          );
        })}
      </div>
      <p className="tabular mt-1 text-right text-2xs text-text-muted">{busy} of {model.n_slots} slot{model.n_slots === 1 ? '' : 's'} busy</p>
    </div>
  );
};

export const SlotTable: React.FC<{ model: ModelInfo; requests: LiveRequest[] }> = ({ model, requests }) => (
  <details className="group mt-2">
    <summary className="cursor-pointer list-none py-1 text-xs text-text-muted hover:text-text-secondary">
      <span className="inline-block transition-transform group-open:rotate-90" aria-hidden="true">›</span> Slot details
    </summary>
    <div className="mt-1 overflow-x-auto rounded border border-border-slate">
      <table className="w-full min-w-[440px] table-fixed border-collapse text-left" aria-label={`${model.id} live slots`}>
        <colgroup><col className="w-12" /><col /><col className="w-28" /><col className="w-[76px]" /><col className="w-[76px]" /><col className="w-[72px]" /></colgroup>
        <thead>
          <tr className="text-xs text-text-muted">
            <th className="px-3 py-1.5 font-medium">Slot</th>
            <th className="py-1.5 font-medium">Client (API key)</th>
            <th className="py-1.5 font-medium">Phase</th>
            <th className="py-1.5 text-right font-medium">PP tok/s</th>
            <th className="py-1.5 text-right font-medium">TPS tok/s</th>
            <th className="py-1.5 pr-3 text-right font-medium">Elapsed</th>
          </tr>
        </thead>
        <tbody>
          {requests.map(request => (
            <tr key={`${request.id}:${request.startedUnixMs}`} className="border-t border-border-slate" title={`${request.phase} / ${request.endpoint} / ${request.id}`}>
              <td className="tabular px-3 py-2 text-xs text-text-muted"><span className="sr-only">Slot </span>{request.slotId}</td>
              <td className="min-w-0 truncate py-2 pr-2 text-sm">{clientName(request)} <span className="text-2xs text-text-muted">p{request.priority}</span><span className="sr-only">{request.requestedModel}</span></td>
              <td className="py-2 text-xs text-text-secondary">{PHASE_LABEL[request.phase]}</td>
              <td className="tabular py-2 text-right text-sm">{request.promptTokensPerSecond == null ? 'N/A' : request.promptTokensPerSecond.toFixed(0)}</td>
              <td className="tabular py-2 text-right text-sm">{request.tokensPerSecond == null ? 'N/A' : request.tokensPerSecond.toFixed(1)}</td>
              <td className="tabular py-2 pr-3 text-right text-sm text-text-secondary">{formatDuration(request.elapsedMs)}</td>
            </tr>
          ))}
          {requests.length === 0 && (
            <tr className="border-t border-border-slate"><td colSpan={6} className="px-3 py-2.5 text-xs text-text-muted">All slots idle.</td></tr>
          )}
        </tbody>
      </table>
    </div>
  </details>
);
