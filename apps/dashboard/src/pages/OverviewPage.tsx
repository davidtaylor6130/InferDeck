import React, { useEffect, useMemo, useState } from 'react';
import type { PageProps, ServiceRecord } from '../types';
import { EmptyState } from '../components/EmptyState';
import { MetricCard } from '../components/MetricCard';
import { ModelTable } from '../components/ModelTable';
import { QueuePreview } from '../components/QueuePreview';
import { SectionCard } from '../components/SectionCard';
import { StatusBadge } from '../components/StatusBadge';
import { CommandButton } from '../components/CommandButton';
import { CopyButton } from '../components/CopyButton';
import { DASHBOARD_URL, GATEWAY_API, LLAMA_BACKEND, OPENAI_API, formatBytes, formatUptime, getQueueCounts, isOnlineStatus, modeLabel, timeAgo } from '../utils';

export const OverviewPage: React.FC<PageProps> = ({ state, actions }) => {
  const queue = getQueueCounts(state.statusData, state.jobsList);
  const currentMode = state.statusData?.mode?.mode || 'ai';
  const healthStatus = (state.healthData?.status || (state.connected ? 'healthy' : 'degraded')) as string;
  const degradedReason = state.errors.health || state.errors.status || state.errors.services || 'Gateway reachable, telemetry partially unavailable';
  const activeJob = state.jobsList.find(job => job.status === 'running' || job.status === 'leased') || null;
  const onlineServices = state.servicesList.filter(service => isOnlineStatus(service.status)).length;
  const activeModel = state.runningModels[0]?.name || state.modelsList.find(model => model.loaded)?.name || state.modelsList[0]?.name || 'N/A';
  const services = normalizeServices(state.servicesList, state.connected);
  const llamaService = services.find(service => service.kind === 'llama_cpp' || service.id === 'llama-server');
  const llamaBackendUrl = llamaService?.baseUrl || LLAMA_BACKEND;
  const telemetry = state.statusData?.hardware || state.statusData?.telemetry;
  const samples = Array.isArray(state.statusData?.metricsSamples) ? state.statusData.metricsSamples : [];
  const summary = state.statusData?.summary || {};
  const metrics = state.statusData?.metrics || {};
  const storage = state.statusData?.storage || {};
  const gpu = telemetry?.gpu;
  const gpuUtilization = normalizePercent(gpu?.utilization);
  const vramPercent = normalizePercent(gpu?.memoryPercent);
  const vramLabel = gpu?.memoryUsed != null && gpu?.memoryTotal != null ? `${formatBytes(gpu.memoryUsed)} / ${formatBytes(gpu.memoryTotal)}` : 'N/A';
  const [gpuHistory, setGpuHistory] = useState<Array<{ timestamp: string; utilization: number; vram: number }>>([]);

  useEffect(() => {
    const timestamp = telemetry?.timestamp || state.lastUpdatedAt?.toISOString();
    if (!timestamp || !gpu) return;
    setGpuHistory(current => {
      if (current[current.length - 1]?.timestamp === timestamp) return current;
      return [...current, { timestamp, utilization: gpuUtilization ?? 0, vram: vramPercent ?? 0 }].slice(-60);
    });
  }, [gpu, gpuUtilization, state.lastUpdatedAt, telemetry?.timestamp, vramPercent]);

  const taskManagerHistory = useMemo(() => gpuHistory.length ? gpuHistory : [{ timestamp: 'current', utilization: gpuUtilization ?? 0, vram: vramPercent ?? 0 }], [gpuHistory, gpuUtilization, vramPercent]);

  return (
    <div className="space-y-5">
      <SectionCard title="System Control & Status" bodyClassName="space-y-4">
        <div className="grid gap-0 overflow-hidden rounded-lg border border-border-slate lg:grid-cols-7">
          <StatusBlock label="Overall Health" value={healthStatus === 'healthy' ? 'Healthy' : healthStatus === 'degraded' ? 'Degraded' : 'Offline'} tone={healthStatus} detail={healthStatus === 'healthy' ? 'Everything operating normally' : degradedReason} />
          <StatusBlock label="Current Mode" value={modeLabel(currentMode)} tone={currentMode} detail="Background jobs allowed" />
          <StatusBlock label="Gateway Uptime" value={formatUptime(state.healthData?.uptime)} detail="Since last service start" />
          <StatusBlock label="llama.cpp Status" value={state.connected && state.servicesList.find(s => s.kind === 'llama_cpp') ? (isOnlineStatus(state.servicesList.find(s => s.kind === 'llama_cpp')!.status) ? 'Online' : 'Unknown') : 'Offline'} tone={state.connected ? (state.servicesList.find(s => s.kind === 'llama_cpp') && isOnlineStatus(state.servicesList.find(s => s.kind === 'llama_cpp')!.status) ? 'online' : 'degraded') : 'offline'} detail="127.0.0.1:11434" />
          <StatusBlock label="Queue Status" value={`${queue.queued} queued`} detail={`${queue.running} running • ${queue.paused} paused`} />
          <StatusBlock label="GPU Lock" value={queue.gpuLocked ? 'Locked' : 'Free'} tone={queue.gpuLocked ? 'locked' : 'free'} detail={queue.lockOwner || 'No active workload'} />
          <StatusBlock label="Last Heartbeat" value={timeAgo(state.lastUpdatedAt)} detail={state.connected ? 'All systems responsive' : 'Waiting for gateway'} />
        </div>
        <div>
          <p className="mb-2 text-sm text-text-secondary">Quick Actions</p>
          <div className="flex flex-wrap gap-3">
            <CommandButton onClick={actions.unloadModels}>Unload Models</CommandButton>
            <CommandButton tone="blue" onClick={actions.restartBackend}>Restart llama.cpp</CommandButton>
            <CommandButton tone="amber" onClick={actions.pauseQueue}>Pause Queue</CommandButton>
            <CommandButton tone="violet" onClick={() => window.location.hash = '#queue'}>Open Full Queue</CommandButton>
          </div>
        </div>
      </SectionCard>

      <SectionCard title="GPU Task Manager" action={<StatusBadge label="Live" tone={state.connected ? 'running' : 'offline'} />}>
        <div className="grid gap-5 xl:grid-cols-[0.85fr_1.15fr]">
          <div className="grid gap-4 sm:grid-cols-2">
            <TaskMeter label="Utilization" value={gpuUtilization == null ? 'N/A' : `${gpuUtilization.toFixed(1)}%`} detail={gpu?.name || 'AMD GPU'} percent={gpuUtilization ?? 0} tone="cyan" />
            <TaskMeter label="Dedicated VRAM" value={vramPercent == null ? 'N/A' : `${vramPercent.toFixed(1)}%`} detail={vramLabel} percent={vramPercent ?? 0} tone="mint" />
          </div>
          <div className="min-w-0">
            <div className="mb-3 flex items-center justify-between gap-3 text-xs text-text-secondary">
              <span>{gpu?.backend || 'llama.cpp Vulkan'}</span>
              <span>{telemetry?.timestamp ? timeAgo(telemetry.timestamp) : timeAgo(state.lastUpdatedAt)}</span>
            </div>
            <div className="grid h-52 grid-cols-2 gap-4 rounded-lg border border-border-slate bg-deck-navy p-4">
              <TaskGraph label="GPU %" color="bg-ion-cyan" values={taskManagerHistory.map(item => item.utilization)} />
              <TaskGraph label="VRAM %" color="bg-gpu-mint" values={taskManagerHistory.map(item => item.vram)} />
            </div>
          </div>
        </div>
      </SectionCard>

      <div className="grid gap-4 md:grid-cols-2 xl:grid-cols-3 2xl:grid-cols-6">
        <MetricCard title="Queue" icon="◇" lines={[
          { label: 'Queued', value: queue.queued },
          { label: 'Running', value: queue.running },
          { label: 'Paused', value: queue.paused },
          { label: 'Failed', value: queue.failed, tone: queue.failed ? 'rose' : 'muted' },
        ]} />
        <MetricCard title="GPU" icon="◌" lines={[
          { label: 'Utilization', value: gpuUtilization != null ? `${gpuUtilization.toFixed(1)}%` : 'N/A', bar: gpuUtilization ?? undefined },
          { label: 'VRAM', value: vramLabel, bar: vramPercent ?? undefined },
          { label: 'Temp', value: gpu?.temperature != null ? `${gpu.temperature}°C` : 'N/A' },
          { label: 'Lock Owner', value: queue.lockOwner || '—' },
        ]} />
        <MetricCard title="llama.cpp" icon="⬡" lines={[
          { label: 'Status', value: state.connected ? 'Online' : 'Offline', tone: state.connected ? 'green' : 'amber' },
          { label: 'Loaded', value: state.modelsList.length ? Math.min(state.modelsList.length, 1) : 'N/A' },
          { label: 'Installed', value: state.modelsList.length || 'N/A' },
          { label: 'Active', value: <span className="font-mono text-[11px]" title={activeModel}>{activeModel}</span> },
        ]} />
        <MetricCard title="Services" icon="◈" lines={[
          { label: 'Online / Total', value: `${onlineServices} / ${services.length}` },
          { label: 'Unhealthy', value: services.filter(s => !isOnlineStatus(s.status)).length, tone: 'amber' },
          { label: 'Last Check', value: timeAgo(state.lastUpdatedAt) },
        ]} />
        <MetricCard title="Jobs Today" icon="○" lines={[
          { label: 'Total', value: summary.jobsToday ?? state.jobsList.length },
          { label: 'Succeeded', value: summary.succeededToday ?? state.jobsList.filter(j => j.status === 'succeeded').length },
          { label: 'Failed', value: summary.failedToday ?? queue.failed, tone: (summary.failedToday ?? queue.failed) ? 'rose' : 'muted' },
          { label: 'Avg. Latency', value: summary.avgLatencyMs ? `${Math.round(summary.avgLatencyMs)} ms` : 'No samples' },
        ]} />
        <MetricCard title="Tokens" icon="T" lines={[
          { label: 'Total', value: formatTokenCount(summary.totalTokens ?? metrics.total_tokens) },
          { label: 'Prompt', value: formatTokenCount(summary.promptTokens ?? metrics.tokens_processed) },
          { label: 'Completion', value: formatTokenCount(summary.completionTokens ?? metrics.tokens_generated) },
          { label: 'Requests', value: metrics.total_requests ?? 0 },
        ]} />
        <MetricCard title="Network" icon="⌁" lines={[
          { label: 'Dashboard', value: <NetworkValue value={DASHBOARD_URL} onCopied={actions.toast} /> },
          { label: 'Gateway', value: <NetworkValue value={GATEWAY_API} onCopied={actions.toast} /> },
          { label: 'OpenAI', value: <NetworkValue value={OPENAI_API} onCopied={actions.toast} /> },
          { label: 'Backend', value: <NetworkValue value={llamaBackendUrl} onCopied={actions.toast} /> },
        ]} />
      </div>

      <div className="grid gap-4 xl:grid-cols-[1.05fr_1.25fr]">
        <SectionCard title="Active Workload" action={activeJob && <StatusBadge label="Running" tone="running" />}>
          {activeJob ? (
            <div className="grid gap-4 md:grid-cols-[44px_1fr_1fr]">
              <div className="flex h-12 w-12 items-center justify-center rounded-xl border border-infer-violet bg-infer-violet/20 text-infer-violet">□</div>
              <div className="grid min-w-0 gap-4 sm:grid-cols-2">
                <Info label="Job ID" value={activeJob.id} mono copy onCopied={actions.toast} />
                <Info label="Type" value={activeJob.type} />
                <Info label="Client" value={activeJob.client || 'Open WebUI'} />
                <Info label="Model" value={activeJob.model || activeModel} mono />
                <Info label="Resource class" value={activeJob.resourceClass || activeJob.resource_class || 'gpu_llm'} mono />
                <Info label="Started" value={activeJob.startedAt || activeJob.started_at || 'Recently'} />
              </div>
              <div className="flex min-w-0 flex-col justify-end">
                <Info label="Duration" value="00:03:47" />
                <div className="mt-5 flex items-center gap-3">
                  <div className="h-2 flex-1 overflow-hidden rounded-full bg-card-highlight"><div className="h-full w-[38%] rounded-full bg-infer-violet" /></div>
                  <span className="text-sm text-text-primary">38%</span>
                </div>
                <CommandButton tone="rose" className="mt-5 w-fit" onClick={() => actions.cancelJob(activeJob.id)}>Cancel Job</CommandButton>
              </div>
            </div>
          ) : (
            <EmptyState title="GPU is free. No active AI workload." description="The scheduler has no running GPU lease at the moment." />
          )}
        </SectionCard>

        <SectionCard title="Loaded Models" action={<CommandButton onClick={() => window.location.hash = '#models'}>View all</CommandButton>}>
          <ModelTable models={state.modelsList.slice(0, 3)} loadedNames={state.runningModels.map(m => m.name)} onLoad={actions.loadModel} onUnload={actions.unloadModel} onCopied={actions.toast} />
          {state.modelsList.length === 0 && <EmptyState title="No GGUF models found." description="Place .gguf files in the models directory or check the llama.cpp backend connection at 127.0.0.1:11434." />}
        </SectionCard>
      </div>

      <div className="grid gap-4 2xl:grid-cols-[1.05fr_1fr_1.2fr]">
        <SectionCard title="Queue Preview" action={<CommandButton onClick={() => window.location.hash = '#queue'}>View full</CommandButton>}>
          {state.jobsList.length ? <QueuePreview jobs={state.jobsList} /> : <EmptyState title="Queue is empty." description="Queued jobs will appear here when the gateway reports them." />}
        </SectionCard>
        <SectionCard title="Services" action={<CommandButton onClick={() => window.location.hash = '#services'}>View all</CommandButton>}>
          <div className="overflow-x-auto">
            <table className="min-w-[520px] w-full table-fixed text-left text-sm">
              <thead><tr className="bg-card-highlight text-xs text-text-secondary"><th className="rounded-l-lg px-3 py-2">Service</th><th className="px-3 py-2">Status</th><th className="px-3 py-2">Address</th><th className="px-3 py-2">Last</th><th className="rounded-r-lg px-3 py-2">Action</th></tr></thead>
              <tbody className="divide-y divide-border-slate/70">{services.map(s => <tr key={s.id}><td className="truncate px-3 py-3">{s.name}</td><td className="px-3 py-3"><StatusBadge label={s.status} tone={s.status} /></td><td className="truncate px-3 py-3 font-mono text-xs text-text-secondary">{s.baseUrl || '—'}</td><td className="px-3 py-3">{s.lastHealthcheckAt ? timeAgo(s.lastHealthcheckAt) : '—'}</td><td className="px-3 py-3"><CommandButton className="min-h-8 px-3 py-1 text-xs" onClick={() => actions.restartService(s.id)}>{s.status === 'not_configured' ? '—' : '↻'}</CommandButton></td></tr>)}</tbody>
            </table>
          </div>
        </SectionCard>
        <SectionCard title="System Activity" action={<StatusBadge label="1H" tone="locked" />}>
          <div className="mb-3 flex justify-end gap-6 text-xs text-text-secondary"><span className="text-infer-violet">Queue Depth</span><span className="text-success-green">GPU Utilization</span></div>
          {samples.length > 0 ? (
            <div className="flex h-56 items-end gap-1 rounded-lg border border-border-slate bg-deck-navy p-4">
              {samples.slice(-60).map((sample: any, index: number) => <div key={index} className="flex flex-1 flex-col justify-end gap-1"><div className="rounded-t bg-success-green" style={{ height: `${Math.max(1, Math.min(Number(sample.metricValue ?? 0), 100))}%` }} /><div className="h-1 rounded bg-infer-violet" /></div>)}
            </div>
          ) : (
            <EmptyState title="No activity samples yet." description="Queue and GPU charts need telemetry or event history from the gateway." />
          )}
        </SectionCard>
      </div>

      <div className="grid gap-0 overflow-hidden rounded-xl border border-border-slate bg-panel-slate text-sm md:grid-cols-2 xl:grid-cols-4 2xl:grid-cols-8">
        <Strip label="Data Directory" value={storage.dataDirectory || 'N/A'} mono />
        <Strip label="Free Space" value={formatBytes(storage.freeSpace)} />
        <Strip label="Logs Directory" value={storage.logsDirectory || 'N/A'} mono />
        <Strip label="Log Size" value={formatBytes(storage.logSize)} />
        <Strip label="DB/Data Size" value={formatBytes(storage.dbSize)} />
        <Strip label="Storage" value={storage.storage || 'filesystem'} />
        <Strip label="Hardware" value={gpu?.name || 'AMD GPU (Vulkan)'} mono />
        <Strip label="Driver" value={gpu?.driverVersion || gpu?.backend || 'Vulkan'} />
      </div>
    </div>
  );
};

const StatusBlock: React.FC<{ label: string; value: string; detail?: string; tone?: string }> = ({ label, value, detail, tone }) => (
  <div className="min-w-0 border-b border-r border-border-slate/70 p-4 lg:border-b-0">
    <p className="truncate text-xs text-text-muted">{label}</p>
    <p className={`mt-2 truncate text-xl font-semibold ${tone === 'healthy' || tone === 'online' || tone === 'free' ? 'text-success-green' : tone === 'degraded' || tone === 'maintenance' ? 'text-warning-amber' : tone === 'gaming' || tone === 'locked' ? 'text-gaming-orange' : tone === 'ai' ? 'text-infer-violet' : tone === 'offline' ? 'text-danger-rose' : 'text-text-primary'}`}>{value}</p>
    {detail && <p className="mt-1 truncate text-xs text-text-secondary" title={detail}>{detail}</p>}
  </div>
);

const TaskMeter: React.FC<{ label: string; value: string; detail: string; percent: number; tone: 'cyan' | 'mint' }> = ({ label, value, detail, percent, tone }) => (
  <div className="min-w-0 rounded-lg border border-border-slate bg-deck-navy p-4">
    <div className="flex items-start justify-between gap-3">
      <div className="min-w-0">
        <p className="truncate text-xs uppercase tracking-wide text-text-secondary">{label}</p>
        <p className="mt-2 truncate text-3xl font-semibold text-text-primary">{value}</p>
      </div>
      <div className={`h-3 w-3 shrink-0 rounded-sm ${tone === 'cyan' ? 'bg-ion-cyan' : 'bg-gpu-mint'}`} />
    </div>
    <p className="mt-2 truncate text-xs text-text-secondary" title={detail}>{detail}</p>
    <div className="mt-4 h-2 overflow-hidden rounded-full bg-card-highlight">
      <div className={`h-full rounded-full ${tone === 'cyan' ? 'bg-ion-cyan' : 'bg-gpu-mint'}`} style={{ width: `${Math.max(0, Math.min(percent, 100))}%` }} />
    </div>
  </div>
);

const TaskGraph: React.FC<{ label: string; color: string; values: number[] }> = ({ label, color, values }) => {
  const padded = [...Array(Math.max(0, 40 - values.length)).fill(0), ...values].slice(-40);
  return (
    <div className="flex min-w-0 flex-col">
      <div className="mb-2 flex items-center justify-between text-xs text-text-secondary">
        <span>{label}</span>
        <span>{padded[padded.length - 1]?.toFixed(1) ?? '0.0'}%</span>
      </div>
      <div className="flex min-h-0 flex-1 items-end gap-1 border border-border-slate/70 bg-panel-slate/60 p-2">
        {padded.map((value, index) => (
          <div key={`${label}-${index}`} className="flex flex-1 items-end">
            <div className={`w-full rounded-t-sm ${color}`} style={{ height: `${Math.max(2, Math.min(value, 100))}%` }} />
          </div>
        ))}
      </div>
    </div>
  );
};

const NetworkValue: React.FC<{ value: string; onCopied: (message: string) => void }> = ({ value, onCopied }) => <span className="flex min-w-0 items-center justify-end gap-1"><span className="truncate font-mono text-[11px]" title={value}>{value}</span><CopyButton value={value} label="URL" onCopied={onCopied} /></span>;

const Info: React.FC<{ label: string; value: string; mono?: boolean; copy?: boolean; onCopied?: (message: string) => void }> = ({ label, value, mono, copy, onCopied }) => (
  <div className="min-w-0">
    <p className="text-xs text-text-muted">{label}</p>
    <div className="mt-1 flex min-w-0 items-center gap-2"><p className={`truncate text-sm text-text-primary ${mono ? 'font-mono text-xs' : ''}`} title={value}>{value}</p>{copy && <CopyButton value={value} label={label} onCopied={onCopied} />}</div>
  </div>
);

const Strip: React.FC<{ label: string; value: string; mono?: boolean }> = ({ label, value, mono }) => <div className="min-w-0 border-b border-r border-border-slate p-3"><p className="truncate text-xs text-text-muted">{label}</p><p className={`mt-1 truncate text-sm text-text-primary ${mono ? 'font-mono text-xs' : ''}`} title={value}>{value}</p></div>;

function formatTokenCount(value?: number | null): string {
  const count = Number(value ?? 0);
  if (count >= 1_000_000) return `${(count / 1_000_000).toFixed(1)}M`;
  if (count >= 1_000) return `${(count / 1_000).toFixed(1)}K`;
  return `${count}`;
}

function normalizePercent(value?: number | null): number | null {
  if (value == null || Number.isNaN(Number(value))) return null;
  return Math.max(0, Math.min(Number(value), 100));
}

function normalizeServices(services: ServiceRecord[], connected: boolean): ServiceRecord[] {
  const fallback: ServiceRecord[] = [
    { id: 'gateway', name: 'Gateway', kind: 'gateway', status: connected ? 'running' : 'offline', baseUrl: DASHBOARD_URL, lastHealthcheckAt: connected ? new Date().toISOString() : null },
    { id: 'llama-server', name: 'llama.cpp', kind: 'llama_cpp', status: 'unknown', baseUrl: LLAMA_BACKEND, lastHealthcheckAt: null },
    { id: 'comfyui', name: 'ComfyUI', kind: 'comfyui', status: 'not_configured', baseUrl: '127.0.0.1:8188' },
    { id: 'rag-worker', name: 'RAG Worker', kind: 'rag-worker', status: 'not_configured' },
    { id: 'speech', name: 'Speech', kind: 'speech', status: 'not_configured' },
  ];
  return fallback.map(item => services.find(s => s.kind === item.kind || s.id === item.id) || item);
}
