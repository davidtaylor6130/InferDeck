import React, { useCallback, useEffect, useRef, useState } from 'react';
import { getLogs } from '../api';
import { Button, Panel, ProgressBar, SectionTitle, Stat } from '../components/ui';
import { useGateway } from '../gateway';
import { clamp, formatBytes, formatMb, formatUptime, temperatureTone, threshold, toneText } from '../utils';

const LOG_POLL_MS = 5_000;

export const SystemPage: React.FC = () => {
  const { stats, status } = useGateway();
  const gpu = stats?.gpu;
  const memory = status?.hardware?.memory;
  const cpu = status?.hardware?.cpu;
  const memoryPercent = memory ? clamp(memory.percentage, 0, 100) : null;

  return (
    <div className="space-y-4">
      <section className="grid gap-4 xl:grid-cols-2">
        <Panel>
          <SectionTitle title="GPU" aside={gpu?.name || status?.hardware?.provider} />
          <div className="mt-3 space-y-4">
            <MeterRow
              label="Utilization"
              display={gpu ? `${Math.round(gpu.utilizationPct)}%` : 'N/A'}
              percent={gpu?.utilizationPct ?? 0}
              toneValue={gpu?.utilizationPct}
            />
            <MeterRow
              label="VRAM"
              display={gpu ? formatMb(gpu.vramUsedMb) : 'N/A'}
              percent={0}
              hideBar
            />
            <div className="grid grid-cols-2 gap-4">
              <Stat
                label="Temperature"
                value={gpu && gpu.temperatureC > 0 ? `${Math.round(gpu.temperatureC)}°C` : 'N/A'}
                tone={temperatureTone(gpu?.temperatureC)}
              />
              <Stat label="Power" value={gpu && gpu.powerW > 0 ? `${Math.round(gpu.powerW)} W` : 'N/A'} />
            </div>
            {!gpu?.available && (
              <p className="text-xs text-warning-amber">GPU telemetry unavailable — check the ADLX helper configuration in gateway.yml.</p>
            )}
          </div>
        </Panel>

        <Panel>
          <SectionTitle title="Host" aside={cpu?.name} />
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

      <LogPanel />
    </div>
  );
};

const MeterRow: React.FC<{
  label: string;
  display: string;
  percent: number;
  toneValue?: number | null;
  hideBar?: boolean;
}> = ({ label, display, percent, toneValue, hideBar }) => (
  <div>
    <div className="mb-1 flex items-baseline justify-between gap-3">
      <span className="text-xs text-text-muted">{label}</span>
      <span className={`text-sm font-semibold ${toneValue != null ? toneText(threshold(toneValue)) : 'text-text-primary'}`}>{display}</span>
    </div>
    {!hideBar && <ProgressBar percent={percent} tone={threshold(toneValue)} />}
  </div>
);

const LogPanel: React.FC = () => {
  const [lines, setLines] = useState<string[]>([]);
  const [limit, setLimit] = useState(250);
  const [paused, setPaused] = useState(false);
  const [follow, setFollow] = useState(true);
  const scrollRef = useRef<HTMLPreElement | null>(null);

  const fetchLogs = useCallback(async () => {
    try {
      setLines(await getLogs(limit));
    } catch {
      // keep last lines on transient failures
    }
  }, [limit]);

  useEffect(() => {
    void fetchLogs();
    if (paused) return;
    const timer = setInterval(() => { void fetchLogs(); }, LOG_POLL_MS);
    return () => clearInterval(timer);
  }, [fetchLogs, paused]);

  useEffect(() => {
    if (follow && scrollRef.current) {
      scrollRef.current.scrollTop = scrollRef.current.scrollHeight;
    }
  }, [lines, follow]);

  return (
    <Panel>
      <SectionTitle
        title="Gateway log"
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
        className="mt-3 h-[420px] overflow-auto rounded-lg border border-white/10 bg-[#05080f] p-3 font-mono text-[12px] leading-5 text-text-secondary"
      >
        {lines.length ? lines.join('\n') : 'No log lines available.'}
      </pre>
    </Panel>
  );
};
