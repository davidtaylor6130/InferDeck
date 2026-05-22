import React, { useEffect, useMemo, useState } from 'react';
import {
  ArrowPathIcon,
  BoltIcon,
  BriefcaseIcon,
  ChartBarIcon,
  CheckCircleIcon,
  CircleStackIcon,
  CpuChipIcon,
  CubeIcon,
  DocumentTextIcon,
  ExclamationTriangleIcon,
  FireIcon,
  ListBulletIcon,
  MagnifyingGlassIcon,
  PlayIcon,
  PowerIcon,
  ServerStackIcon,
  StopIcon,
  XCircleIcon,
} from '@heroicons/react/24/outline';
import type { JobRecord, ModelRecord, PageProps, ServiceRecord } from '../types';
import { CommandButton } from '../components/CommandButton';
import { CopyButton } from '../components/CopyButton';
import { EmptyState } from '../components/EmptyState';
import { QueuePreview } from '../components/QueuePreview';
import { StatusBadge } from '../components/StatusBadge';
import {
  DASHBOARD_URL,
  GATEWAY_API,
  LLAMA_BACKEND,
  OPENAI_API,
  formatBytes,
  formatUptime,
  getQueueCounts,
  isOnlineStatus,
  timeAgo,
} from '../utils';

type Tone = 'good' | 'warn' | 'critical' | 'idle' | 'info';

interface GaugeMetric {
  key: string;
  label: string;
  value: string;
  detail?: string;
  percent?: number | null;
  tone: Tone;
  Icon: React.ComponentType<React.SVGProps<SVGSVGElement>>;
}

export const OverviewPage: React.FC<PageProps> = ({ state, actions }) => {
  const queue = getQueueCounts(state.statusData, state.jobsList);
  const services = normalizeServices(state.servicesList, state.connected);
  const gatewayService = services.find(service => service.kind === 'gateway' || service.id === 'gateway') || services[0];
  const llamaService = services.find(service => service.kind === 'llama_cpp' || service.id === 'llama-server');
  const llamaBackendUrl = llamaService?.baseUrl || LLAMA_BACKEND;
  const telemetry = state.statusData?.hardware || state.statusData?.telemetry || {};
  const system = telemetry.system || state.statusData?.system || {};
  const gpu = telemetry.gpu || {};
  const summary = state.statusData?.summary || {};
  const metrics = state.statusData?.metrics || {};
  const storage = state.statusData?.storage || {};
  const activeJob = state.jobsList.find(isActiveJob) || null;
  const activeModel = getActiveModel(state.runningModels, state.modelsList);
  const errors = getErrorCount(state.errors, state.jobsList, summary, metrics);
  const jobsToday = Number(summary.jobsToday ?? state.jobsList.length ?? 0);
  const failedToday = Number(summary.failedToday ?? queue.failed ?? 0);
  const totalTokens = Number(summary.totalTokens ?? metrics.total_tokens ?? sumJobTokens(state.jobsList, 'totalTokens'));
  const promptTokens = Number(summary.promptTokens ?? metrics.tokens_processed ?? sumJobTokens(state.jobsList, 'promptTokens'));
  const completionTokens = Number(summary.completionTokens ?? metrics.tokens_generated ?? sumJobTokens(state.jobsList, 'completionTokens'));
  const cpuPercent = firstPercent(system.cpuUtilization, system.cpuPercent, system.cpu, telemetry.cpu?.utilization, telemetry.cpu?.usage);
  const ramUsed = firstNumber(system.memoryUsed, system.ramUsed, telemetry.memory?.used, telemetry.ram?.used);
  const ramTotal = firstNumber(system.memoryTotal, system.ramTotal, telemetry.memory?.total, telemetry.ram?.total);
  const ramPercent = firstPercent(system.memoryPercent, system.ramPercent, percentOf(ramUsed, ramTotal));
  const diskUsed = firstNumber(storage.usedSpace, storage.diskUsed, storage.totalSpace != null && storage.freeSpace != null ? storage.totalSpace - storage.freeSpace : null);
  const diskTotal = firstNumber(storage.totalSpace, storage.diskTotal);
  const diskPercent = firstPercent(storage.usedPercent, storage.diskPercent, percentOf(diskUsed, diskTotal));
  const gpuPercent = firstPercent(gpu.utilization, gpu.usage, gpu.gpuUtilization);
  const vramUsed = firstNumber(gpu.memoryUsed, gpu.vramUsed);
  const vramTotal = firstNumber(gpu.memoryTotal, gpu.vramTotal);
  const vramPercent = firstPercent(gpu.memoryPercent, gpu.vramPercent, percentOf(vramUsed, vramTotal));
  const gpuTemperature = firstNumber(gpu.temperature, gpu.tempC, gpu.temperatureC);
  const queuePercent = queueHealthPercent(queue);
  const [history, setHistory] = useState<Array<{ timestamp: string; cpu: number; gpu: number; queue: number; vram: number }>>([]);

  useEffect(() => {
    const timestamp = telemetry.timestamp || state.lastUpdatedAt?.toISOString();
    if (!timestamp) return;
    setHistory(current => {
      if (current[current.length - 1]?.timestamp === timestamp) return current;
      return [...current, {
        timestamp,
        cpu: cpuPercent ?? 0,
        gpu: gpuPercent ?? 0,
        queue: queuePercent,
        vram: vramPercent ?? 0,
      }].slice(-48);
    });
  }, [cpuPercent, gpuPercent, queuePercent, state.lastUpdatedAt, telemetry.timestamp, vramPercent]);

  const gauges: GaugeMetric[] = [
    { key: 'cpu', label: 'CPU', value: percentLabel(cpuPercent), detail: 'processor', percent: cpuPercent, tone: threshold(cpuPercent), Icon: CpuChipIcon },
    { key: 'ram', label: 'RAM', value: percentLabel(ramPercent), detail: bytesPair(ramUsed, ramTotal), percent: ramPercent, tone: threshold(ramPercent), Icon: CircleStackIcon },
    { key: 'gpu', label: 'GPU', value: percentLabel(gpuPercent), detail: gpu.name || gpu.backend || 'Vulkan device', percent: gpuPercent, tone: threshold(gpuPercent), Icon: BoltIcon },
    { key: 'vram', label: 'VRAM', value: bytesPair(vramUsed, vramTotal), detail: `${percentLabel(vramPercent)} used`, percent: vramPercent, tone: threshold(vramPercent), Icon: CircleStackIcon },
    { key: 'temp', label: 'GPU temp', value: gpuTemperature == null ? 'N/A' : `${Math.round(gpuTemperature)} C`, detail: 'thermal', percent: tempToPercent(gpuTemperature), tone: temperatureTone(gpuTemperature), Icon: FireIcon },
    { key: 'disk', label: 'Disk', value: percentLabel(diskPercent), detail: storage.freeSpace != null ? `${formatBytes(storage.freeSpace)} free` : 'storage', percent: diskPercent, tone: threshold(diskPercent), Icon: CircleStackIcon },
  ];

  const serviceTiles = [
    {
      key: 'gateway',
      label: 'Gateway',
      value: state.connected && isOnlineStatus(gatewayService?.status) ? 'Online' : state.connected ? 'Reachable' : 'Offline',
      detail: GATEWAY_API,
      tone: state.connected ? 'good' as Tone : 'critical' as Tone,
      Icon: ServerStackIcon,
    },
    {
      key: 'llama',
      label: 'llama.cpp',
      value: llamaService && isOnlineStatus(llamaService.status) ? 'Online' : llamaService?.status === 'starting' ? 'Starting' : 'Offline',
      detail: llamaBackendUrl,
      tone: llamaService && isOnlineStatus(llamaService.status) ? 'good' as Tone : llamaService?.status === 'starting' ? 'warn' as Tone : 'critical' as Tone,
      Icon: PowerIcon,
    },
    {
      key: 'model',
      label: 'Model',
      value: compactModel(activeModel),
      detail: state.runningModels.length ? 'loaded' : `${state.modelsList.length || 0} available`,
      tone: state.runningModels.length ? 'good' as Tone : 'warn' as Tone,
      Icon: CubeIcon,
    },
    {
      key: 'errors',
      label: 'Warnings',
      value: `${errors}`,
      detail: failedToday ? `${failedToday} failed today` : 'recent logs',
      tone: errors > 0 ? (errors > 3 ? 'critical' as Tone : 'warn' as Tone) : 'good' as Tone,
      Icon: errors > 0 ? ExclamationTriangleIcon : CheckCircleIcon,
    },
  ];

  return (
    <div className="space-y-4">
      <section className="rounded-xl border border-border-slate bg-panel-slate p-4 shadow-deck">
        <div className="flex min-w-0 flex-col gap-4 xl:flex-row xl:items-center xl:justify-between">
          <div className="min-w-0">
            <div className="flex flex-wrap items-center gap-2">
              <StatusPill tone={state.connected ? 'good' : 'critical'} icon={state.connected ? CheckCircleIcon : XCircleIcon}>
                {state.connected ? 'API healthy' : 'API offline'}
              </StatusPill>
              <StatusPill tone={llamaService && isOnlineStatus(llamaService.status) ? 'good' : 'critical'} icon={PowerIcon}>
                llama.cpp {llamaService && isOnlineStatus(llamaService.status) ? 'ready' : 'down'}
              </StatusPill>
              <StatusPill tone={queue.failed ? 'critical' : queue.queued > 12 ? 'warn' : 'good'} icon={ListBulletIcon}>
                {queue.queued} queued
              </StatusPill>
              <span className="truncate text-xs text-text-muted">updated {timeAgo(state.lastUpdatedAt)}</span>
            </div>
            <div className="mt-3 flex min-w-0 flex-wrap items-end gap-x-4 gap-y-1">
              <h1 className="truncate text-2xl font-semibold text-text-primary">InferDeck Overview</h1>
              <span className="min-w-0 truncate font-mono text-sm text-ion-cyan" title={activeModel}>{activeModel}</span>
            </div>
          </div>
          <div className="flex flex-wrap gap-2">
            <IconCommand title="Start llama.cpp" onClick={() => actions.startService(llamaService?.id || 'llama-server')} tone="green" Icon={PlayIcon} />
            <IconCommand title="Stop llama.cpp" onClick={() => actions.stopService(llamaService?.id || 'llama-server')} tone="rose" Icon={StopIcon} />
            <IconCommand title="Restart llama.cpp" onClick={actions.restartBackend} tone="blue" Icon={ArrowPathIcon} />
            <IconCommand title="Rescan models" onClick={actions.rescanModels} tone="amber" Icon={MagnifyingGlassIcon} />
            <IconCommand title="View logs" onClick={() => window.location.hash = '#logs'} tone="neutral" Icon={DocumentTextIcon} />
          </div>
        </div>
      </section>

      <section className="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
        {serviceTiles.map(({ key, ...tile }) => <StateTile key={key} {...tile} />)}
      </section>

      <section className="grid gap-3 md:grid-cols-2 xl:grid-cols-3 2xl:grid-cols-6">
        {gauges.map(metric => <GaugeTile key={metric.key} metric={metric} />)}
      </section>

      <section className="grid gap-3 xl:grid-cols-[1.15fr_0.85fr]">
        <div className="rounded-xl border border-border-slate bg-panel-slate p-4 shadow-deck">
          <div className="mb-4 flex items-center justify-between gap-3">
            <IconHeading Icon={ChartBarIcon} title="Live Load" tone="info" />
            <StatusBadge label={telemetry.timestamp ? timeAgo(telemetry.timestamp) : timeAgo(state.lastUpdatedAt)} tone={state.connected ? 'running' : 'offline'} />
          </div>
          <div className="grid h-56 gap-3 sm:grid-cols-4">
            <MiniChart label="CPU" tone="good" values={history.map(item => item.cpu)} fallback={cpuPercent ?? 0} />
            <MiniChart label="GPU" tone="info" values={history.map(item => item.gpu)} fallback={gpuPercent ?? 0} />
            <MiniChart label="VRAM" tone="warn" values={history.map(item => item.vram)} fallback={vramPercent ?? 0} />
            <MiniChart label="Queue" tone={queue.failed ? 'critical' : 'idle'} values={history.map(item => item.queue)} fallback={queuePercent} />
          </div>
        </div>

        <div className="rounded-xl border border-border-slate bg-panel-slate p-4 shadow-deck">
          <div className="mb-4 flex items-center justify-between gap-3">
            <IconHeading Icon={ListBulletIcon} title="Queue" tone={queue.failed ? 'critical' : queue.queued > 12 ? 'warn' : 'good'} />
            <CommandButton className="min-h-8 px-3 py-1 text-xs" onClick={() => window.location.hash = '#queue'}>Open</CommandButton>
          </div>
          <div className="grid grid-cols-4 gap-2">
            <CountTile Icon={ListBulletIcon} label="Queued" value={queue.queued} tone={queue.queued > 12 ? 'warn' : 'good'} />
            <CountTile Icon={BriefcaseIcon} label="Running" value={queue.running} tone={queue.running ? 'info' : 'idle'} />
            <CountTile Icon={XCircleIcon} label="Failed" value={queue.failed} tone={queue.failed ? 'critical' : 'good'} />
            <CountTile Icon={PowerIcon} label="Lock" value={queue.gpuLocked ? 'On' : 'Free'} tone={queue.gpuLocked ? 'warn' : 'good'} />
          </div>
          <div className="mt-4 min-h-32">
            {state.jobsList.length ? <QueuePreview jobs={state.jobsList.slice(0, 5)} /> : <EmptyState title="Queue clear" description="No waiting or running work." />}
          </div>
        </div>
      </section>

      <section className="grid gap-3 xl:grid-cols-[0.9fr_1.1fr_1fr]">
        <div className="rounded-xl border border-border-slate bg-panel-slate p-4 shadow-deck">
          <div className="mb-4 flex items-center justify-between gap-3">
            <IconHeading Icon={BriefcaseIcon} title="Jobs" tone={failedToday ? 'critical' : 'good'} />
            {activeJob && <StatusBadge label="Running" tone="running" />}
          </div>
          <div className="grid grid-cols-3 gap-2">
            <CountTile Icon={BriefcaseIcon} label="Today" value={jobsToday} tone="info" />
            <CountTile Icon={CheckCircleIcon} label="Done" value={summary.succeededToday ?? state.jobsList.filter(job => job.status === 'succeeded').length} tone="good" />
            <CountTile Icon={XCircleIcon} label="Failed" value={failedToday} tone={failedToday ? 'critical' : 'good'} />
          </div>
          <div className="mt-4 rounded-lg border border-border-slate bg-deck-navy p-3">
            {activeJob ? <ActiveJob job={activeJob} activeModel={activeModel} onCancel={actions.cancelJob} onCopied={actions.toast} /> : <div className="flex items-center gap-3 text-sm text-text-secondary"><PowerIcon className="h-5 w-5 text-success-green" /> GPU lease is free</div>}
          </div>
        </div>

        <div className="rounded-xl border border-border-slate bg-panel-slate p-4 shadow-deck">
          <div className="mb-4 flex items-center justify-between gap-3">
            <IconHeading Icon={CubeIcon} title="Model Control" tone={state.runningModels.length ? 'good' : 'warn'} />
            <CommandButton className="min-h-8 px-3 py-1 text-xs" onClick={() => window.location.hash = '#models'}>Models</CommandButton>
          </div>
          <div className="mb-3 min-w-0 rounded-lg border border-border-slate bg-deck-navy p-3">
            <p className="truncate font-mono text-sm text-text-primary" title={activeModel}>{activeModel}</p>
            <p className="mt-1 truncate text-xs text-text-muted">{state.runningModels.length ? 'loaded in llama.cpp' : `${state.modelsList.length || 0} local model candidates`}</p>
          </div>
          <div className="grid gap-2 sm:grid-cols-2">
            <ModelPicker models={state.modelsList} activeModel={activeModel} onLoad={actions.loadModel} />
            <CommandButton onClick={actions.unloadModels}>Unload all</CommandButton>
          </div>
        </div>

        <div className="rounded-xl border border-border-slate bg-panel-slate p-4 shadow-deck">
          <div className="mb-4 flex items-center justify-between gap-3">
            <IconHeading Icon={DocumentTextIcon} title="Tokens" tone="info" />
            <span className="text-xs text-text-muted">{formatUptime(state.healthData?.uptime)}</span>
          </div>
          <div className="grid grid-cols-3 gap-2">
            <CountTile Icon={DocumentTextIcon} label="Total" value={formatTokenCount(totalTokens)} tone="info" />
            <CountTile Icon={DocumentTextIcon} label="Prompt" value={formatTokenCount(promptTokens)} tone="idle" />
            <CountTile Icon={DocumentTextIcon} label="Output" value={formatTokenCount(completionTokens)} tone="good" />
          </div>
          <div className="mt-4 grid gap-2 text-xs text-text-secondary">
            <Endpoint value={DASHBOARD_URL} label="UI" onCopied={actions.toast} />
            <Endpoint value={OPENAI_API} label="OpenAI" onCopied={actions.toast} />
            <Endpoint value={llamaBackendUrl} label="llama" onCopied={actions.toast} />
          </div>
        </div>
      </section>
    </div>
  );
};

const StateTile: React.FC<{ label: string; value: string; detail?: string; tone: Tone; Icon: React.ComponentType<React.SVGProps<SVGSVGElement>> }> = ({ label, value, detail, tone, Icon }) => (
  <div className="min-w-0 rounded-xl border border-border-slate bg-panel-slate p-4 shadow-deck">
    <div className="flex items-center justify-between gap-3">
      <IconBadge Icon={Icon} tone={tone} />
      <StatusDot tone={tone} />
    </div>
    <p className="mt-4 truncate text-xs uppercase text-text-muted">{label}</p>
    <p className={`mt-1 truncate text-2xl font-semibold ${toneText(tone)}`} title={value}>{value}</p>
    {detail && <p className="mt-1 truncate font-mono text-xs text-text-secondary" title={detail}>{detail}</p>}
  </div>
);

const GaugeTile: React.FC<{ metric: GaugeMetric }> = ({ metric }) => (
  <div className="min-w-0 rounded-xl border border-border-slate bg-panel-slate p-4 shadow-deck">
    <div className="mb-3 flex items-center justify-between gap-3">
      <IconBadge Icon={metric.Icon} tone={metric.tone} />
      <span className={`text-2xl font-semibold ${toneText(metric.tone)}`}>{metric.value}</span>
    </div>
    <div className="flex items-end justify-between gap-3">
      <div className="min-w-0">
        <p className="truncate text-sm font-medium text-text-primary">{metric.label}</p>
        {metric.detail && <p className="truncate text-xs text-text-secondary" title={metric.detail}>{metric.detail}</p>}
      </div>
      <Ring percent={metric.percent ?? 0} tone={metric.tone} />
    </div>
  </div>
);

const Ring: React.FC<{ percent: number; tone: Tone }> = ({ percent, tone }) => {
  const safe = Math.max(0, Math.min(percent, 100));
  return (
    <div
      className="grid h-12 w-12 shrink-0 place-items-center rounded-full"
      style={{ background: `conic-gradient(${toneHex(tone)} ${safe * 3.6}deg, rgba(100,116,139,0.22) 0deg)` }}
    >
      <div className="h-8 w-8 rounded-full bg-panel-slate" />
    </div>
  );
};

const MiniChart: React.FC<{ label: string; values: number[]; fallback: number; tone: Tone }> = ({ label, values, fallback, tone }) => {
  const padded = [...Array(Math.max(0, 32 - values.length)).fill(0), ...(values.length ? values : [fallback])].slice(-32);
  return (
    <div className="flex min-w-0 flex-col rounded-lg border border-border-slate bg-deck-navy p-3">
      <div className="mb-2 flex items-center justify-between gap-2 text-xs">
        <span className="truncate text-text-secondary">{label}</span>
        <span className={toneText(threshold(padded[padded.length - 1]))}>{Math.round(padded[padded.length - 1])}%</span>
      </div>
      <div className="flex min-h-0 flex-1 items-end gap-1">
        {padded.map((value, index) => (
          <div key={`${label}-${index}`} className="flex flex-1 items-end">
            <div className="w-full rounded-t-sm" style={{ height: `${Math.max(3, Math.min(value, 100))}%`, background: toneHex(tone) }} />
          </div>
        ))}
      </div>
    </div>
  );
};

const CountTile: React.FC<{ Icon: React.ComponentType<React.SVGProps<SVGSVGElement>>; label: string; value: React.ReactNode; tone: Tone }> = ({ Icon, label, value, tone }) => (
  <div className="min-w-0 rounded-lg border border-border-slate bg-deck-navy p-3">
    <Icon className={`h-5 w-5 ${toneText(tone)}`} />
    <p className="mt-2 truncate text-[11px] uppercase text-text-muted">{label}</p>
    <p className={`mt-1 truncate text-xl font-semibold ${toneText(tone)}`} title={String(value)}>{value}</p>
  </div>
);

const StatusPill: React.FC<{ tone: Tone; icon: React.ComponentType<React.SVGProps<SVGSVGElement>>; children: React.ReactNode }> = ({ tone, icon: Icon, children }) => (
  <span className={`inline-flex min-h-7 items-center gap-1.5 rounded-full border px-2.5 text-xs ${toneBorder(tone)} ${toneText(tone)}`}>
    <Icon className="h-4 w-4" />
    <span className="truncate">{children}</span>
  </span>
);

const IconCommand: React.FC<{ title: string; onClick: () => void; tone: 'green' | 'rose' | 'blue' | 'amber' | 'neutral'; Icon: React.ComponentType<React.SVGProps<SVGSVGElement>> }> = ({ title, onClick, tone, Icon }) => (
  <CommandButton aria-label={title} title={title} onClick={onClick} tone={tone} className="h-10 w-10 px-0">
    <Icon className="h-5 w-5" />
  </CommandButton>
);

const IconHeading: React.FC<{ Icon: React.ComponentType<React.SVGProps<SVGSVGElement>>; title: string; tone: Tone }> = ({ Icon, title, tone }) => (
  <div className="flex min-w-0 items-center gap-2">
    <IconBadge Icon={Icon} tone={tone} small />
    <h2 className="truncate text-base font-semibold text-text-primary">{title}</h2>
  </div>
);

const IconBadge: React.FC<{ Icon: React.ComponentType<React.SVGProps<SVGSVGElement>>; tone: Tone; small?: boolean }> = ({ Icon, tone, small }) => (
  <div className={`grid shrink-0 place-items-center rounded-lg border ${small ? 'h-8 w-8' : 'h-10 w-10'} ${toneBorder(tone)} ${toneText(tone)}`}>
    <Icon className={small ? 'h-4 w-4' : 'h-5 w-5'} />
  </div>
);

const StatusDot: React.FC<{ tone: Tone }> = ({ tone }) => <span className="h-2.5 w-2.5 rounded-full" style={{ background: toneHex(tone) }} />;

const ActiveJob: React.FC<{ job: JobRecord; activeModel: string; onCancel: (id: string) => void; onCopied: (message: string) => void }> = ({ job, activeModel, onCancel, onCopied }) => (
  <div className="min-w-0">
    <div className="flex min-w-0 items-center justify-between gap-3">
      <div className="min-w-0">
        <p className="truncate font-mono text-xs text-text-primary" title={job.id}>{job.id}</p>
        <p className="mt-1 truncate text-xs text-text-secondary">{job.type} / {job.model || activeModel}</p>
      </div>
      <CopyButton value={job.id} label="Job" onCopied={onCopied} />
    </div>
    <div className="mt-3 flex items-center justify-between gap-3">
      <span className="truncate text-xs text-text-muted">{job.client || job.resourceClass || job.resource_class || 'gpu_llm'}</span>
      <CommandButton tone="rose" className="min-h-8 px-3 py-1 text-xs" onClick={() => onCancel(job.id)}>Cancel</CommandButton>
    </div>
  </div>
);

const ModelPicker: React.FC<{ models: ModelRecord[]; activeModel: string; onLoad: (model: string) => void }> = ({ models, activeModel, onLoad }) => (
  <select
    className="min-h-10 min-w-0 rounded-lg border border-border-slate bg-deck-navy px-3 py-2 text-sm text-text-primary"
    value={models.some(model => model.name === activeModel) ? activeModel : ''}
    onChange={event => event.target.value && onLoad(event.target.value)}
    aria-label="Load model"
  >
    <option value="">Load model</option>
    {models.map(model => <option key={model.name} value={model.name}>{model.name}</option>)}
  </select>
);

const Endpoint: React.FC<{ label: string; value: string; onCopied: (message: string) => void }> = ({ label, value, onCopied }) => (
  <div className="flex min-w-0 items-center justify-between gap-2 rounded-lg border border-border-slate bg-deck-navy px-3 py-2">
    <span className="shrink-0 text-text-muted">{label}</span>
    <span className="min-w-0 truncate font-mono text-text-primary" title={value}>{value}</span>
    <CopyButton value={value} label={label} onCopied={onCopied} />
  </div>
);

function normalizeServices(services: ServiceRecord[], connected: boolean): ServiceRecord[] {
  const fallback: ServiceRecord[] = [
    { id: 'gateway', name: 'Gateway', kind: 'gateway', status: connected ? 'running' : 'offline', baseUrl: DASHBOARD_URL, lastHealthcheckAt: connected ? new Date().toISOString() : null },
    { id: 'llama-server', name: 'llama.cpp', kind: 'llama_cpp', status: 'offline', baseUrl: LLAMA_BACKEND, lastHealthcheckAt: null },
  ];
  return fallback.map(item => services.find(service => service.kind === item.kind || service.id === item.id) || item);
}

function isActiveJob(job: JobRecord): boolean {
  return job.status === 'running' || job.status === 'leased';
}

function getActiveModel(running: ModelRecord[], models: ModelRecord[]): string {
  return running[0]?.name || models.find(model => model.loaded)?.name || models[0]?.name || 'No model loaded';
}

function compactModel(model: string): string {
  if (model === 'No model loaded') return 'None';
  const file = model.split(/[\\/]/).pop() || model;
  return file.length > 26 ? `${file.slice(0, 23)}...` : file;
}

function threshold(value?: number | null): Tone {
  if (value == null) return 'idle';
  if (value >= 95) return 'critical';
  if (value >= 80) return 'warn';
  return 'good';
}

function temperatureTone(value?: number | null): Tone {
  if (value == null) return 'idle';
  if (value >= 90) return 'critical';
  if (value >= 78) return 'warn';
  return 'good';
}

function queueHealthPercent(queue: ReturnType<typeof getQueueCounts>): number {
  return Math.max(0, Math.min(100, queue.failed * 25 + queue.queued * 5 + queue.running * 15));
}

function firstNumber(...values: unknown[]): number | null {
  for (const value of values) {
    if (value == null || value === '') continue;
    const number = Number(value);
    if (Number.isFinite(number)) return number;
  }
  return null;
}

function firstPercent(...values: unknown[]): number | null {
  const number = firstNumber(...values);
  if (number == null) return null;
  return Math.max(0, Math.min(number, 100));
}

function percentOf(used?: number | null, total?: number | null): number | null {
  if (used == null || total == null || total <= 0) return null;
  return (used / total) * 100;
}

function tempToPercent(value?: number | null): number | null {
  if (value == null) return null;
  return Math.max(0, Math.min(100, (value / 100) * 100));
}

function percentLabel(value?: number | null): string {
  return value == null ? 'N/A' : `${Math.round(value)}%`;
}

function bytesPair(used?: number | null, total?: number | null): string {
  if (used == null || total == null) return 'N/A';
  return `${formatBytes(used)} / ${formatBytes(total)}`;
}

function formatTokenCount(value?: number | null): string {
  const count = Number(value ?? 0);
  if (count >= 1_000_000) return `${(count / 1_000_000).toFixed(1)}M`;
  if (count >= 1_000) return `${(count / 1_000).toFixed(1)}K`;
  return `${count}`;
}

function sumJobTokens(jobs: JobRecord[], key: 'totalTokens' | 'promptTokens' | 'completionTokens'): number {
  return jobs.reduce((sum, job) => sum + Number(job[key] ?? 0), 0);
}

function getErrorCount(errors: Record<string, string | null>, jobs: JobRecord[], summary: Record<string, any>, metrics: Record<string, any>): number {
  const apiErrors = Object.values(errors).filter(Boolean).length;
  const failedJobs = Number(summary.failedToday ?? jobs.filter(job => job.status === 'failed' || job.status === 'dead_letter').length ?? 0);
  const logWarnings = Number(summary.warningCount ?? summary.errorCount ?? metrics.warning_count ?? metrics.error_count ?? 0);
  return apiErrors + failedJobs + logWarnings;
}

function toneText(tone: Tone): string {
  if (tone === 'good') return 'text-success-green';
  if (tone === 'warn') return 'text-warning-amber';
  if (tone === 'critical') return 'text-danger-rose';
  if (tone === 'info') return 'text-ion-cyan';
  return 'text-text-secondary';
}

function toneBorder(tone: Tone): string {
  if (tone === 'good') return 'border-success-green/50 bg-success-green/10';
  if (tone === 'warn') return 'border-warning-amber/50 bg-warning-amber/10';
  if (tone === 'critical') return 'border-danger-rose/50 bg-danger-rose/10';
  if (tone === 'info') return 'border-ion-cyan/50 bg-ion-cyan/10';
  return 'border-border-slate bg-card-highlight/50';
}

function toneHex(tone: Tone): string {
  if (tone === 'good') return '#22C55E';
  if (tone === 'warn') return '#F59E0B';
  if (tone === 'critical') return '#F43F5E';
  if (tone === 'info') return '#22D3EE';
  return '#64748B';
}
