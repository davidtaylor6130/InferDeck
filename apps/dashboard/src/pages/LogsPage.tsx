import React, { useEffect, useMemo, useState } from 'react';
import type { PageProps } from '../types';
import { CommandButton } from '../components/CommandButton';
import { LogViewer } from '../components/LogViewer';
import { SectionCard } from '../components/SectionCard';
import { formatError } from '../utils';

const tabs = [
  { id: 'gateway', label: 'Gateway logs' },
  { id: 'job-events', label: 'Job events' },
  { id: 'service-errors', label: 'Service errors' },
];

interface LogEntry {
  timestamp: string;
  level: string;
  service: string;
  jobId?: string | null;
  message: string;
  data?: unknown;
}

export const LogsPage: React.FC<PageProps> = ({ actions }) => {
  const [tab, setTab] = useState(tabs[0].id);
  const [level, setLevel] = useState('all');
  const [service, setService] = useState('all');
  const [jobId, setJobId] = useState('');
  const [search, setSearch] = useState('');
  const [autoScroll, setAutoScroll] = useState(true);
  const [logs, setLogs] = useState<LogEntry[]>([]);
  const [error, setError] = useState<string | null>(null);

  const refresh = async () => {
    const params = new URLSearchParams({ tab, level, service, jobId, search, limit: '500' });
    try {
      const res = await fetch(`/api/logs?${params.toString()}`);
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const data = await res.json();
      setLogs(data.logs || []);
      setError(null);
    } catch (err) {
      setError(formatError(err));
    }
  };

  useEffect(() => {
    void refresh();
    const id = window.setInterval(() => void refresh(), autoScroll ? 4000 : 15000);
    return () => window.clearInterval(id);
  }, [tab, level, service, jobId, search, autoScroll]);

  const lines = useMemo(() => {
    if (error) return [`${new Date().toISOString()} ERROR dashboard ${error}`];
    if (logs.length === 0) return [`${new Date().toISOString()} INFO ${tab} no log entries found`];
    return logs.map(entry => `${entry.timestamp} ${entry.level.toUpperCase()} ${entry.service}${entry.jobId ? ` job=${entry.jobId}` : ''} ${entry.message}${entry.data ? ` ${JSON.stringify(entry.data)}` : ''}`);
  }, [error, logs, tab]);

  return (
    <SectionCard title="Logs" eyebrow="Gateway logs, job events, and service errors" action={<CommandButton onClick={() => void refresh()}>Refresh</CommandButton>}>
      <div className="mb-4 flex flex-wrap gap-2">
        {tabs.map(item => <button key={item.id} onClick={() => setTab(item.id)} className={`rounded-lg border px-3 py-2 text-sm ${tab === item.id ? 'border-infer-violet bg-infer-violet/20 text-text-primary' : 'border-border-slate bg-deck-navy text-text-secondary'}`}>{item.label}</button>)}
      </div>
      <div className="mb-4 grid gap-3 md:grid-cols-5">
        <select className="rounded-lg border border-border-slate bg-deck-navy px-3 py-2 text-sm text-text-primary" value={level} onChange={e => setLevel(e.target.value)}><option value="all">All levels</option><option value="debug">DEBUG</option><option value="info">INFO</option><option value="warn">WARN</option><option value="error">ERROR</option></select>
        <select className="rounded-lg border border-border-slate bg-deck-navy px-3 py-2 text-sm text-text-primary" value={service} onChange={e => setService(e.target.value)}><option value="all">All services</option><option value="gateway">Gateway</option><option value="llama_cpp">llama.cpp</option><option value="scheduler">Scheduler</option><option value="service-error">Service Errors</option></select>
        <input className="rounded-lg border border-border-slate bg-deck-navy px-3 py-2 font-mono text-sm text-text-primary" placeholder="Job ID" value={jobId} onChange={e => setJobId(e.target.value)} />
        <input className="rounded-lg border border-border-slate bg-deck-navy px-3 py-2 text-sm text-text-primary" placeholder="Search" value={search} onChange={e => setSearch(e.target.value)} />
        <label className="flex items-center gap-2 rounded-lg border border-border-slate bg-deck-navy px-3 py-2 text-sm text-text-secondary"><input type="checkbox" checked={autoScroll} onChange={e => setAutoScroll(e.target.checked)} /> Auto-scroll</label>
      </div>
      <LogViewer lines={lines} onCopied={actions.toast} />
    </SectionCard>
  );
};
