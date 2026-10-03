import React, { useMemo, useState } from 'react';
import { USAGE_RANGE_PHRASE, UsageHeader, UsageLineChart } from '../components/UsageCharts';
import { DetailItem, EmptyState, Panel, SectionTitle } from '../components/ui';
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

type MediaSection = Extract<DashboardSection, 'image' | 'music' | 'video'>;
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
  const modalities = section === 'image' ? ['image'] : section === 'music' ? ['audio_generation'] : ['video_generation'];

  return (
    <div className="space-y-8">
      <UsageHeader
        label={`${label} usage`}
        range={range}
        onRange={setRange}
        headline={section === 'image'
          ? `${totals.outputImageCount.toLocaleString()} image${totals.outputImageCount === 1 ? '' : 's'} generated ${USAGE_RANGE_PHRASE[range]}`
          : section === 'music'
            ? `${formatAudio(totals.outputAudioSeconds)} of music generated ${USAGE_RANGE_PHRASE[range]}`
            : `${totals.successful.toLocaleString()} video${totals.successful === 1 ? '' : 's'} generated ${USAGE_RANGE_PHRASE[range]}`}
        detail={`${totals.requests.toLocaleString()} request${totals.requests === 1 ? '' : 's'}${failed ? `, ${failed} failed` : ''}, averaging ${formatDuration(averageMs)} each. Totals come from the persisted SQL ledger.`}
      />

      <Panel>
        <SectionTitle title="Request volume" aside={TOKEN_RANGE_LABELS[range]} />
        <UsageLineChart
          labels={chart.labels}
          series={[{
            label,
            color: 'rgb(var(--series-1))',
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
            <div className="mt-3 divide-y divide-border-slate md:hidden" aria-label={`Per-model ${label.toLowerCase()} usage cards`}>
              {perModel.map(row => (
                <article key={row.model} className="py-4 first:pt-0 last:pb-0">
                  <h3 className="break-words text-sm font-semibold text-text-primary">{compactModel(row.model)}</h3>
                  <dl className="mt-3 grid grid-cols-2 gap-x-4 gap-y-3">
                    <DetailItem label="Requests">{row.requests.toLocaleString()}</DetailItem>
                    <DetailItem label="Success">{row.requests ? `${(row.successful / row.requests * 100).toFixed(1)}%` : 'N/A'}</DetailItem>
                    <DetailItem label={section === 'image' ? 'Images generated' : section === 'music' ? 'Audio generated' : 'Videos generated'}>
                      {section === 'image' ? row.outputImageCount.toLocaleString() : section === 'music' ? formatAudio(row.outputAudioSeconds) : row.successful.toLocaleString()}
                    </DetailItem>
                    <DetailItem label="Processing time">{formatDuration(row.durationMs)}</DetailItem>
                  </dl>
                </article>
              ))}
            </div>
            <div className="mt-3 hidden overflow-x-auto md:block" role="region" aria-label={`Per-model ${label.toLowerCase()} usage`} tabIndex={0}>
              <table className="w-full min-w-[720px] text-left text-sm">
                <thead>
                  <tr className="border-b border-border-slate text-xs text-text-muted">
                    <th className="py-2 pr-4 font-medium">Model</th>
                    <th className="py-2 pr-4 font-medium">Requests</th>
                    <th className="py-2 pr-4 font-medium">Success</th>
                    <th className="py-2 pr-4 font-medium">{section === 'image' ? 'Images' : section === 'music' ? 'Audio' : 'Videos'}</th>
                    <th className="py-2 pr-4 font-medium">Processing time</th>
                    <th className="py-2 font-medium">Last used</th>
                  </tr>
                </thead>
                <tbody className="divide-y divide-border-slate">
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
                          {section === 'image' ? row.outputImageCount.toLocaleString() : section === 'music' ? formatAudio(row.outputAudioSeconds) : row.successful.toLocaleString()}
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
