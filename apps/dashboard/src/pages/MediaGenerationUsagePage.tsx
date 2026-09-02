import React, { useMemo, useState } from 'react';
import { UsageLineChart, UsageRangeTabs } from '../components/UsageCharts';
import { Badge, DetailItem, EmptyState, Panel, SectionTitle, Stat } from '../components/ui';
import { TOKEN_RANGE_LABELS, type TokenRange } from '../cost';
import {
  bucketUsageForSection,
  sectionLabel,
  usageForSection,
  type DashboardSection,
} from '../dashboardSections';
import { useGateway } from '../gateway';
import type { MonthlyUsageRow, UsageRow } from '../types';
import { compactModel, formatDuration, timeAgo } from '../utils';
import { MediaJobsPanel } from './MediaJobsPanel';

type MediaSection = Extract<DashboardSection, 'image' | 'music'>;
type MediaUsageRow = Pick<
  UsageRow | MonthlyUsageRow,
  'model' | 'requests' | 'successfulRequests' | 'generationDurationMs' |
  'outputAudioSeconds' | 'outputImageCount'
>;

interface MediaAggregate {
  model: string;
  requests: number;
  successful: number;
  durationMs: number;
  outputAudioSeconds: number;
  outputImageCount: number;
}

function aggregate(rows: MediaUsageRow[]): MediaAggregate {
  return rows.reduce<MediaAggregate>((total, row) => ({
    model: total.model,
    requests: total.requests + row.requests,
    successful: total.successful + row.successfulRequests,
    durationMs: total.durationMs + Number(row.generationDurationMs ?? 0),
    outputAudioSeconds: total.outputAudioSeconds + Number(row.outputAudioSeconds ?? 0),
    outputImageCount: total.outputImageCount + Number(row.outputImageCount ?? 0),
  }), {
    model: '',
    requests: 0,
    successful: 0,
    durationMs: 0,
    outputAudioSeconds: 0,
    outputImageCount: 0,
  });
}

function selectBuckets(
  range: TokenRange,
  monthly: MonthlyUsageRow[],
  daily: MonthlyUsageRow[],
  hourly: MonthlyUsageRow[],
): MonthlyUsageRow[] {
  const source = range === 'day'
    ? hourly
    : range === 'week' || range === 'month'
      ? daily
      : monthly;
  const keys = Array.from(new Set(source.map(row => row.bucket))).sort();
  const limit = range === 'week' ? 7 : range === 'year' ? 12 : 0;
  const selected = limit ? new Set(keys.slice(-limit)) : new Set(keys);
  return source.filter(row => selected.has(row.bucket));
}

function groupByModel(rows: MediaUsageRow[]): MediaAggregate[] {
  const grouped = new Map<string, MediaUsageRow[]>();
  for (const row of rows) {
    const current = grouped.get(row.model) ?? [];
    current.push(row);
    grouped.set(row.model, current);
  }
  return Array.from(grouped, ([model, modelRows]) => ({
    ...aggregate(modelRows),
    model,
  })).sort((left, right) =>
    right.requests - left.requests || left.model.localeCompare(right.model));
}

function formatAudio(seconds: number): string {
  if (!seconds) return '0 min';
  return `${(seconds / 60).toLocaleString(undefined, { maximumFractionDigits: 1 })} min`;
}

export const MediaGenerationUsagePage: React.FC<{ section: MediaSection }> = ({
  section,
}) => {
  const { status, models } = useGateway();
  const [range, setRange] = useState<TokenRange>('all');
  const label = sectionLabel(section);
  const lifetime = useMemo(
    () => usageForSection(status?.tokenUsage ?? [], models, section),
    [status?.tokenUsage, models, section],
  );
  const monthly = useMemo(
    () => bucketUsageForSection(status?.monthlyTokenUsage ?? [], models, section),
    [status?.monthlyTokenUsage, models, section],
  );
  const daily = useMemo(
    () => bucketUsageForSection(status?.dailyTokenUsage ?? [], models, section),
    [status?.dailyTokenUsage, models, section],
  );
  const hourly = useMemo(
    () => bucketUsageForSection(status?.hourlyTokenUsage ?? [], models, section),
    [status?.hourlyTokenUsage, models, section],
  );
  const buckets = useMemo(
    () => selectBuckets(range, monthly, daily, hourly),
    [range, monthly, daily, hourly],
  );
  const summaryRows: MediaUsageRow[] = range === 'all' ? lifetime : buckets;
  const totals = useMemo(() => aggregate(summaryRows), [summaryRows]);
  const perModel = useMemo(() => groupByModel(summaryRows), [summaryRows]);
  const failed = Math.max(0, totals.requests - totals.successful);
  const averageMs = totals.successful ? totals.durationMs / totals.successful : 0;
  const chart = useMemo(() => {
    const labels = Array.from(new Set(buckets.map(row => row.bucket))).sort();
    return {
      labels,
      requests: labels.map(bucket => buckets
        .filter(row => row.bucket === bucket)
        .reduce((sum, row) => sum + row.requests, 0)),
    };
  }, [buckets]);
  const modalities = section === 'image' ? ['image'] : ['audio_generation'];

  return (
    <div className="space-y-4">
      <Panel>
        <SectionTitle title={`${label} usage`} aside={TOKEN_RANGE_LABELS[range]} />
        <div className="mt-3 grid grid-cols-2 gap-4 sm:grid-cols-3 xl:grid-cols-6">
          <Stat label="Requests" value={totals.requests.toLocaleString()} />
          <Stat label="Successful" value={totals.successful.toLocaleString()} tone={totals.successful ? 'good' : 'idle'} />
          <Stat label="Failed" value={failed.toLocaleString()} tone={failed ? 'critical' : 'idle'} />
          {section === 'image'
            ? <Stat label="Images generated" value={totals.outputImageCount.toLocaleString()} />
            : <Stat label="Audio generated" value={formatAudio(totals.outputAudioSeconds)} />}
          <Stat label="Processing time" value={formatDuration(totals.durationMs)} />
          <Stat label="Average request" value={formatDuration(averageMs)} />
        </div>
        <div className="mt-4"><UsageRangeTabs value={range} onChange={setRange} /></div>
        <p className="mt-3 text-xs text-text-muted">
          Requests, outputs, and processing time come from the persisted SQL ledger.
        </p>
      </Panel>

      <Panel>
        <SectionTitle title="Request volume" aside={TOKEN_RANGE_LABELS[range]} />
        <UsageLineChart
          labels={chart.labels}
          series={[{
            label,
            color: section === 'image' ? '#60A5FA' : '#A78BFA',
            values: chart.requests,
          }]}
          ariaLabel={`${label} generation requests for ${TOKEN_RANGE_LABELS[range]}`}
        />
      </Panel>

      <Panel>
        <SectionTitle title="Per-model usage" aside={TOKEN_RANGE_LABELS[range]} />
        {perModel.length === 0 ? (
          <div className="mt-3"><EmptyState title={`No persisted ${label.toLowerCase()} usage`} /></div>
        ) : (
          <>
            <div className="mt-3 divide-y divide-white/10 md:hidden" aria-label={`Per-model ${label.toLowerCase()} usage cards`}>
              {perModel.map(row => (
                <article key={row.model} className="py-4 first:pt-0 last:pb-0">
                  <h3 className="break-words font-mono text-sm text-text-primary">{compactModel(row.model)}</h3>
                  <dl className="mt-3 grid grid-cols-2 gap-x-4 gap-y-3">
                    <DetailItem label="Requests">{row.requests.toLocaleString()}</DetailItem>
                    <DetailItem label="Success">{row.requests ? `${(row.successful / row.requests * 100).toFixed(1)}%` : 'N/A'}</DetailItem>
                    <DetailItem label={section === 'image' ? 'Images generated' : 'Audio generated'}>
                      {section === 'image' ? row.outputImageCount.toLocaleString() : formatAudio(row.outputAudioSeconds)}
                    </DetailItem>
                    <DetailItem label="Processing time">{formatDuration(row.durationMs)}</DetailItem>
                  </dl>
                </article>
              ))}
            </div>
            <div className="mt-3 hidden overflow-x-auto md:block" role="region" aria-label={`Per-model ${label.toLowerCase()} usage`} tabIndex={0}>
              <table className="w-full min-w-[720px] text-left text-sm">
                <thead>
                  <tr className="border-b border-white/10 text-xs uppercase tracking-wide text-text-muted">
                    <th className="py-2 pr-4 font-medium">Model</th>
                    <th className="py-2 pr-4 font-medium">Requests</th>
                    <th className="py-2 pr-4 font-medium">Success</th>
                    <th className="py-2 pr-4 font-medium">{section === 'image' ? 'Images' : 'Audio'}</th>
                    <th className="py-2 pr-4 font-medium">Processing time</th>
                    <th className="py-2 font-medium">Last used</th>
                  </tr>
                </thead>
                <tbody className="divide-y divide-white/5">
                  {perModel.map(row => {
                    const lifetimeRow = lifetime.find(item => item.model === row.model);
                    return (
                      <tr key={row.model}>
                        <td className="py-2.5 pr-4 font-mono text-text-primary">{compactModel(row.model)}</td>
                        <td className="py-2.5 pr-4 text-text-secondary">{row.requests.toLocaleString()}</td>
                        <td className="py-2.5 pr-4 text-text-secondary">
                          {row.requests ? `${(row.successful / row.requests * 100).toFixed(1)}%` : 'N/A'}
                        </td>
                        <td className="py-2.5 pr-4 text-text-secondary">
                          {section === 'image' ? row.outputImageCount.toLocaleString() : formatAudio(row.outputAudioSeconds)}
                        </td>
                        <td className="py-2.5 pr-4 text-text-secondary">{formatDuration(row.durationMs)}</td>
                        <td className="py-2.5 text-text-secondary">
                          {lifetimeRow?.lastTimestampUnixMs ? timeAgo(lifetimeRow.lastTimestampUnixMs) : 'Never'}
                        </td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          </>
        )}
      </Panel>

      <MediaJobsPanel
        modalities={modalities}
        title={`${label} history`}
        emptyTitle={`No ${label.toLowerCase()} attempts yet`}
        emptyDetail={`${label} generation attempts and downloadable outputs appear here.`}
        showEmpty
      />
    </div>
  );
};
