import React from 'react';
import type { PageProps } from '../types';
import { CopyButton } from '../components/CopyButton';
import { SectionCard } from '../components/SectionCard';
import { DASHBOARD_URL, GATEWAY_API, LLAMA_BACKEND, OPENAI_API } from '../utils';

export const SettingsPage: React.FC<PageProps> = ({ state, actions }) => (
  <div className="grid gap-5 xl:grid-cols-2">
    <SectionCard title="Read-only Configuration">
      <Config label="Dashboard URL" value={DASHBOARD_URL} onCopied={actions.toast} />
      <Config label="Gateway API" value={GATEWAY_API} onCopied={actions.toast} />
      <Config label="OpenAI API" value={OPENAI_API} onCopied={actions.toast} />
      <Config label="llama.cpp backend" value={LLAMA_BACKEND} onCopied={actions.toast} />
    </SectionCard>
    <SectionCard title="Scheduler Settings">
      <Config label="Mode" value={state.statusData?.mode?.mode || 'ai'} />
      <Config label="Queue policy" value="single_gpu_fifo_with_priority" />
      <Config label="Background jobs" value="allowed" />
      <Config label="SSE stream" value="/events/stream with polling fallback" />
    </SectionCard>
    <SectionCard title="Security Settings">
      <Config label="API keys" value="Configured values are hidden" />
      <Config label="Local domain" value="ai.homelab.com" />
      <Config label="CORS" value="Local network only" />
    </SectionCard>
    <SectionCard title="Storage & Version">
      <Config label="Data directory" value="D:\\AI-Gateway\\data" />
      <Config label="Logs directory" value="D:\\AI-Gateway\\logs" />
      <Config label="Storage mode" value="SQLite WAL" />
      <Config label="Version" value={`InferDeck gateway v${state.healthData?.version || '0.1.0'}`} />
    </SectionCard>
  </div>
);

const Config: React.FC<{ label: string; value: string; onCopied?: (message: string) => void }> = ({ label, value, onCopied }) => (
  <div className="flex min-w-0 items-center justify-between gap-4 border-b border-border-slate/70 py-3 last:border-b-0">
    <div className="min-w-0">
      <p className="text-xs uppercase tracking-wide text-text-muted">{label}</p>
      <p className="mt-1 truncate font-mono text-sm text-text-primary" title={value}>{value}</p>
    </div>
    {onCopied && <CopyButton value={value} label={label} onCopied={onCopied} />}
  </div>
);
