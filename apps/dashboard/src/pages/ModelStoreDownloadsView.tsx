import React from 'react';
import type { StoreDownload } from '../api';
import { Badge, Button, EmptyState, Panel, ProgressBar, SectionTitle } from '../components/ui';
import { formatBytes } from '../utils';

export const ModelStoreDownloadsView: React.FC<{
  downloads: StoreDownload[];
  onControl: (id: number, action: 'cancel' | 'resume') => void;
}> = ({ downloads, onControl }) => (
  <Panel className="border-t-0 pt-0">
    <SectionTitle title="Downloads" aside={`${downloads.length} current and recent`} />
    {downloads.length === 0 ? (
      <div className="mt-4">
        <EmptyState
          title="No downloads yet"
          detail="Choose a verified model in Discover to start a background install."
        />
      </div>
    ) : (
      <div className="mt-4 space-y-4">
        {downloads.map(download => {
          const percent = download.bytesTotal
            ? download.bytesDownloaded / download.bytesTotal * 100
            : 0;
          const active = download.state === 'downloading' || download.state === 'queued';
          const resumable = download.state === 'failed' || download.state === 'cancelled';
          return (
            <div key={download.id} className="border-l border-border-slate pl-3">
              <div className="flex flex-wrap items-center justify-between gap-2">
                <span className="break-all font-mono text-xs text-text-primary">
                  {download.modelName}
                </span>
                <Badge
                  label={download.state}
                  tone={download.state === 'installed'
                    ? 'good'
                    : download.state === 'failed' ? 'critical' : 'info'}
                />
              </div>
              <div className="mt-2">
                <ProgressBar
                  percent={percent}
                  tone={download.state === 'failed' ? 'critical' : 'info'}
                />
              </div>
              <div className="mt-2 flex flex-wrap items-center justify-between gap-2 text-xs text-text-muted">
                <span>
                  {formatBytes(download.bytesDownloaded)} / {formatBytes(download.bytesTotal)}
                  {download.error ? ` / ${download.error}` : ''}
                </span>
                {active ? (
                  <Button tone="danger" onClick={() => onControl(download.id, 'cancel')}>
                    Cancel
                  </Button>
                ) : resumable ? (
                  <Button onClick={() => onControl(download.id, 'resume')}>
                    Resume
                  </Button>
                ) : null}
              </div>
            </div>
          );
        })}
      </div>
    )}
  </Panel>
);
