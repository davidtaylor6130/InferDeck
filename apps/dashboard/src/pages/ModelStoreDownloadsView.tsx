import React from 'react';
import type { StoreDownload } from '../api';
import { Badge, Button, EmptyState, ProgressBar } from '../components/ui';
import { formatBytes } from '../utils';

export const ModelStoreDownloadsView: React.FC<{
  downloads: StoreDownload[];
  onControl: (id: number, action: 'cancel' | 'resume') => void;
}> = ({ downloads, onControl }) => (
  <section aria-label="Downloads">
    <div className="flex items-baseline justify-between gap-3 pb-2">
      <h2 className="text-sm font-semibold">Downloads</h2>
      <span className="text-xs text-text-muted">{downloads.length} current and recent</span>
    </div>
    {downloads.length === 0 ? (
      <EmptyState
        title="No downloads yet"
        detail="Choose a verified model in Discover to start a background install."
      />
    ) : (
      <div className="divide-y divide-border-slate rounded-lg border border-border-slate bg-panel-slate shadow-card">
        {downloads.map(download => {
          const percent = download.bytesTotal
            ? download.bytesDownloaded / download.bytesTotal * 100
            : 0;
          const active = download.state === 'downloading' || download.state === 'queued';
          const resumable = download.state === 'failed' || download.state === 'cancelled';
          const done = download.state === 'installed';
          return (
            <div key={download.id} className="flex flex-wrap items-center gap-x-4 gap-y-2 px-3 py-2.5 sm:flex-nowrap">
              <div className="min-w-0 flex-1">
                <div className="flex min-w-0 items-center gap-2">
                  <span className="truncate font-mono text-sm text-text-primary">{download.modelName}</span>
                  <Badge
                    label={download.state}
                    tone={done ? 'good' : download.state === 'failed' ? 'critical' : download.state === 'cancelled' ? 'warn' : 'info'}
                  />
                </div>
                <p className="tabular mt-0.5 truncate text-xs text-text-muted">
                  {formatBytes(download.bytesDownloaded)} of {formatBytes(download.bytesTotal)}
                  {download.error ? `, ${download.error}` : ''}
                </p>
              </div>
              {active && (
                <div className="flex w-full items-center gap-3 sm:w-48">
                  <div className="flex-1"><ProgressBar percent={percent} tone="info" /></div>
                  <span className="tabular w-9 text-right text-xs text-text-secondary">{percent.toFixed(0)}%</span>
                </div>
              )}
              {active ? (
                <Button tone="danger" onClick={() => onControl(download.id, 'cancel')} title="Cancel download">Cancel</Button>
              ) : resumable ? (
                <Button onClick={() => onControl(download.id, 'resume')}>Resume</Button>
              ) : null}
            </div>
          );
        })}
      </div>
    )}
  </section>
);
