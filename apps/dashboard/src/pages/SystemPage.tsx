import React, { useCallback, useEffect, useState } from 'react';
import { getLogs } from '../api';
import { Badge, Button, EmptyState, Panel, ProgressBar, SectionTitle, Stat } from '../components/ui';
import { modalityLabel, modelsForSection, type DashboardSection } from '../dashboardSections';
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
  toneText,
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

export const SystemPage: React.FC<{ section?: DashboardSection }> = ({ section = 'llm' }) => {
  const { stats, status, models } = useGateway();
  const gpu = stats?.gpu;
  const memory = status?.hardware?.memory;
  const cpu = status?.hardware?.cpu;
  const memoryPercent = memory ? clamp(memory.percentage, 0, 100) : null;
  const speechModels = modelsForSection(models, 'dictation');

  return (
    <div className="space-y-4">
      <section className="grid gap-4 xl:grid-cols-2">
        {section === 'llm' ? (
          <Panel>
            <SectionTitle title="LLM accelerator" aside={gpu?.name || status?.hardware?.provider} />
            <div className="mt-3 space-y-4">
              <MeterRow
                label="GPU utilization"
                display={gpu ? `${Math.round(gpu.utilizationPct)}%` : 'N/A'}
                percent={gpu?.utilizationPct ?? 0}
                tone="info"
              />
              <MeterRow
                label="VRAM"
                display={gpu ? formatMb(gpu.vramUsedMb) : 'N/A'}
                percent={gpu?.vramTotalMb ? gpu.vramUsedMb / gpu.vramTotalMb * 100 : 0}
                hideBar={!gpu?.vramTotalMb}
                toneValue={gpu?.vramTotalMb ? gpu.vramUsedMb / gpu.vramTotalMb * 100 : undefined}
              />
              <div className="grid grid-cols-2 gap-4">
                <Stat
                  label="Temperature"
                  value={gpu && gpu.temperatureC > 0 ? `${Math.round(gpu.temperatureC)}°C` : 'N/A'}
                  tone={temperatureTone(gpu?.temperatureC)}
                  statusLabel
                />
                <Stat label="Power" value={gpu && gpu.powerW > 0 ? `${Math.round(gpu.powerW)} W` : 'N/A'} />
              </div>
              {!gpu?.available && (
                <p className="text-xs text-warning-amber">
                  GPU telemetry is unavailable from the configured provider. Inference can continue without it.
                </p>
              )}
            </div>
          </Panel>
        ) : (
          <Panel>
            <SectionTitle title="Dictation runtimes" aside={`${speechModels.length} configured`} />
            {speechModels.length === 0 ? (
              <div className="mt-3"><EmptyState title="No dictation runtimes configured" /></div>
            ) : (
              <div className="mt-3 divide-y divide-white/5">
                {speechModels.map(model => (
                  <div key={model.id} className="flex min-w-0 items-center gap-3 py-2.5">
                    <div className="min-w-0 flex-1">
                      <p className="truncate font-mono text-sm text-text-primary" title={model.id}>{compactModel(model.id)}</p>
                      <p className="mt-0.5 text-xs text-text-muted">{model.runtime || 'unknown runtime'} · {modalityLabel(model.modality)}</p>
                    </div>
                    {model.runtime_available === false
                      ? <Badge label="Unavailable" tone="critical" />
                      : <Badge label={model.loaded ? 'Loaded' : 'Ready'} tone={model.loaded ? 'good' : 'idle'} />}
                  </div>
                ))}
              </div>
            )}
          </Panel>
        )}

        <Panel>
          <SectionTitle title={section === 'dictation' ? 'Dictation host resources' : 'Host'} aside={cpu?.name} />
          <div className="mt-3 space-y-4">
            <MeterRow
              label="System RAM"
              display={memory ? `${formatBytes(memory.used)} / ${formatBytes(memory.total)}` : 'N/A'}
              percent={memoryPercent ?? 0}
              toneValue={memoryPercent}
            />
            <div className="grid grid-cols-2 gap-4">
              <Stat label="Logical processors" value={cpu ? String(cpu.logicalProcessors) : 'N/A'} />
              <Stat label="Gateway uptime" value={formatUptime(stats?.uptimeSeconds ?? status?.uptime)} />
            </div>
          </div>
        </Panel>
      </section>

      {section === 'dictation' && <MediaJobsPanel showEmpty />}
      <LogPanel />
      <ConfigPanel />
    </div>
  );
};

const MeterRow: React.FC<{
  label: string;
  display: string;
  percent: number;
  toneValue?: number | null;
  tone?: 'good' | 'warn' | 'critical' | 'idle' | 'info' | 'violet';
  hideBar?: boolean;
}> = ({ label, display, percent, toneValue, tone: explicitTone, hideBar }) => {
  const tone = explicitTone ?? threshold(toneValue);
  const badgeLabel = toneValue != null && !explicitTone ? toneLabel(tone) : '';
  return (
    <div>
      <div className="mb-1 flex items-baseline justify-between gap-3">
        <span className="text-xs text-text-muted">{label}</span>
        <span className="flex items-center gap-2">
          <span className={`text-sm font-semibold ${toneValue != null ? toneText(tone) : 'text-text-primary'}`}>{display}</span>
          {badgeLabel && <Badge label={badgeLabel} tone={tone} />}
        </span>
      </div>
      {!hideBar && <ProgressBar percent={percent} tone={tone} />}
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
    <Panel>
      <SectionTitle
        title="Warnings & errors"
        aside={`${errorCount} error${errorCount === 1 ? '' : 's'} · ${warningCount} warning${warningCount === 1 ? '' : 's'}`}
        action={
          <div className="flex flex-wrap items-center justify-end gap-2">
            <select
              aria-label="Log history"
              className="min-h-10 rounded border border-white/10 bg-[#0b1626] px-2 text-xs text-text-primary"
              value={limit}
              onChange={event => setLimit(Number(event.target.value))}
            >
              {[100, 250, 500, 1000].map(value => <option key={value} value={value}>Last {value} lines</option>)}
            </select>
            <Button onClick={() => setPaused(current => !current)}>{paused ? 'Resume' : 'Pause'}</Button>
          </div>
        }
      />
      <p className="mt-2 text-xs text-text-muted">Recent gateway exceptions are shown first. Open the full log only when you need surrounding diagnostic context.</p>
      {loadError && (
        <div className="mt-3 flex flex-wrap items-center justify-between gap-2 border-l-2 border-danger-rose bg-danger-rose/10 px-3 py-2 text-xs text-danger-rose" role="alert">
          <span>Alert updates failed: {loadError}{lines ? ' · showing the last successful result' : ''}</span>
          <Button tone="danger" onClick={() => { void fetchLogs(); }}>Retry</Button>
        </div>
      )}
      {lines === null && !loadError ? (
        <p className="mt-3 border-y border-dashed border-border-slate py-8 text-center text-sm text-text-muted" role="status">Checking recent gateway alerts…</p>
      ) : lines !== null && issues.length === 0 ? (
        <div className="mt-3"><EmptyState title="No warnings or errors" detail={`Checked the last ${limit} gateway log lines.`} /></div>
      ) : issues.length > 0 ? (
        <div className="mt-3 divide-y divide-white/10 border-y border-white/10" role="log" aria-live="polite">
          {issues.map((entry, index) => {
            const critical = entry.level === 'error' || entry.level === 'critical';
            return (
              <div key={`${entry.timestampUnixMs}:${entry.event}:${index}`} className="grid gap-2 py-3 sm:grid-cols-[auto_minmax(0,1fr)_auto] sm:items-start">
                <Badge label={entry.level === 'critical' ? 'Critical' : critical ? 'Error' : 'Warning'} tone={critical ? 'critical' : 'warn'} />
                <div className="min-w-0">
                  <p className="break-words text-sm text-text-primary">{entry.message || 'No detail was recorded.'}</p>
                  <p className="mt-1 font-mono text-xs text-text-muted">{entry.event.replace(/_/g, ' ')}</p>
                </div>
                <span className="text-xs text-text-muted">
                  {entry.timestampUnixMs ? new Date(entry.timestampUnixMs).toLocaleTimeString() : 'Recent'}
                </span>
              </div>
            );
          })}
        </div>
      ) : null}
      <details className="mt-4 border-t border-border-slate pt-3">
        <summary className="cursor-pointer text-sm font-medium text-text-secondary">Full gateway log <span className="text-xs font-normal text-text-muted">(last {limit} lines)</span></summary>
        <pre className="mt-3 h-[360px] overflow-auto border border-border-slate bg-[#0b1017] p-3 font-mono text-[12px] leading-5 text-text-secondary">
          {lines?.length ? lines.join('\n') : 'No log lines available.'}
        </pre>
      </details>
    </Panel>
  );
};
