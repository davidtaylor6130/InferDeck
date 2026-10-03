import React, { useCallback, useEffect, useState } from 'react';
import { ChevronRightIcon } from '@heroicons/react/20/solid';
import { getLogs } from '../api';
import { Badge, Button, Dot, EmptyState, GroupHeader, Meter, Notice, SectionTitle } from '../components/ui';
import { modalityLabel, modelsForSection, sectionLabel, type DashboardSection } from '../dashboardSections';
import { useGateway } from '../gateway';
import {
  clamp,
  compactModel,
  formatBytes,
  formatMb,
  formatUptime,
  temperatureTone,
  threshold,
  toneLabel,
} from '../utils';
import { ConfigPanel } from './ConfigPanel';
import { MediaJobsPanel } from './MediaJobsPanel';

const LOG_POLL_MS = 5_000;
type AlertLogLevel = 'trace' | 'debug' | 'info' | 'warn' | 'error' | 'critical';

const normalizeLogLevel = (value: unknown): AlertLogLevel => {
  const level = String(value || '').toLowerCase();
  if (level === 'warning') return 'warn';
  if (level === 'fatal') return 'critical';
  return ['trace', 'debug', 'info', 'warn', 'error', 'critical'].includes(level)
    ? level as AlertLogLevel
    : 'info';
};

export function parseDashboardLogLine(line: string) {
  try {
    const value = JSON.parse(line) as Record<string, unknown>;
    if (value && typeof value === 'object') {
      return {
        level: normalizeLogLevel(value.level),
        event: typeof value.event === 'string' ? value.event : 'gateway',
        message: typeof value.message === 'string' ? value.message : line,
        timestampUnixMs: Number.isFinite(Number(value.ts)) ? Number(value.ts) : 0,
      };
    }
  } catch {}
  const level = line.match(/\[(trace|debug|info|warn|warning|error|critical|fatal)\]/i)?.[1]
    ?? line.match(/\b(trace|debug|info|warn|warning|error|critical|fatal)\b/i)?.[1];
  const eventMatch = line.match(/\bevent=([^\s]+)/);
  const timestamp = line.match(/^\[([^\]]+)\]/)?.[1];
  return {
    level: normalizeLogLevel(level),
    event: eventMatch?.[1] ?? 'gateway',
    message: eventMatch?.index == null ? line : line.slice(eventMatch.index + eventMatch[0].length).trim(),
    timestampUnixMs: timestamp ? Date.parse(timestamp.replace(' ', 'T')) || 0 : 0,
  };
}

const RuntimeList: React.FC<{ title: string; models: ReturnType<typeof modelsForSection>; emptyTitle: string }> = ({ title, models, emptyTitle }) => (
  <section aria-label={title}>
    <GroupHeader title={title} aside={`${models.length} configured`} />
    {models.length === 0 ? (
      <EmptyState title={emptyTitle} />
    ) : (
      <div className="divide-y divide-border-slate rounded-lg border border-border-slate bg-panel-slate shadow-card">
        {models.map(model => (
          <div key={model.id} className="flex min-w-0 items-center gap-3 px-3 py-2">
            <div className="min-w-0 flex-1">
              <p className="truncate font-mono text-sm text-text-primary" title={model.id}>{compactModel(model.id)}</p>
              <p className="mt-0.5 text-xs text-text-muted">{model.runtime || 'Unknown runtime'}, {modalityLabel(model.modality)}</p>
            </div>
            {model.runtime_available === false
              ? <Badge label="Unavailable" tone="critical" />
              : <Badge label={model.loaded ? 'Loaded' : 'Ready'} tone={model.loaded ? 'good' : 'idle'} />}
          </div>
        ))}
      </div>
    )}
  </section>
);

const Fact: React.FC<{ label: string; value: string }> = ({ label, value }) => (
  <div className="flex items-baseline justify-between gap-3">
    <span className="text-sm text-text-secondary">{label}</span>
    <span className="tabular text-sm font-medium text-text-primary">{value}</span>
  </div>
);

export const SystemPage: React.FC<{ section?: DashboardSection }> = ({ section = 'llm' }) => {
  const { stats, status, models } = useGateway();
  const gpu = stats?.gpu;
  const memory = status?.hardware?.memory;
  const cpu = status?.hardware?.cpu;
  const memoryPercent = memory ? clamp(memory.percentage, 0, 100) : null;
  const scopedModels = modelsForSection(models, section);
  const label = sectionLabel(section);
  const usesGpu = section !== 'dictation';
  const vramPercent = gpu?.vramTotalMb ? gpu.vramUsedMb / gpu.vramTotalMb * 100 : 0;
  const temperature = temperatureTone(gpu?.temperatureC);
  const unavailable = scopedModels.filter(model => model.runtime_available === false).length;
  const vramTone = gpu?.vramTotalMb ? threshold(vramPercent) : 'idle';
  const problems = [
    unavailable ? `${unavailable} runtime${unavailable === 1 ? ' is' : 's are'} unavailable` : '',
    usesGpu && (vramTone === 'warn' || vramTone === 'critical') ? `VRAM is ${Math.round(vramPercent)}% full` : '',
    usesGpu && (temperature === 'warn' || temperature === 'critical') && gpu ? `The GPU is running hot at ${Math.round(gpu.temperatureC)}°C` : '',
    memoryPercent != null && threshold(memoryPercent) === 'critical' ? `System RAM is ${Math.round(memoryPercent)}% full` : '',
  ].filter(Boolean);

  return (
    <div className="space-y-8">
      <header>
        <p className="flex items-center gap-2 text-sm text-text-muted"><Dot tone={problems.length ? 'warn' : 'good'} /> {label} health</p>
        <h1 className="mt-2 text-2xl font-semibold text-text-primary sm:text-[28px] sm:leading-9">
          {problems.length ? problems.join('. ') + '.' : 'Everything looks healthy.'}
        </h1>
        <p className="mt-1 text-sm text-text-muted">
          {scopedModels.length - unavailable} of {scopedModels.length} {label} runtime{scopedModels.length === 1 ? '' : 's'} available. Gateway warnings, the full log and configuration recovery are further down.
        </p>
      </header>

      <div className="grid gap-8 xl:grid-cols-2">
        {usesGpu ? (
          <section>
            <SectionTitle title={`${label} accelerator`} aside={gpu?.name || status?.hardware?.provider} />
            <div className="mt-4 space-y-4">
              <Meter label="GPU utilization" value={gpu ? `${Math.round(gpu.utilizationPct)}%` : 'N/A'} percent={gpu?.utilizationPct ?? 0} />
              <Meter
                label="VRAM"
                value={gpu ? `${formatMb(gpu.vramUsedMb)}${gpu.vramTotalMb ? ` of ${formatMb(gpu.vramTotalMb)}` : ''}` : 'N/A'}
                percent={vramPercent}
                tone={vramTone === 'idle' || vramTone === 'good' ? 'info' : vramTone}
                sub={gpu?.vramTotalMb ? `${Math.round(vramPercent)}% used${toneLabel(vramTone) ? `, ${toneLabel(vramTone)}` : ''}` : undefined}
              />
              <Meter
                label="Temperature"
                value={gpu && gpu.temperatureC > 0 ? `${Math.round(gpu.temperatureC)}°C` : 'N/A'}
                percent={gpu?.temperatureC ?? 0}
                tone={temperature === 'warn' || temperature === 'critical' ? temperature : 'info'}
                sub={toneLabel(temperature) || undefined}
              />
              <Meter label="Power" value={gpu && gpu.powerW > 0 ? `${Math.round(gpu.powerW)} W` : 'N/A'} percent={gpu && gpu.powerW > 0 ? Math.min(100, gpu.powerW / 3.5) : 0} />
            </div>
            {!gpu?.available && (
              <Notice tone="warn" className="mt-4">GPU telemetry is unavailable from the configured provider. Inference can continue without it.</Notice>
            )}
          </section>
        ) : (
          <RuntimeList title="Dictation runtimes" models={scopedModels} emptyTitle="No dictation runtimes configured" />
        )}

        <section>
          <SectionTitle title={section === 'llm' ? 'Host' : `${label} host resources`} aside={cpu?.name} />
          <div className="mt-4 space-y-4">
            <Meter
              label="System RAM"
              value={memoryPercent == null ? 'N/A' : `${Math.round(memoryPercent)}%`}
              percent={memoryPercent ?? 0}
              tone={memoryPercent != null && threshold(memoryPercent) !== 'good' ? threshold(memoryPercent) : 'info'}
              sub={memory ? `${formatBytes(memory.used)} of ${formatBytes(memory.total)}` : undefined}
            />
            <Fact label="Logical processors" value={cpu ? String(cpu.logicalProcessors) : 'N/A'} />
            <Fact label="Gateway uptime" value={formatUptime(stats?.uptimeSeconds ?? status?.uptime)} />
          </div>
        </section>
      </div>

      {(section === 'image' || section === 'music' || section === 'video') && (
        <RuntimeList title={`${label} runtimes`} models={scopedModels} emptyTitle={`No ${label.toLowerCase()} runtimes configured`} />
      )}
      {section === 'dictation' && <MediaJobsPanel showEmpty />}
      {section === 'image' && (
        <MediaJobsPanel
          modalities={['image']}
          title="Image generation health"
          emptyTitle="No image generation jobs"
          emptyDetail="Completed, running, and failed image jobs appear here."
          showEmpty
        />
      )}
      {section === 'video' && (
        <MediaJobsPanel
          modalities={['video_generation']}
          title="Video generation health"
          emptyTitle="No video generation jobs"
          emptyDetail="Completed, running, and failed video jobs appear here."
          showEmpty
        />
      )}
      {section === 'music' && (
        <MediaJobsPanel
          modalities={['audio_generation']}
          title="Music generation health"
          emptyTitle="No music generation jobs"
          emptyDetail="Completed, running, and failed music jobs appear here."
          showEmpty
        />
      )}
      <LogPanel />
      <ConfigPanel />
    </div>
  );
};

const LogPanel: React.FC = () => {
  const [lines, setLines] = useState<string[] | null>(null);
  const [limit, setLimit] = useState(250);
  const [paused, setPaused] = useState(false);
  const [loadError, setLoadError] = useState('');

  const fetchLogs = useCallback(async () => {
    try {
      setLines(await getLogs(limit));
      setLoadError('');
    } catch (error) {
      setLoadError(error instanceof Error ? error.message : 'Gateway logs are unavailable');
    }
  }, [limit]);

  useEffect(() => {
    void fetchLogs();
    if (paused) return;
    const timer = setInterval(() => { void fetchLogs(); }, LOG_POLL_MS);
    return () => clearInterval(timer);
  }, [fetchLogs, paused]);

  const entries = (lines ?? []).map(parseDashboardLogLine);
  const issues = entries
    .filter(entry => entry.level === 'warn' || entry.level === 'error' || entry.level === 'critical')
    .reverse();
  const warningCount = issues.filter(entry => entry.level === 'warn').length;
  const errorCount = issues.length - warningCount;

  return (
    <section aria-label="Warnings and errors">
      <div className="flex flex-wrap items-end justify-between gap-3 pb-2">
        <div>
          <h2 className="text-sm font-semibold">Warnings &amp; errors</h2>
          <p className="text-xs text-text-muted">
            {errorCount} error{errorCount === 1 ? '' : 's'}, {warningCount} warning{warningCount === 1 ? '' : 's'}, most recent first
          </p>
        </div>
        <div className="flex flex-wrap items-center justify-end gap-2">
          <select
            aria-label="Log history"
            className="h-8 px-2.5 text-sm"
            value={limit}
            onChange={event => setLimit(Number(event.target.value))}
          >
            {[100, 250, 500, 1000].map(value => <option key={value} value={value}>Last {value} lines</option>)}
          </select>
          <Button onClick={() => setPaused(current => !current)}>
            {paused ? 'Resume updates' : 'Pause updates'}
          </Button>
        </div>
      </div>
      {loadError && (
        <Notice tone="critical" role="alert" className="mb-2">
          Alert updates failed: {loadError}{lines ? '. Showing the last successful result.' : ''}{' '}
          <button type="button" className="font-medium text-text-primary underline" onClick={() => { void fetchLogs(); }}>Retry</button>
        </Notice>
      )}
      {lines === null && !loadError ? (
        <p className="rounded-md border border-dashed border-border-slate py-8 text-center text-sm text-text-muted" role="status">Checking recent gateway alerts…</p>
      ) : lines !== null && issues.length === 0 ? (
        <EmptyState title="No warnings or errors" detail={`Checked the last ${limit} gateway log lines.`} />
      ) : issues.length > 0 ? (
        <div role="log" aria-live="polite" aria-label="Gateway alerts" className="divide-y divide-border-slate rounded-lg border border-border-slate bg-panel-slate shadow-card">
          {issues.map((entry, index) => {
            const critical = entry.level === 'error' || entry.level === 'critical';
            return (
              <div key={`${entry.timestampUnixMs}:${entry.event}:${index}`} className="grid gap-x-3 gap-y-1 px-3 py-2 sm:grid-cols-[88px_minmax(0,1fr)_auto]">
                <span><Badge label={entry.level === 'critical' ? 'Critical' : critical ? 'Error' : 'Warning'} tone={critical ? 'critical' : 'warn'} /></span>
                <div className="min-w-0">
                  <p className="break-words text-sm text-text-primary">{entry.message || 'No detail was recorded.'}</p>
                  <p className="font-mono text-xs text-text-muted">{entry.event}</p>
                </div>
                <span className="tabular text-xs text-text-muted">
                  {entry.timestampUnixMs ? new Date(entry.timestampUnixMs).toLocaleTimeString() : 'Recent'}
                </span>
              </div>
            );
          })}
        </div>
      ) : null}
      <details className="group mt-3">
        <summary className="flex cursor-pointer list-none items-center gap-2 py-2 text-sm text-text-secondary hover:text-text-primary">
          <ChevronRightIcon className="h-4 w-4 text-text-muted transition-transform group-open:rotate-90" aria-hidden="true" />
          Full gateway log <span className="text-xs text-text-muted">(last {limit} lines)</span>
        </summary>
        <pre className="mt-1 h-[380px] overflow-auto rounded-md border border-border-slate bg-deck-navy p-3 font-mono text-xs leading-5 text-text-secondary">
          {lines?.length ? lines.join('\n') : 'No log lines available.'}
        </pre>
      </details>
    </section>
  );
};
