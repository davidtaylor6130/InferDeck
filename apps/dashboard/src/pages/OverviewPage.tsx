import React from 'react';
import { useGateway } from '../gateway';
import { Badge, Button, EmptyState, Panel, ProgressBar, SectionTitle, Sparkline, Stat } from '../components/ui';
import {
  compactModel,
  formatDuration,
  formatMb,
  formatTokenCount,
  formatUptime,
  temperatureTone,
  threshold,
  timeAgo,
  toneText,
} from '../utils';

export const OverviewPage: React.FC = () => {
  const { stats, statsHistory, status, models, swap, activity, cancelSwap, unload } = useGateway();

  const loadedModel = stats?.loadedModel || status?.current || '';
  const loadedInfo = models.find(model => model.id === loadedModel);
  const gpu = stats?.gpu;
  const history = statsHistory.slice(-60);
  const summary = status?.summary;
  const swapElapsed = swap.swapping && swap.startedUnixMs > 0 ? Date.now() - swap.startedUnixMs : 0;

  return (
    <div className="space-y-4">
      <section className="grid gap-4 xl:grid-cols-[1.2fr_2fr]">
        <Panel>
          <SectionTitle title="Model" />
          <div className="mt-3 space-y-3">
            {swap.swapping ? (
              <>
                <div className="flex flex-wrap items-center gap-2">
                  <h3 className="font-mono text-sm font-semibold text-text-primary">{compactModel(swap.target)}</h3>
                  <Badge label="Swapping" tone="info" />
                </div>
                <ProgressBar percent={0} tone="info" indeterminate />
                <p className="text-xs text-text-muted">
                  {swap.from ? `Replacing ${compactModel(swap.from)} · ` : ''}
                  {swapElapsed > 0 ? `${Math.round(swapElapsed / 1000)}s elapsed` : 'starting'}
                </p>
                <Button tone="danger" onClick={() => void cancelSwap()}>Cancel swap</Button>
              </>
            ) : loadedModel ? (
              <>
                <div className="flex flex-wrap items-center gap-2">
                  <h3 className="truncate font-mono text-sm font-semibold text-text-primary" title={loadedModel}>{compactModel(loadedModel)}</h3>
                  <Badge label="Loaded" tone="good" />
                  {loadedInfo?.has_vision && <Badge label="Vision" tone="violet" />}
                </div>
                <dl className="grid grid-cols-2 gap-3 text-sm">
                  <ModelFact label="Context" value={loadedInfo ? `${formatTokenCount(loadedInfo.context_size)} tokens` : 'N/A'} />
                  <ModelFact label="Slots" value={loadedInfo ? String(loadedInfo.n_slots) : 'N/A'} />
                  <ModelFact label="VRAM budget" value={loadedInfo ? formatMb(loadedInfo.vram_required_mb) : 'N/A'} />
                  <ModelFact label="Active requests" value={String(stats?.activeRequests ?? 0)} />
                </dl>
                <div className="grid grid-cols-2 gap-2">
                  <Button onClick={() => void unload()}>Unload</Button>
                  <Button tone="blue" onClick={() => { window.location.hash = '#models'; }}>Models</Button>
                </div>
              </>
            ) : (
              <>
                <div className="flex flex-wrap items-center gap-2">
                  <h3 className="text-sm font-semibold text-text-secondary">No model loaded</h3>
                  <Badge label="Standby" tone="idle" />
                </div>
                {swap.lastError && <p className="text-xs text-danger-rose">Last swap failed: {swap.lastError}</p>}
                <Button tone="blue" onClick={() => { window.location.hash = '#models'; }}>Load a model</Button>
              </>
            )}
          </div>
        </Panel>

        <Panel>
          <SectionTitle title="Live" aside="last 60s" />
          <div className="mt-3 grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
            <Sparkline
              label="GPU utilization"
              display={gpu ? `${Math.round(gpu.utilizationPct)}%` : 'N/A'}
              values={history.map(item => item.gpu.utilizationPct)}
              tone={threshold(gpu?.utilizationPct)}
              yMax={100}
            />
            <Sparkline
              label="VRAM used"
              display={gpu ? formatMb(gpu.vramUsedMb) : 'N/A'}
              values={history.map(item => item.gpu.vramUsedMb)}
              tone="violet"
            />
            <Sparkline
              label="GPU temp"
              display={gpu && gpu.temperatureC > 0 ? `${Math.round(gpu.temperatureC)}°C` : 'N/A'}
              values={history.map(item => item.gpu.temperatureC)}
              tone={temperatureTone(gpu?.temperatureC)}
              yMax={100}
            />
            <Sparkline
              label="Active requests"
              display={String(stats?.activeRequests ?? 0)}
              values={history.map(item => item.activeRequests)}
              tone="info"
              yMax={Math.max(2, ...history.map(item => item.activeRequests))}
            />
          </div>
        </Panel>
      </section>

      <Panel>
        <SectionTitle title="Lifetime" aside={stats ? `up ${formatUptime(stats.uptimeSeconds)}` : undefined} />
        <div className="mt-3 grid grid-cols-2 gap-4 sm:grid-cols-4 xl:grid-cols-7">
          <Stat label="Requests" value={String(stats?.totalRequests ?? summary?.totalRequests ?? 0)} />
          <Stat label="Tokens in" value={formatTokenCount(stats?.lifetimeTokensIn ?? summary?.promptTokens)} />
          <Stat label="Tokens out" value={formatTokenCount(stats?.lifetimeTokensOut ?? summary?.completionTokens)} />
          <Stat label="Avg t/s" value={(stats?.avgTokensPerSecond ?? 0).toFixed(1)} />
          <Stat label="p50 latency" value={formatDuration(summary?.p50LatencyMs)} />
          <Stat label="p95 latency" value={formatDuration(summary?.p95LatencyMs)} />
          <Stat label="Swaps" value={String(stats?.totalSwaps ?? 0)} />
        </div>
      </Panel>

      <Panel>
        <SectionTitle title="Recent activity" />
        <div className="mt-2 divide-y divide-white/10">
          {activity.length === 0 ? (
            <div className="py-4"><EmptyState title="No activity yet" detail="Completed requests and model swaps appear here in real time." /></div>
          ) : (
            activity.slice(0, 10).map(item => (
              <div key={item.id} className="flex min-w-0 items-center gap-3 py-2.5">
                <span className={`h-2 w-2 shrink-0 rounded-full ${item.tone === 'critical' ? 'bg-danger-rose' : item.tone === 'good' ? 'bg-success-green' : item.tone === 'info' ? 'bg-queue-blue' : 'bg-text-muted'}`} />
                <div className="min-w-0 flex-1">
                  <p className="truncate text-sm text-text-primary">{item.label}</p>
                  {item.detail && <p className={`truncate text-xs ${item.tone === 'critical' ? toneText('critical') : 'text-text-muted'}`}>{item.detail}</p>}
                </div>
                <span className="shrink-0 text-xs text-text-muted">{timeAgo(item.timestampUnixMs)}</span>
              </div>
            ))
          )}
        </div>
      </Panel>
    </div>
  );
};

const ModelFact: React.FC<{ label: string; value: string }> = ({ label, value }) => (
  <div className="min-w-0">
    <dt className="text-xs text-text-muted">{label}</dt>
    <dd className="mt-0.5 truncate text-sm text-text-primary" title={value}>{value}</dd>
  </div>
);
