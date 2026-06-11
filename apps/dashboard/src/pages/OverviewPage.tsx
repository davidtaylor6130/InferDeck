import React, { useEffect, useMemo, useState } from 'react';
import {
  ArrowPathIcon,
  BriefcaseIcon,
  ChartBarIcon,
  CheckCircleIcon,
  ClockIcon,
  CpuChipIcon,
  CubeIcon,
  DocumentTextIcon,
  GlobeAltIcon,
  ListBulletIcon,
  MegaphoneIcon,
  MicrophoneIcon,
  PauseIcon,
  PencilSquareIcon,
  QueueListIcon,
  ServerStackIcon,
  SparklesIcon,
  Squares2X2Icon,
} from '@heroicons/react/24/outline';
import type { JobRecord, ModelRecord, PageProps, ServiceRecord } from '../types';
import { CommandButton } from '../components/CommandButton';
import { StatusBadge } from '../components/StatusBadge';
import { formatBytes, formatUptime, getQueueCounts, isOnlineStatus, timeAgo } from '../utils';

type Tone = 'good' | 'warn' | 'critical' | 'idle' | 'info' | 'violet';

interface MetricDatum {
  label: string;
  value: number;
  tone: Tone;
}

interface ServiceDatum {
  key: string;
  label: string;
  status: string;
  tone: Tone;
  Icon: React.ComponentType<React.SVGProps<SVGSVGElement>>;
}

interface ModelCostConfig {
  equivalentModel: string;
  promptPerMillion: number;
  outputPerMillion: number;
  breakEvenTarget: number;
  source?: string;
  defaultsVersion?: number;
  userEdited?: boolean;
}

const DEFAULT_BREAK_EVEN_TARGET = 1739;
const MODEL_COST_DEFAULTS_VERSION = 4;

interface ModelTokenUsage {
  model: string;
  prompt: number;
  output: number;
  total: number;
}

interface PersistedUsage {
  model: string;
  requests?: number;
  successfulRequests?: number;
  promptTokens?: number;
  completionTokens?: number;
  totalTokens?: number;
  prompt_tokens?: number;
  completion_tokens?: number;
  total_tokens?: number;
}

interface PersistedUsageBucket {
  bucket: string;
  model: string;
  promptTokens?: number;
  completionTokens?: number;
  totalTokens?: number;
  prompt_tokens?: number;
  completion_tokens?: number;
  total_tokens?: number;
  requests?: number;
  successfulRequests?: number;
}

type TokenRange = 'week' | 'month' | 'year' | 'all';

const DEFAULT_COST_CONFIG: ModelCostConfig = {
  equivalentModel: 'OpenRouter median tracked chat model',
  promptPerMillion: 0.55,
  outputPerMillion: 1.44,
  breakEvenTarget: DEFAULT_BREAK_EVEN_TARGET,
  source: 'OpenRouter median chat model pricing',
  defaultsVersion: MODEL_COST_DEFAULTS_VERSION,
};

const MODEL_COST_DEFAULTS: Record<string, ModelCostConfig> = {
  'qwen3.6-27b': { equivalentModel: 'Qwen3 VL 32B / Qwen3 32B class', promptPerMillion: 0.104, outputPerMillion: 0.416, breakEvenTarget: DEFAULT_BREAK_EVEN_TARGET, source: 'Qwen3 27B open-weight equivalent: Qwen3 VL 32B / Qwen3 32B public pricing', defaultsVersion: MODEL_COST_DEFAULTS_VERSION },
  'qwen3.6-35b-a3b': { equivalentModel: 'Qwen3.6 35B A3B', promptPerMillion: 0.14, outputPerMillion: 0.90, breakEvenTarget: DEFAULT_BREAK_EVEN_TARGET, source: 'PricePerToken Qwen3.6 35B A3B', defaultsVersion: MODEL_COST_DEFAULTS_VERSION },
  'qwen3-coder-30b-a3b': { equivalentModel: 'Qwen3 Coder 30B A3B Instruct', promptPerMillion: 0.07, outputPerMillion: 0.27, breakEvenTarget: DEFAULT_BREAK_EVEN_TARGET, source: 'OpenRouter Qwen3 Coder 30B A3B Instruct', defaultsVersion: MODEL_COST_DEFAULTS_VERSION },
  'qwen3-coder-next': { equivalentModel: 'Qwen3 Coder Next', promptPerMillion: 0.11, outputPerMillion: 0.80, breakEvenTarget: DEFAULT_BREAK_EVEN_TARGET, source: 'OpenRouter Qwen3 Coder Next', defaultsVersion: MODEL_COST_DEFAULTS_VERSION },
  'qwen2.5-coder-3b': { equivalentModel: 'Qwen2.5 Coder / small coder API average', promptPerMillion: 0.08, outputPerMillion: 0.28, breakEvenTarget: DEFAULT_BREAK_EVEN_TARGET, source: 'Qwen 30B low-cost public provider average fallback', defaultsVersion: MODEL_COST_DEFAULTS_VERSION },
  'gemma-4-26b-a4b': { equivalentModel: 'Gemma 3 27B Instruct', promptPerMillion: 0.08, outputPerMillion: 0.16, breakEvenTarget: DEFAULT_BREAK_EVEN_TARGET, source: 'OpenRouter / Puter Gemma 3 27B', defaultsVersion: MODEL_COST_DEFAULTS_VERSION },
  'gpt-oss-20b': { equivalentModel: 'GPT-OSS 20B', promptPerMillion: 0.05, outputPerMillion: 0.20, breakEvenTarget: DEFAULT_BREAK_EVEN_TARGET, source: 'Puter GPT-OSS 20B', defaultsVersion: MODEL_COST_DEFAULTS_VERSION },
};

const LEGACY_DEFAULT_PRICES: Record<string, Array<[number, number]>> = {
  'qwen3.6-27b': [[0.455, 1.82], [0.325, 1.95]],
  'qwen3.6-35b-a3b': [[0.455, 1.82], [0.325, 1.95], [0.129, 0.512]],
  'qwen3-coder-30b-a3b': [[0.455, 1.82]],
  'qwen2.5-coder-3b': [[0.30, 0.30]],
  'gemma-4-26b-a4b': [[0.10, 0.30]],
  'gpt-oss-20b': [[0.05, 0.20]],
};

const ALL_MODELS = 'All tracked models';
const COST_STORAGE_KEY = 'inferdeck:model-token-costs';
const TOKEN_RANGE_LABELS: Record<TokenRange, string> = {
  week: 'Week',
  month: 'Month',
  year: 'Year',
  all: 'All time',
};

export const OverviewPage: React.FC<PageProps> = ({ state, actions }) => {
  const queue = getQueueCounts(state.statusData, state.jobsList);
  const services = normalizeServices(state.servicesList, state.connected);
  const gatewayService = services.find(service => service.kind === 'gateway' || service.id === 'gateway');
  const llamaService = services.find(service => service.kind === 'llama_cpp' || service.id === 'llama-server');
  const whisperService = services.find(service => service.kind === 'whisper_cpp' || service.id === 'whisper');
  const telemetry = state.statusData?.hardware || state.statusData?.telemetry || {};
  const system = telemetry.system || state.statusData?.system || {};
  const gpu = telemetry.gpu || {};
  const summary = state.statusData?.summary || {};
  const metrics = state.statusData?.metrics || {};
  const whisper = state.statusData?.whisper || {};
  const activeModel = getActiveModel(state.runningModels, state.modelsList);
  const activeModelRecord = getActiveModelRecord(state.runningModels, state.modelsList);
  const totalTokens = Number(summary.totalTokens ?? metrics.total_tokens ?? sumJobTokens(state.jobsList, 'totalTokens'));
  const promptTokens = Number(summary.promptTokens ?? metrics.tokens_processed ?? sumJobTokens(state.jobsList, 'promptTokens'));
  const outputTokens = Number(summary.completionTokens ?? metrics.tokens_generated ?? sumJobTokens(state.jobsList, 'completionTokens'));
  const persistedUsage = Array.isArray(state.statusData?.tokenUsage) ? state.statusData.tokenUsage as PersistedUsage[] : [];
  const monthlyUsage = Array.isArray(state.statusData?.monthlyTokenUsage) ? state.statusData.monthlyTokenUsage as PersistedUsageBucket[] : [];
  const cpuPercent = firstPercent(system.cpuUtilization, system.cpuPercent, system.cpu, telemetry.cpu?.utilization, telemetry.cpu?.usage);
  const cpuTemp = firstNumber(system.temperature, system.cpuTemp, system.cpuTemperature, telemetry.cpu?.temperature, telemetry.cpu?.tempC);
  const ramUsed = firstNumber(system.memoryUsed, system.ramUsed, telemetry.memory?.used, telemetry.ram?.used);
  const ramTotal = firstNumber(system.memoryTotal, system.ramTotal, telemetry.memory?.total, telemetry.ram?.total);
  const ramPercent = firstPercent(system.memoryPercent, system.ramPercent, percentOf(ramUsed, ramTotal));
  const gpuPercent = firstPercent(gpu.utilization, gpu.usage, gpu.gpuUtilization);
  const vramUsed = firstNumber(gpu.memoryUsed, gpu.vramUsed);
  const vramTotal = firstNumber(gpu.memoryTotal, gpu.vramTotal);
  const vramPercent = firstPercent(gpu.memoryPercent, gpu.vramPercent, percentOf(vramUsed, vramTotal));
  const gpuTemp = firstNumber(gpu.temperature, gpu.tempC, gpu.temperatureC);
  const healthTone: Tone = state.connected && !getErrorCount(state.errors, state.jobsList, summary, metrics) ? 'good' : state.connected ? 'warn' : 'critical';
  const queueTone: Tone = queue.failed ? 'critical' : queue.queued > 0 ? 'info' : 'idle';
  const modelNames = useMemo(() => getModelNames(state.jobsList, state.modelsList, activeModel, persistedUsage), [activeModel, persistedUsage, state.jobsList, state.modelsList]);
  const [selectedCostModel, setSelectedCostModel] = useState(activeModel);
  const [tokenRange, setTokenRange] = useState<TokenRange>('all');
  const [costConfig, setCostConfig] = useState<Record<string, ModelCostConfig>>(() => loadCostConfig());
  useEffect(() => {
    if (!modelNames.includes(selectedCostModel)) setSelectedCostModel(modelNames[0] || activeModel);
  }, [activeModel, modelNames, selectedCostModel]);
  const selectedModelCost = getCostConfigForModel(selectedCostModel, costConfig);
  const tokenSeries = useMemo(
    () => buildTokenSeries(state.jobsList, selectedCostModel, selectedModelCost, monthlyUsage, costConfig, tokenRange),
    [costConfig, monthlyUsage, selectedCostModel, selectedModelCost, state.jobsList, tokenRange],
  );
  const modelUsage = useMemo(
    () => tokenUsageFromSeries(selectedCostModel, tokenSeries),
    [selectedCostModel, tokenSeries],
  );
  const portfolioCost = getCostConfigForModel(ALL_MODELS, costConfig);
  const portfolioCostAvoided = persistedUsage.length
    ? estimatePortfolioCostAvoided(persistedUsage, costConfig)
    : estimateJobsPortfolioCostAvoided(state.jobsList, costConfig, { prompt: promptTokens, output: outputTokens, total: totalTokens });
  const roiRemaining = Math.max(0, portfolioCost.breakEvenTarget - portfolioCostAvoided);
  const roiProgress = portfolioCost.breakEvenTarget > 0
    ? Math.min(100, (portfolioCostAvoided / portfolioCost.breakEvenTarget) * 100)
    : 0;
  const activity = buildActivity(state.jobsList, state.statusData?.observability, state.lastUpdatedAt);
  const [history, setHistory] = useState<Array<{ timestamp: string; cpu: number; ram: number; gpu: number; vram: number }>>([]);

  useEffect(() => {
    const timestamp = telemetry.timestamp || state.lastUpdatedAt?.toISOString();
    if (!timestamp) return;
    setHistory(current => {
      if (current[current.length - 1]?.timestamp === timestamp) return current;
      return [...current, {
        timestamp,
        cpu: cpuPercent ?? 0,
        ram: ramPercent ?? 0,
        gpu: gpuPercent ?? 0,
        vram: vramPercent ?? 0,
      }].slice(-60);
    });
  }, [cpuPercent, gpuPercent, ramPercent, state.lastUpdatedAt, telemetry.timestamp, vramPercent]);

  const backendHistory = useMemo(() => getHardwareHistory(state.statusData?.hardwareSamples), [state.statusData?.hardwareSamples]);
  const liveHistory = useMemo(() => fillHistory(backendHistory.length ? backendHistory : history), [backendHistory, history]);

  const servicesMini: ServiceDatum[] = [
    { key: 'gateway', label: 'API / Gateway', status: state.connected && isOnlineStatus(gatewayService?.status) ? 'Online' : 'Offline', tone: state.connected ? 'good' : 'critical', Icon: GlobeAltIcon },
    { key: 'llama', label: 'llama.cpp', status: isOnlineStatus(llamaService?.status) ? 'Online' : 'Offline', tone: isOnlineStatus(llamaService?.status) ? 'good' : 'critical', Icon: PencilSquareIcon },
    { key: 'whisper', label: 'Whisper', status: isOnlineStatus(whisperService?.status) ? 'Ready' : whisperService?.status === 'not_configured' ? 'Config' : 'Offline', tone: isOnlineStatus(whisperService?.status) ? 'good' : whisperService?.status === 'not_configured' ? 'warn' : 'critical', Icon: MicrophoneIcon },
    { key: 'training', label: 'Training', status: 'Idle', tone: 'idle', Icon: ChartBarIcon },
    { key: 'tts', label: 'TTS', status: 'Idle', tone: 'idle', Icon: MegaphoneIcon },
    { key: 'image', label: 'Image', status: 'Idle', tone: 'idle', Icon: SparklesIcon },
    { key: 'jobs', label: 'Jobs', status: queue.running ? `${queue.running} running` : 'Idle', tone: queue.running ? 'info' : 'idle', Icon: BriefcaseIcon },
    { key: 'queue', label: 'Queue', status: `${queue.queued} queued`, tone: queueTone, Icon: ListBulletIcon },
  ];

  return (
    <div className="mx-auto max-w-[1780px] space-y-4">
      <section className="flex min-w-0 flex-col gap-4 xl:flex-row xl:items-start xl:justify-between">
        <div className="min-w-0">
          <h1 className="truncate text-2xl font-semibold text-text-primary md:text-3xl">InferDeck Overview</h1>
          <p className="mt-1 truncate text-sm text-text-secondary">Local AI throughput, system load, and backend health at a glance.</p>
        </div>
        <div className="flex flex-wrap gap-2">
          <CommandButton tone="blue" onClick={actions.pauseQueue} className="min-h-9 px-3 py-1.5 text-xs"><PauseIcon className="h-4 w-4" />Pause Queue</CommandButton>
          <CommandButton tone="neutral" onClick={actions.restartBackend} className="min-h-9 px-3 py-1.5 text-xs"><ArrowPathIcon className="h-4 w-4" />Refresh Runtime</CommandButton>
          <CommandButton tone="neutral" onClick={actions.refreshAll} className="min-h-9 px-3 py-1.5 text-xs"><ArrowPathIcon className="h-4 w-4" />Refresh</CommandButton>
        </div>
      </section>

      <section className="flex flex-wrap gap-2">
        <StatusPill label="Current Mode" value="AI Mode" tone="violet" Icon={Squares2X2Icon} />
        <StatusPill label="Health" value={healthTone === 'good' ? 'Healthy' : healthTone === 'warn' ? 'Degraded' : 'Offline'} tone={healthTone} Icon={CheckCircleIcon} />
        <StatusPill label="API / Gateway" value={state.connected ? 'Online' : 'Offline'} tone={state.connected ? 'good' : 'critical'} Icon={GlobeAltIcon} />
        <StatusPill label="llama.cpp" value={isOnlineStatus(llamaService?.status) ? 'Online' : 'Offline'} tone={isOnlineStatus(llamaService?.status) ? 'good' : 'critical'} Icon={ServerStackIcon} />
        <StatusPill label="Whisper" value={isOnlineStatus(whisperService?.status) ? 'Ready' : 'Config'} tone={isOnlineStatus(whisperService?.status) ? 'good' : 'warn'} Icon={MicrophoneIcon} />
        <StatusPill label="Queue" value={`${queue.queued} queued`} tone={queueTone} Icon={QueueListIcon} />
        <StatusPill label="Updated" value={timeAgo(state.lastUpdatedAt)} tone="idle" Icon={ClockIcon} />
      </section>

      <section className="grid gap-4 xl:grid-cols-2">
        <UsagePanel title="CPU & System" metrics={[
          { label: 'CPU Usage', value: cpuPercent ?? 0, tone: threshold(cpuPercent) },
          { label: 'System RAM', value: ramPercent ?? 0, tone: threshold(ramPercent) },
          { label: 'CPU Temp', value: tempToPercent(cpuTemp), tone: temperatureTone(cpuTemp) },
        ]} details={[cpuDetail(system), bytesPair(ramUsed, ramTotal), tempLabel(cpuTemp, telemetry.cpu?.temperatureReason)]} />
        <UsagePanel title="GPU & VRAM" metrics={[
          { label: 'GPU Utilization', value: gpuPercent ?? 0, tone: threshold(gpuPercent) },
          { label: 'VRAM Usage', value: vramPercent ?? 0, tone: threshold(vramPercent) },
          { label: 'GPU Temp', value: tempToPercent(gpuTemp), tone: temperatureTone(gpuTemp) },
        ]} details={[gpu.name || gpu.backend || 'Vulkan device', bytesPair(vramUsed, vramTotal), tempLabel(gpuTemp, gpu.temperatureReason)]} />
      </section>

      <section className="grid gap-3 sm:grid-cols-2 xl:grid-cols-4 2xl:grid-cols-8">
        {servicesMini.map(service => <MiniServiceCard key={service.key} service={service} />)}
      </section>

      <section className="grid gap-4 xl:grid-cols-[0.98fr_1.02fr]">
        <Panel className="min-h-[278px]">
          <SectionTitle title="Live Load" aside="Last 60 seconds" />
          <div className="mt-4 grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
            <MiniLineChart label="CPU %" value={cpuPercent ?? 0} values={liveHistory.map(item => item.cpu)} tone="info" />
            <MiniLineChart label="RAM %" value={ramPercent ?? 0} values={liveHistory.map(item => item.ram)} tone="good" />
            <MiniLineChart label="GPU %" value={gpuPercent ?? 0} values={liveHistory.map(item => item.gpu)} tone="violet" />
            <MiniLineChart label="VRAM %" value={vramPercent ?? 0} values={liveHistory.map(item => item.vram)} tone="violet" />
          </div>
        </Panel>

        <Panel className="min-h-[278px]">
          <div className="flex flex-col gap-3 min-[1800px]:flex-row min-[1800px]:items-start min-[1800px]:justify-between">
            <SectionTitle title="Token Usage & Cost" aside={TOKEN_RANGE_LABELS[tokenRange]} />
            <div className="grid grid-cols-2 gap-x-6 gap-y-2 text-sm sm:grid-cols-5">
              <SummaryStat label="Total Tokens" value={formatTokenCount(modelUsage.total)} />
              <SummaryStat label="Prompt" value={formatTokenCount(modelUsage.prompt)} />
              <SummaryStat label="Output" value={formatTokenCount(modelUsage.output)} />
              <SummaryStat label="Portfolio Cost Avoided" value={formatCurrency(portfolioCostAvoided)} tone="good" />
              <SummaryStat label="ROI Remaining" value={portfolioCost.breakEvenTarget > 0 ? formatCurrency(roiRemaining) : 'Set target'} tone={roiRemaining === 0 && portfolioCost.breakEvenTarget > 0 ? 'good' : 'warn'} />
            </div>
          </div>
          <TokenRangeSelector value={tokenRange} onChange={setTokenRange} />
          {portfolioCost.breakEvenTarget > 0 && (
            <div className="mt-3">
              <div className="mb-1 flex justify-between text-xs text-text-muted">
                <span>Portfolio break-even progress</span>
                <span>{Math.round(roiProgress)}%</span>
              </div>
              <div className="h-2 overflow-hidden rounded-full bg-white/10">
                <div className="h-full rounded-full bg-success-green" style={{ width: `${roiProgress}%` }} />
              </div>
            </div>
          )}
          <TokenUsageGraph series={tokenSeries} />
          <TokenCostSettings
            models={modelNames}
            selectedModel={selectedCostModel}
            config={selectedModelCost}
            portfolioBreakEvenTarget={portfolioCost.breakEvenTarget}
            usage={modelUsage}
            onSelectModel={setSelectedCostModel}
            onChange={(next) => {
              const merged = {
                ...costConfig,
                [selectedCostModel]: {
                  ...next,
                  defaultsVersion: MODEL_COST_DEFAULTS_VERSION,
                  userEdited: selectedCostModel !== ALL_MODELS,
                },
              };
              setCostConfig(merged);
              saveCostConfig(merged);
            }}
            onChangePortfolioBreakEven={(breakEvenTarget) => {
              const current = getCostConfigForModel(ALL_MODELS, costConfig);
              const merged = {
                ...costConfig,
                [ALL_MODELS]: {
                  ...current,
                  breakEvenTarget,
                  defaultsVersion: MODEL_COST_DEFAULTS_VERSION,
                },
              };
              setCostConfig(merged);
              saveCostConfig(merged);
            }}
          />
        </Panel>
      </section>

      <section className="grid gap-4 xl:grid-cols-[1.05fr_0.95fr_1.45fr]">
        <Panel>
          <SectionTitle title="Active Model" />
          <div className="mt-4 rounded-lg border border-white/10 bg-white/[0.035] p-4">
            <div className="flex min-w-0 items-start gap-3">
              <IconShell Icon={CubeIcon} tone={state.runningModels.length ? 'good' : 'warn'} />
              <div className="min-w-0 flex-1">
                <div className="flex min-w-0 flex-wrap items-center gap-2">
                  <h3 className="truncate font-mono text-sm font-semibold text-text-primary" title={activeModel}>{activeModel}</h3>
                  <StatusBadge label={state.runningModels.length ? 'Online' : 'Standby'} tone={state.runningModels.length ? 'online' : 'idle'} />
                </div>
                <p className="mt-1 text-sm text-text-secondary">{state.runningModels.length ? 'Loaded in-process' : `${state.modelsList.length || 0} local model candidates`}</p>
              </div>
            </div>
            <dl className="mt-4 grid grid-cols-2 gap-3 text-sm">
              <ModelFact label="Context" value={String(summary.contextSize ?? metrics.context_size ?? '100000 ctx')} />
              <ModelFact label="Quantization" value={activeModelRecord?.details?.quantization_level || inferQuant(activeModel)} />
              <ModelFact label="Memory" value={formatBytes(activeModelRecord?.size) || bytesPair(vramUsed, vramTotal)} />
              <ModelFact label="Uptime" value={formatUptime(state.healthData?.uptime)} />
            </dl>
            <div className="mt-4 grid grid-cols-2 gap-2">
              <CommandButton tone="neutral" onClick={actions.unloadModels} className="min-h-9 text-xs">Unload</CommandButton>
              <CommandButton tone="blue" onClick={() => { window.location.hash = '#models'; }} className="min-h-9 text-xs">Model Control</CommandButton>
            </div>
          </div>
        </Panel>

        <Panel>
          <SectionTitle title="Service Health" action={<CommandButton tone="neutral" onClick={() => { window.location.hash = '#services'; }} className="min-h-8 px-3 py-1 text-xs">View all</CommandButton>} />
          <div className="mt-4 divide-y divide-white/10">
            {servicesMini.slice(0, 4).map(service => <ServiceRow key={service.key} service={service} />)}
          </div>
        </Panel>

        <Panel>
          <SectionTitle title="Recent Activity" action={<CommandButton tone="neutral" onClick={() => { window.location.hash = '#logs'; }} className="min-h-8 px-3 py-1 text-xs">View all</CommandButton>} />
          <div className="mt-4 divide-y divide-white/10">
            {activity.map(item => <ActivityRow key={`${item.label}-${item.time}`} {...item} />)}
          </div>
        </Panel>
      </section>
    </div>
  );
};

const Panel: React.FC<{ children: React.ReactNode; className?: string }> = ({ children, className = '' }) => (
  <div className={`rounded-lg border border-white/10 bg-[#0b1626]/88 p-4 shadow-deck ${className}`}>{children}</div>
);

const SectionTitle: React.FC<{ title: string; aside?: string; action?: React.ReactNode }> = ({ title, aside, action }) => (
  <div className="flex min-w-0 items-center justify-between gap-3">
    <div className="min-w-0">
      <h2 className="truncate text-base font-semibold text-text-primary">{title} {aside && <span className="text-xs font-normal text-text-muted">({aside})</span>}</h2>
    </div>
    {action}
  </div>
);

const StatusPill: React.FC<{ label: string; value: string; tone: Tone; Icon: React.ComponentType<React.SVGProps<SVGSVGElement>> }> = ({ label, value, tone, Icon }) => (
  <div className="inline-flex min-h-9 min-w-0 items-center gap-2 rounded-lg border border-white/10 bg-[#0d1a2b] px-3 text-sm shadow-[0_0_20px_rgba(15,23,42,0.35)]">
    <Icon className={`h-4 w-4 shrink-0 ${toneText(tone)}`} />
    <span className="truncate text-text-secondary">{label}</span>
    <span className={`inline-flex items-center gap-1 rounded-md px-2 py-0.5 text-xs font-semibold ${toneBg(tone)} ${toneText(tone)}`}>
      <span className="h-1.5 w-1.5 rounded-full" style={{ background: toneHex(tone) }} />
      {value}
    </span>
  </div>
);

const UsagePanel: React.FC<{ title: string; metrics: MetricDatum[]; details: string[] }> = ({ title, metrics, details }) => (
  <Panel>
    <h2 className="text-base font-semibold text-text-primary">{title}</h2>
    <div className="mt-5 grid gap-4 sm:grid-cols-3">
      {metrics.map((metric, index) => (
        <div key={metric.label} className="flex min-w-0 flex-col items-center gap-3 text-center 2xl:flex-row 2xl:text-left">
          <ProgressRing value={metric.value} tone={metric.tone} center={metric.label.includes('Temp') ? tempDisplay(details[index]) : `${Math.round(metric.value)}%`} />
          <div className="min-w-0">
            <p className="truncate text-sm font-medium text-text-primary">{metric.label}</p>
            <p className="mt-1 truncate text-xs text-text-secondary" title={details[index]}>{details[index]}</p>
            {metric.label.includes('Temp') && <p className={`mt-1 text-xs ${toneText(metric.tone)}`}>{metric.tone === 'idle' ? 'Unknown' : metric.tone === 'good' ? 'Normal' : metric.tone === 'warn' ? 'Warm' : 'Hot'}</p>}
          </div>
        </div>
      ))}
    </div>
  </Panel>
);

const ProgressRing: React.FC<{ value: number; tone: Tone; center: string }> = ({ value, tone, center }) => {
  const safe = clamp(value, 0, 100);
  return (
    <div
      className="grid h-24 w-24 shrink-0 place-items-center rounded-full"
      style={{ background: `conic-gradient(${toneHex(tone)} ${safe * 3.6}deg, rgba(100,116,139,0.24) 0deg)` }}
    >
      <div className="grid h-[74px] w-[74px] place-items-center rounded-full bg-[#0b1626] text-center">
        <span className="text-xl font-semibold text-text-primary">{center}</span>
      </div>
    </div>
  );
};

const MiniServiceCard: React.FC<{ service: ServiceDatum }> = ({ service }) => (
  <div className="flex min-h-[72px] min-w-0 items-center gap-3 rounded-lg border border-white/10 bg-[#0d1a2b] px-4 py-3">
    <service.Icon className={`h-7 w-7 shrink-0 ${toneText(service.tone)}`} />
    <div className="min-w-0">
      <p className="truncate text-sm font-medium text-text-primary">{service.label}</p>
      <p className={`mt-0.5 truncate text-xs font-semibold ${toneText(service.tone)}`}>{service.status}</p>
    </div>
  </div>
);

const MiniLineChart: React.FC<{ label: string; value: number; values: number[]; tone: Tone }> = ({ label, value, values, tone }) => (
  <div className="rounded-lg border border-white/10 bg-[#07101d] p-3">
    <div className="flex items-start justify-between gap-3">
      <div>
        <p className="text-xs font-medium text-text-primary">{label}</p>
        <p className="mt-1 text-lg font-semibold text-text-primary">{Math.round(value)}%</p>
      </div>
      <span className={`h-2 w-2 rounded-full ${toneClass(tone)}`} />
    </div>
    <LineChart values={values} tone={tone} height={92} yMax={100} xLabels={['60s', '30s', '0s']} />
  </div>
);

const LineChart: React.FC<{ values: number[]; tone: Tone; height: number; yMax?: number; xLabels?: string[]; dashed?: boolean }> = ({ values, tone, height, yMax, xLabels, dashed }) => {
  const width = 220;
  const max = yMax ?? Math.max(1, ...values);
  const path = toPath(values, width, height, max);
  const area = `${path} L ${width} ${height} L 0 ${height} Z`;
  return (
    <div className="mt-3">
      <svg viewBox={`0 0 ${width} ${height}`} className="h-[92px] w-full overflow-visible" role="img" aria-label="line chart">
        <g stroke="rgba(148,163,184,0.14)" strokeDasharray="3 4">
          <line x1="0" y1="0" x2={width} y2="0" />
          <line x1="0" y1={height / 2} x2={width} y2={height / 2} />
          <line x1="0" y1={height} x2={width} y2={height} />
          <line x1="0" y1="0" x2="0" y2={height} />
          <line x1={width} y1="0" x2={width} y2={height} />
        </g>
        <path d={area} fill={toneHex(tone)} opacity="0.08" />
        <path d={path} fill="none" stroke={toneHex(tone)} strokeWidth="3" strokeLinecap="round" strokeLinejoin="round" strokeDasharray={dashed ? '7 6' : undefined} />
      </svg>
      {xLabels && <div className="mt-1 flex justify-between text-[11px] text-text-muted">{xLabels.map(label => <span key={label}>{label}</span>)}</div>}
    </div>
  );
};

const TokenUsageGraph: React.FC<{ series: ReturnType<typeof buildTokenSeries> }> = ({ series }) => {
  const tokenMax = Math.max(1, ...series.total, ...series.prompt, ...series.output);
  const costMax = Math.max(1, ...series.cost);
  const monthX = (index: number) => series.months.length <= 1 ? 340 : (680 / (series.months.length - 1)) * index;
  return (
    <div className="mt-5">
      <div className="grid grid-cols-[34px_1fr_42px] gap-2 text-xs text-text-muted">
        <div className="flex flex-col justify-between py-2"><span>{formatTokenCount(tokenMax)}</span><span>{formatTokenCount(tokenMax / 2)}</span><span>0</span></div>
        <div>
          <svg viewBox="0 0 680 150" className="h-[150px] w-full overflow-visible" role="img" aria-label="token usage graph">
            <g stroke="rgba(148,163,184,0.14)" strokeDasharray="4 5">
              {[0, 75, 150].map(y => <line key={y} x1="0" y1={y} x2="680" y2={y} />)}
              {series.months.map((_, index) => <line key={index} x1={monthX(index)} y1="0" x2={monthX(index)} y2="150" />)}
            </g>
            <path d={toPath(series.total, 680, 150, tokenMax)} fill="none" stroke="#60A5FA" strokeWidth="3" />
            <path d={toPath(series.prompt, 680, 150, tokenMax)} fill="none" stroke="#34D399" strokeWidth="3" />
            <path d={toPath(series.output, 680, 150, tokenMax)} fill="none" stroke="#A78BFA" strokeWidth="3" />
            <path d={toPath(series.cost, 680, 150, costMax)} fill="none" stroke="#22C55E" strokeWidth="3" strokeDasharray="8 6" />
          </svg>
          <div className="mt-1 flex justify-between text-xs text-text-muted">{series.months.map(month => <span key={month}>{month}</span>)}</div>
        </div>
        <div className="flex flex-col justify-between py-2 text-right"><span>{formatCurrency(costMax)}</span><span>{formatCurrency(costMax / 2)}</span><span>$0</span></div>
      </div>
      <div className="mt-4 flex flex-wrap gap-x-5 gap-y-2 text-xs text-text-secondary">
        <Legend color="#60A5FA" label="Total Tokens" />
        <Legend color="#34D399" label="Prompt Tokens" />
        <Legend color="#A78BFA" label="Output Tokens" />
        <Legend color="#22C55E" label="Est. Cost Avoided (USD)" dashed />
      </div>
    </div>
  );
};

const TokenRangeSelector: React.FC<{ value: TokenRange; onChange: (value: TokenRange) => void }> = ({ value, onChange }) => (
  <div className="mt-3 flex flex-wrap gap-1 rounded-lg border border-white/10 bg-[#07101d] p-1 text-xs">
    {(Object.keys(TOKEN_RANGE_LABELS) as TokenRange[]).map(range => (
      <button
        key={range}
        type="button"
        className={`rounded-md px-3 py-1.5 font-medium transition ${value === range ? 'bg-primary-blue text-white' : 'text-text-muted hover:bg-white/5 hover:text-text-primary'}`}
        onClick={() => onChange(range)}
      >
        {TOKEN_RANGE_LABELS[range]}
      </button>
    ))}
  </div>
);

const TokenCostSettings: React.FC<{
  models: string[];
  selectedModel: string;
  config: ModelCostConfig;
  portfolioBreakEvenTarget: number;
  usage: ModelTokenUsage;
  onSelectModel: (model: string) => void;
  onChange: (config: ModelCostConfig) => void;
  onChangePortfolioBreakEven: (breakEvenTarget: number) => void;
}> = ({ models, selectedModel, config, portfolioBreakEvenTarget, usage, onSelectModel, onChange, onChangePortfolioBreakEven }) => (
  <div className="mt-4 grid gap-3 rounded-lg border border-white/10 bg-[#07101d] p-3 lg:grid-cols-[1.15fr_1fr_1fr_1fr]">
    <label className="min-w-0 text-xs text-text-secondary">
      <span className="mb-1 block text-text-muted">Model</span>
      <select
        className="h-9 w-full rounded-md border border-white/10 bg-[#0b1626] px-2 text-sm text-text-primary"
        value={selectedModel}
        onChange={event => onSelectModel(event.target.value)}
      >
        {models.map(model => <option key={model} value={model}>{model}</option>)}
      </select>
    </label>
    <label className={`min-w-0 text-xs text-text-secondary ${selectedModel === ALL_MODELS ? 'opacity-50' : ''}`}>
      <span className="mb-1 block text-text-muted">Prompt $/1M</span>
      <input
        className="h-9 w-full rounded-md border border-white/10 bg-[#0b1626] px-2 text-sm text-text-primary"
        type="number"
        min="0"
        step="0.01"
        value={config.promptPerMillion}
        disabled={selectedModel === ALL_MODELS}
        onChange={event => onChange({ ...config, promptPerMillion: Number(event.target.value) || 0 })}
      />
    </label>
    <label className={`min-w-0 text-xs text-text-secondary ${selectedModel === ALL_MODELS ? 'opacity-50' : ''}`}>
      <span className="mb-1 block text-text-muted">Output $/1M</span>
      <input
        className="h-9 w-full rounded-md border border-white/10 bg-[#0b1626] px-2 text-sm text-text-primary"
        type="number"
        min="0"
        step="0.01"
        value={config.outputPerMillion}
        disabled={selectedModel === ALL_MODELS}
        onChange={event => onChange({ ...config, outputPerMillion: Number(event.target.value) || 0 })}
      />
    </label>
    <label className="min-w-0 text-xs text-text-secondary">
      <span className="mb-1 block text-text-muted">Portfolio Break-even $</span>
      <input
        className="h-9 w-full rounded-md border border-white/10 bg-[#0b1626] px-2 text-sm text-text-primary"
        type="number"
        min="0"
        step="1"
        value={portfolioBreakEvenTarget}
        onChange={event => onChangePortfolioBreakEven(Number(event.target.value) || 0)}
      />
    </label>
    <p className="lg:col-span-4 text-xs text-text-muted">
      {selectedModel === ALL_MODELS
        ? 'Portfolio cost avoided uses each model\'s persisted tokens and saved per-model API prices; the break-even target applies to the whole tracked portfolio.'
        : `The token graph and price fields are for ${compactModel(usage.model)}. Headline ROI always uses all tracked models and the portfolio break-even target.`}
      {selectedModel !== ALL_MODELS && config.source ? ` Default source: ${config.source}.` : ''}
    </p>
  </div>
);

const SummaryStat: React.FC<{ label: string; value: string; tone?: Tone }> = ({ label, value, tone = 'idle' }) => (
  <div className="min-w-0">
    <p className="truncate text-xs text-text-muted">{label}</p>
    <p className={`mt-0.5 truncate text-lg font-semibold ${tone === 'idle' ? 'text-text-primary' : toneText(tone)}`}>{value}</p>
  </div>
);

const Legend: React.FC<{ color: string; label: string; dashed?: boolean }> = ({ color, label, dashed }) => (
  <span className="inline-flex items-center gap-2">
    <span className="h-0.5 w-6" style={{ background: dashed ? `repeating-linear-gradient(90deg, ${color} 0 8px, transparent 8px 13px)` : color }} />
    {label}
  </span>
);

const ModelFact: React.FC<{ label: string; value: string }> = ({ label, value }) => (
  <div className="min-w-0">
    <dt className="text-xs text-text-muted">{label}</dt>
    <dd className="mt-1 truncate text-sm text-text-primary" title={value}>{value}</dd>
  </div>
);

const ServiceRow: React.FC<{ service: ServiceDatum }> = ({ service }) => (
  <div className="flex min-w-0 items-center justify-between gap-3 py-3">
    <div className="flex min-w-0 items-center gap-3">
      <IconShell Icon={service.Icon} tone={service.tone} small />
      <span className="truncate text-sm text-text-primary">{service.label}</span>
    </div>
    <span className={`shrink-0 text-xs font-semibold ${toneText(service.tone)}`}>{service.status}</span>
  </div>
);

const ActivityRow: React.FC<{ Icon: React.ComponentType<React.SVGProps<SVGSVGElement>>; label: string; source: string; time: string; tone: Tone }> = ({ Icon, label, source, time, tone }) => (
  <div className="flex min-w-0 items-center gap-3 py-3">
    <Icon className={`h-5 w-5 shrink-0 ${toneText(tone)}`} />
    <div className="min-w-0 flex-1">
      <p className="truncate text-sm text-text-primary">{label}</p>
      <p className="truncate text-xs text-text-muted">{source}</p>
    </div>
    <span className="shrink-0 text-xs text-text-muted">{time}</span>
  </div>
);

const IconShell: React.FC<{ Icon: React.ComponentType<React.SVGProps<SVGSVGElement>>; tone: Tone; small?: boolean }> = ({ Icon, tone, small }) => (
  <div className={`grid shrink-0 place-items-center rounded-lg border ${small ? 'h-8 w-8' : 'h-11 w-11'} ${toneBorder(tone)}`}>
    <Icon className={`${small ? 'h-4 w-4' : 'h-6 w-6'} ${toneText(tone)}`} />
  </div>
);

function normalizeServices(services: ServiceRecord[], connected: boolean): ServiceRecord[] {
  const fallback: ServiceRecord[] = [
    { id: 'gateway', name: 'Gateway', kind: 'gateway', status: connected ? 'running' : 'offline', baseUrl: null },
    { id: 'llama-server', name: 'llama.cpp', kind: 'llama_cpp', status: 'offline', baseUrl: 'http://127.0.0.1:18080' },
    { id: 'whisper', name: 'Whisper', kind: 'whisper_cpp', status: 'not_configured', baseUrl: null },
  ];
  return fallback.map(item => services.find(service => service.kind === item.kind || service.id === item.id) || item);
}

function fillHistory(history: Array<{ cpu: number; ram: number; gpu: number; vram: number }>) {
  const samples = history.slice(-60);
  if (samples.length >= 2) return samples;
  const latest = samples[0] || { cpu: 0, ram: 0, gpu: 0, vram: 0 };
  return Array.from({ length: 60 }, () => latest);
}

function getHardwareHistory(samples: unknown): Array<{ timestamp: string; cpu: number; ram: number; gpu: number; vram: number }> {
  if (!Array.isArray(samples)) return [];
  return samples
    .map(sample => {
      const row = sample as Record<string, unknown>;
      return {
        timestamp: String(row.timestamp || ''),
        cpu: firstPercent(row.cpu, row.cpuPercent) ?? 0,
        ram: firstPercent(row.ram, row.ramPercent) ?? 0,
        gpu: firstPercent(row.gpu, row.gpuPercent) ?? 0,
        vram: firstPercent(row.vram, row.vramPercent) ?? 0,
      };
    })
    .filter(sample => sample.timestamp)
    .slice(-60);
}

function buildTokenSeries(
  jobs: JobRecord[],
  model: string,
  cost: ModelCostConfig,
  persisted: PersistedUsageBucket[] = [],
  config: Record<string, ModelCostConfig> = {},
  range: TokenRange = 'all',
) {
  const buckets = buildTokenBuckets(range, jobs, model, persisted);
  const byBucket = new Map(buckets.map(bucket => [bucket.key, { prompt: 0, output: 0, total: 0, cost: 0 }]));
  const usePersisted = persisted.length > 0 && (range === 'all' || range === 'year');
  if (usePersisted) {
    for (const row of persisted) {
      if (model !== ALL_MODELS && row.model !== model) continue;
      const bucket = byBucket.get(row.bucket);
      if (!bucket) continue;
      const prompt = Number(row.promptTokens ?? row.prompt_tokens ?? 0);
      const output = Number(row.completionTokens ?? row.completion_tokens ?? 0);
      bucket.prompt += prompt;
      bucket.output += output;
      bucket.total += Number(row.totalTokens ?? row.total_tokens ?? prompt + output);
      bucket.cost += estimateCostAvoided(
        { model: row.model, prompt, output, total: prompt + output },
        model === ALL_MODELS ? getCostConfigForModel(row.model, config) : cost,
      );
    }
    const values = buckets.map(bucket => byBucket.get(bucket.key) || { prompt: 0, output: 0, total: 0, cost: 0 });
    return {
      months: buckets.map(bucket => bucket.label),
      total: values.map(value => value.total),
      prompt: values.map(value => value.prompt),
      output: values.map(value => value.output),
      cost: values.map(value => value.cost),
    };
  }
  for (const job of jobs) {
    if (model !== ALL_MODELS && (job.model || 'Unknown model') !== model) continue;
    const date = parseJobDate(job);
    const chartBucket = findTokenBucket(buckets, date);
    const bucket = chartBucket ? byBucket.get(chartBucket.key) : undefined;
    if (!bucket) continue;
    const prompt = Number(job.promptTokens ?? 0);
    const output = Number(job.completionTokens ?? 0);
    const jobModel = job.model || 'Unknown model';
    bucket.prompt += prompt;
    bucket.output += output;
    bucket.total += Number(job.totalTokens ?? prompt + output);
    bucket.cost += estimateCostAvoided(
      { model: jobModel, prompt, output, total: prompt + output },
      model === ALL_MODELS ? getCostConfigForModel(jobModel, config) : cost,
    );
  }
  const values = buckets.map(bucket => byBucket.get(bucket.key) || { prompt: 0, output: 0, total: 0, cost: 0 });
  return {
    months: buckets.map(bucket => bucket.label),
    total: values.map(value => value.total),
    prompt: values.map(value => value.prompt),
    output: values.map(value => value.output),
    cost: values.map(value => value.cost),
  };
}

type TokenBucket = { key: string; label: string; start: Date; end: Date };

function buildTokenBuckets(range: TokenRange, jobs: JobRecord[], model: string, persisted: PersistedUsageBucket[]): TokenBucket[] {
  if (range === 'week') return fixedDayBuckets(7);
  if (range === 'month') return fixedDayBuckets(30, 5);
  return monthlyTokenBuckets(range, jobs, model, persisted);
}

function fixedDayBuckets(days: number, spanDays = 1): TokenBucket[] {
  const now = startOfDay(new Date());
  const bucketCount = Math.ceil(days / spanDays);
  const firstStart = addDays(now, -(days - 1));
  return Array.from({ length: bucketCount }, (_, index) => {
    const start = addDays(firstStart, index * spanDays);
    const end = addDays(start, spanDays);
    return {
      key: `${dateKey(start)}:${spanDays}`,
      label: spanDays === 1 ? start.toLocaleString([], { weekday: 'short' }) : start.toLocaleString([], { month: 'short', day: 'numeric' }),
      start,
      end: index === bucketCount - 1 ? addDays(now, 1) : end,
    };
  });
}

function monthlyTokenBuckets(range: TokenRange, jobs: JobRecord[], model: string, persisted: PersistedUsageBucket[]): TokenBucket[] {
  const monthKeys = new Set<string>();
  const now = new Date();
  const earliestYearMonth = range === 'year'
    ? `${now.getFullYear() - (now.getMonth() < 11 ? 1 : 0)}-${String(((now.getMonth() + 1) % 12) + 1).padStart(2, '0')}`
    : '';
  for (const row of persisted) {
    if (model !== ALL_MODELS && row.model !== model) continue;
    if (!/^\d{4}-\d{2}$/.test(row.bucket)) continue;
    if (range === 'year' && row.bucket < earliestYearMonth) continue;
    monthKeys.add(row.bucket);
  }
  if (!monthKeys.size) {
    for (const job of jobs) {
      if (model !== ALL_MODELS && (job.model || 'Unknown model') !== model) continue;
      const date = parseJobDate(job);
      const key = monthKey(date);
      if (range === 'year' && key < earliestYearMonth) continue;
      monthKeys.add(key);
    }
  }
  if (!monthKeys.size) monthKeys.add(monthKey(now));
  return Array.from(monthKeys).sort().map(key => {
    const start = monthStart(key);
    return { key, label: monthLabel(key), start, end: addMonths(start, 1) };
  });
}

function findTokenBucket(buckets: TokenBucket[], date: Date): TokenBucket | undefined {
  const time = date.getTime();
  return buckets.find(bucket => time >= bucket.start.getTime() && time < bucket.end.getTime());
}

function tokenUsageFromSeries(model: string, series: ReturnType<typeof buildTokenSeries>): ModelTokenUsage {
  return {
    model,
    prompt: series.prompt.reduce((sum, value) => sum + value, 0),
    output: series.output.reduce((sum, value) => sum + value, 0),
    total: series.total.reduce((sum, value) => sum + value, 0),
  };
}

function buildActivity(jobs: JobRecord[], observability: any, lastUpdatedAt: Date | null) {
  const rows: Array<{ Icon: React.ComponentType<React.SVGProps<SVGSVGElement>>; label: string; source: string; time: string; tone: Tone }> = [];
  const recent = [
    ...(Array.isArray(observability?.recentRunning) ? observability.recentRunning : []),
    ...(Array.isArray(observability?.recentCompleted) ? observability.recentCompleted : []),
    ...(Array.isArray(observability?.recentAccepted) ? observability.recentAccepted : []),
    ...jobs,
  ];
  const seen = new Set<string>();
  for (const job of recent) {
    if (!job?.id || seen.has(job.id)) continue;
    seen.add(job.id);
    rows.push({
      Icon: job.status === 'failed' ? ListBulletIcon : DocumentTextIcon,
      label: `${job.status || 'observed'}: ${compactModel(job.model || job.type || 'AI request')}`,
      source: job.client || job.resourceClass || 'runtime',
      time: timeAgo(job.completedAt || job.completed_at || job.startedAt || job.started_at || job.createdAt || job.created_at),
      tone: job.status === 'failed' ? 'critical' : job.status === 'running' || job.status === 'leased' ? 'info' : 'good',
    });
    if (rows.length >= 4) return rows;
  }
  return [{ Icon: ClockIcon, label: 'No runtime activity recorded yet', source: 'system', time: timeAgo(lastUpdatedAt), tone: 'idle' as Tone }];
}

function toPath(values: number[], width: number, height: number, max: number): string {
  if (!values.length) return `M 0 ${height}`;
  if (values.length === 1) {
    const y = height - (clamp(values[0], 0, max) / max) * height;
    return `M 0 ${y.toFixed(2)} L ${width} ${y.toFixed(2)}`;
  }
  return values.map((value, index) => {
    const x = (width / (values.length - 1)) * index;
    const y = height - (clamp(value, 0, max) / max) * height;
    return `${index === 0 ? 'M' : 'L'} ${x.toFixed(2)} ${y.toFixed(2)}`;
  }).join(' ');
}

function getActiveModel(running: ModelRecord[], models: ModelRecord[]): string {
  return running[0]?.name || models.find(model => model.loaded)?.name || models[0]?.name || 'No model loaded';
}

function getActiveModelRecord(running: ModelRecord[], models: ModelRecord[]): ModelRecord | undefined {
  return running[0] || models.find(model => model.loaded) || models[0];
}

function compactModel(model: string): string {
  const file = model.split(/[\\/]/).pop() || model;
  return file.length > 32 ? `${file.slice(0, 29)}...` : file;
}

function inferQuant(model: string): string {
  const match = model.match(/Q\d_[A-Z_]+/i);
  return match?.[0] || 'Q4_K_M';
}

function cpuDetail(system: Record<string, any>): string {
  const used = firstNumber(system.activeCores, system.cpuCoresUsed);
  const total = firstNumber(system.cores, system.cpuCores, system.logicalProcessors);
  return used && total ? `${used} / ${total} cores` : total ? `${total} logical cores` : 'processor';
}

function tempDisplay(value: string): string {
  const match = value.match(/\d+/);
  return match ? `${match[0]}C` : 'N/A';
}

function tempLabel(value?: number | null, reason?: string | null): string {
  if (value != null) return `${Math.round(value)} C`;
  if (!reason) return 'N/A';
  const cleaned = reason.replace(/\.$/, '');
  if (cleaned.includes('No ADLX helper is installed')) return 'ADLX helper missing';
  return cleaned;
}

function bytesPair(used?: number | null, total?: number | null): string {
  if (used == null || total == null) return 'N/A';
  return `${formatBytes(used)} / ${formatBytes(total)}`;
}

function getModelNames(jobs: JobRecord[], models: ModelRecord[], activeModel: string, persisted: PersistedUsage[] = []): string[] {
  const names = new Set<string>();
  if (persisted.length || jobs.length) names.add(ALL_MODELS);
  if (activeModel && activeModel !== 'No model loaded') names.add(activeModel);
  for (const row of persisted) names.add(row.model);
  for (const job of jobs) names.add(job.model || 'Unknown model');
  for (const model of models) names.add(model.name);
  return Array.from(names).filter(Boolean);
}

function getModelTokenUsage(jobs: JobRecord[], model: string, fallback: { prompt: number; output: number; total: number }, persisted: PersistedUsage[] = []): ModelTokenUsage {
  if (model === ALL_MODELS) {
    if (persisted.length) {
      return persisted.reduce<ModelTokenUsage>((current, row) => {
        const prompt = Number(row.promptTokens ?? row.prompt_tokens ?? 0);
        const output = Number(row.completionTokens ?? row.completion_tokens ?? 0);
        current.prompt += prompt;
        current.output += output;
        current.total += Number(row.totalTokens ?? row.total_tokens ?? prompt + output);
        return current;
      }, { model, prompt: 0, output: 0, total: 0 });
    }
    return { model, ...fallback };
  }
  const stored = persisted.find(row => row.model === model);
  if (stored) {
    const prompt = Number(stored.promptTokens ?? stored.prompt_tokens ?? 0);
    const output = Number(stored.completionTokens ?? stored.completion_tokens ?? 0);
    return {
      model,
      prompt,
      output,
      total: Number(stored.totalTokens ?? stored.total_tokens ?? prompt + output),
    };
  }
  const usage = jobs.reduce<ModelTokenUsage>((current, job) => {
    if ((job.model || 'Unknown model') !== model) return current;
    const prompt = Number(job.promptTokens ?? 0);
    const output = Number(job.completionTokens ?? 0);
    current.prompt += prompt;
    current.output += output;
    current.total += Number(job.totalTokens ?? prompt + output);
    return current;
  }, { model, prompt: 0, output: 0, total: 0 });
  if (usage.total === 0 && fallback.total > 0) {
    return { model, ...fallback };
  }
  return usage;
}

function loadCostConfig(): Record<string, ModelCostConfig> {
  if (typeof window === 'undefined') return {};
  try {
    const parsed = JSON.parse(window.localStorage.getItem(COST_STORAGE_KEY) || '{}') as Record<string, ModelCostConfig>;
    if (!parsed || typeof parsed !== 'object') return {};
    return Object.fromEntries(Object.entries(parsed).map(([model, config]) => [model, normalizeCostConfig(model, config, MODEL_COST_DEFAULTS[model])]));
  } catch {
    return {};
  }
}

function saveCostConfig(config: Record<string, ModelCostConfig>) {
  if (typeof window === 'undefined') return;
  window.localStorage.setItem(COST_STORAGE_KEY, JSON.stringify(config));
}

function normalizeCostConfig(model: string, config?: Partial<ModelCostConfig>, defaultConfig: ModelCostConfig = DEFAULT_COST_CONFIG): ModelCostConfig {
  const shouldRefreshDefaults =
    defaultConfig !== DEFAULT_COST_CONFIG &&
    config &&
    !config.userEdited &&
    Number(config.defaultsVersion ?? 0) < MODEL_COST_DEFAULTS_VERSION &&
    isLegacyDefaultConfig(model, config);
  const source = shouldRefreshDefaults ? defaultConfig : config;
  const savedBreakEven = Number(config?.breakEvenTarget);
  const breakEvenTarget = !Number.isFinite(savedBreakEven) || savedBreakEven === 0
    ? DEFAULT_BREAK_EVEN_TARGET
    : sanitizeMoney(config?.breakEvenTarget, defaultConfig.breakEvenTarget);
  return {
    equivalentModel: typeof source?.equivalentModel === 'string' && source.equivalentModel.trim()
      ? source.equivalentModel
      : defaultConfig.equivalentModel,
    promptPerMillion: sanitizeMoney(source?.promptPerMillion, defaultConfig.promptPerMillion),
    outputPerMillion: sanitizeMoney(source?.outputPerMillion, defaultConfig.outputPerMillion),
    breakEvenTarget,
    source: typeof source?.source === 'string' && source.source.trim() ? source.source : defaultConfig.source,
    defaultsVersion: Number(source?.defaultsVersion ?? defaultConfig.defaultsVersion ?? MODEL_COST_DEFAULTS_VERSION),
    userEdited: Boolean(config?.userEdited),
  };
}

function getCostConfigForModel(model: string, config: Record<string, ModelCostConfig>): ModelCostConfig {
  const defaultConfig = MODEL_COST_DEFAULTS[model] || DEFAULT_COST_CONFIG;
  return normalizeCostConfig(model, config[model], defaultConfig);
}

function sanitizeMoney(value: unknown, fallback: number): number {
  const number = Number(value);
  return Number.isFinite(number) && number >= 0 ? number : fallback;
}

function isLegacyDefaultConfig(model: string, config: Partial<ModelCostConfig>): boolean {
  if (!config.defaultsVersion && config.equivalentModel === DEFAULT_COST_CONFIG.equivalentModel) return true;
  const prompt = Number(config.promptPerMillion);
  const output = Number(config.outputPerMillion);
  return (LEGACY_DEFAULT_PRICES[model] || []).some(([legacyPrompt, legacyOutput]) =>
    Math.abs(prompt - legacyPrompt) < 0.0001 && Math.abs(output - legacyOutput) < 0.0001,
  );
}

function estimateCostAvoided(usage: ModelTokenUsage, cost: ModelCostConfig): number {
  return (usage.prompt / 1_000_000) * cost.promptPerMillion + (usage.output / 1_000_000) * cost.outputPerMillion;
}

function estimatePortfolioCostAvoided(usage: PersistedUsage[], config: Record<string, ModelCostConfig>): number {
  return usage.reduce((sum, row) => {
    const prompt = Number(row.promptTokens ?? row.prompt_tokens ?? 0);
    const output = Number(row.completionTokens ?? row.completion_tokens ?? 0);
    return sum + estimateCostAvoided(
      { model: row.model, prompt, output, total: Number(row.totalTokens ?? row.total_tokens ?? prompt + output) },
      getCostConfigForModel(row.model, config),
    );
  }, 0);
}

function estimateJobsPortfolioCostAvoided(
  jobs: JobRecord[],
  config: Record<string, ModelCostConfig>,
  fallback: { prompt: number; output: number; total: number },
): number {
  if (!jobs.length) return estimateCostAvoided({ model: ALL_MODELS, ...fallback }, DEFAULT_COST_CONFIG);
  return jobs.reduce((sum, job) => {
    const prompt = Number(job.promptTokens ?? 0);
    const output = Number(job.completionTokens ?? 0);
    const model = job.model || 'Unknown model';
    return sum + estimateCostAvoided(
      { model, prompt, output, total: Number(job.totalTokens ?? prompt + output) },
      getCostConfigForModel(model, config),
    );
  }, 0);
}

function monthLabel(key: string): string {
  const [year, month] = key.split('-').map(Number);
  const date = new Date(year, (month || 1) - 1, 1);
  return `${date.toLocaleString([], { month: 'short' })} ${year}`;
}

function monthKey(date: Date): string {
  return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}`;
}

function dateKey(date: Date): string {
  return `${date.getFullYear()}-${String(date.getMonth() + 1).padStart(2, '0')}-${String(date.getDate()).padStart(2, '0')}`;
}

function monthStart(key: string): Date {
  const [year, month] = key.split('-').map(Number);
  return new Date(year, (month || 1) - 1, 1);
}

function startOfDay(date: Date): Date {
  return new Date(date.getFullYear(), date.getMonth(), date.getDate());
}

function addDays(date: Date, days: number): Date {
  const next = new Date(date);
  next.setDate(next.getDate() + days);
  return next;
}

function addMonths(date: Date, months: number): Date {
  const next = new Date(date);
  next.setMonth(next.getMonth() + months);
  return next;
}

function parseJobDate(job: JobRecord): Date {
  const value = job.completedAt || job.completed_at || job.startedAt || job.started_at || job.createdAt || job.created_at;
  const date = value ? new Date(value) : new Date();
  return Number.isNaN(date.getTime()) ? new Date() : date;
}

function formatCurrency(value: number): string {
  return `$${value.toLocaleString(undefined, { maximumFractionDigits: value < 10 ? 2 : 0 })}`;
}

function formatTokenCount(value?: number | null): string {
  const count = Number(value ?? 0);
  if (count >= 1_000_000_000) return `${(count / 1_000_000_000).toFixed(2)}B`;
  if (count >= 1_000_000) return `${Math.round(count / 1_000_000)}M`;
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
  return clamp(number, 0, 100);
}

function percentOf(used?: number | null, total?: number | null): number | null {
  if (used == null || total == null || total <= 0) return null;
  return (used / total) * 100;
}

function tempToPercent(value?: number | null): number {
  if (value == null) return 0;
  return clamp(value, 0, 100);
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

function clamp(value: number, min: number, max: number): number {
  return Math.max(min, Math.min(value, max));
}

function toneText(tone: Tone): string {
  if (tone === 'good') return 'text-success-green';
  if (tone === 'warn') return 'text-warning-amber';
  if (tone === 'critical') return 'text-danger-rose';
  if (tone === 'info') return 'text-queue-blue';
  if (tone === 'violet') return 'text-infer-violet';
  return 'text-text-secondary';
}

function toneBg(tone: Tone): string {
  if (tone === 'good') return 'bg-success-green/10';
  if (tone === 'warn') return 'bg-warning-amber/10';
  if (tone === 'critical') return 'bg-danger-rose/10';
  if (tone === 'info') return 'bg-queue-blue/10';
  if (tone === 'violet') return 'bg-infer-violet/10';
  return 'bg-white/[0.04]';
}

function toneBorder(tone: Tone): string {
  if (tone === 'good') return 'border-success-green/25 bg-success-green/10';
  if (tone === 'warn') return 'border-warning-amber/25 bg-warning-amber/10';
  if (tone === 'critical') return 'border-danger-rose/25 bg-danger-rose/10';
  if (tone === 'info') return 'border-queue-blue/25 bg-queue-blue/10';
  if (tone === 'violet') return 'border-infer-violet/25 bg-infer-violet/10';
  return 'border-white/10 bg-white/[0.04]';
}

function toneHex(tone: Tone): string {
  if (tone === 'good') return '#22C55E';
  if (tone === 'warn') return '#F59E0B';
  if (tone === 'critical') return '#F43F5E';
  if (tone === 'info') return '#60A5FA';
  if (tone === 'violet') return '#8B5CF6';
  return '#64748B';
}

function toneClass(tone: Tone): string {
  if (tone === 'good') return 'bg-success-green';
  if (tone === 'warn') return 'bg-warning-amber';
  if (tone === 'critical') return 'bg-danger-rose';
  if (tone === 'info') return 'bg-queue-blue';
  if (tone === 'violet') return 'bg-infer-violet';
  return 'bg-text-muted';
}
