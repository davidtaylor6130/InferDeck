import React, { useCallback, useEffect, useRef, useState } from 'react';
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
      <LogPanel
        title={section === 'dictation' ? 'Dictation and gateway log' : 'LLM and gateway log'}
        collapsed={section === 'dictation'}
      />
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

const LogPanel: React.FC<{ title: string; collapsed?: boolean }> = ({ title, collapsed }) => {
  const [lines, setLines] = useState<string[]>([]);
  const [limit, setLimit] = useState(250);
  const [paused, setPaused] = useState(false);
  const [follow, setFollow] = useState(true);
  const scrollRef = useRef<HTMLPreElement | null>(null);

  const fetchLogs = useCallback(async () => {
    try {
      setLines(await getLogs(limit));
    } catch {
      // Keep the last good lines during transient gateway failures.
    }
  }, [limit]);

  useEffect(() => {
    void fetchLogs();
    if (paused) return;
    const timer = setInterval(() => { void fetchLogs(); }, LOG_POLL_MS);
    return () => clearInterval(timer);
  }, [fetchLogs, paused]);

  useEffect(() => {
    if (follow && scrollRef.current) scrollRef.current.scrollTop = scrollRef.current.scrollHeight;
  }, [lines, follow]);

  const content = (
    <>
      <SectionTitle
        title={title}
        aside={`last ${limit} lines`}
        action={
          <div className="flex items-center gap-2">
            <select
              className="h-8 rounded-md border border-white/10 bg-[#0b1626] px-2 text-xs text-text-primary"
              value={limit}
              onChange={event => setLimit(Number(event.target.value))}
            >
              {[100, 250, 500, 1000].map(value => <option key={value} value={value}>{value}</option>)}
            </select>
            <Button onClick={() => setFollow(current => !current)}>{follow ? 'Following' : 'Follow'}</Button>
            <Button onClick={() => setPaused(current => !current)}>{paused ? 'Resume' : 'Pause'}</Button>
          </div>
        }
      />
      <pre
        ref={scrollRef}
        className="mt-3 h-[420px] overflow-auto border border-border-slate bg-[#0b1017] p-3 font-mono text-[12px] leading-5 text-text-secondary"
      >
        {lines.length ? lines.join('\n') : 'No log lines available.'}
      </pre>
    </>
  );

  if (collapsed) {
    return (
      <Panel>
        <details>
          <summary className="cursor-pointer text-sm font-medium text-text-secondary">
            Advanced dictation diagnostics
            <span className="ml-1 text-xs font-normal text-text-muted">(logs collapsed)</span>
          </summary>
          <div className="mt-3 border-l border-border-slate pl-4">{content}</div>
        </details>
      </Panel>
    );
  }

  return (
    <Panel>
      {content}
    </Panel>
  );
};
