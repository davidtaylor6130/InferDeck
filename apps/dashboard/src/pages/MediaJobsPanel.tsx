import React, { useCallback, useMemo, useState } from 'react';
import {
  cancelMediaJob,
  getMediaJobs,
  mediaOutputUrl,
  type MediaJob,
} from '../api';
import {
  Badge,
  Button,
  EmptyState,
  Panel,
  ProgressBar,
  SectionTitle,
} from '../components/ui';

import { usePolling } from '../usePolling';

const DICTATION_MODALITIES = ['audio_speech', 'audio_transcription'];

interface MediaJobsPanelProps {
  modalities?: string[];
  title?: string;
  emptyTitle?: string;
  emptyDetail?: string;
  showEmpty?: boolean;
  refreshToken?: number;
}

function statusTone(state: string): 'good' | 'critical' | 'warn' | 'info' {
  if (state === 'completed') return 'good';
  if (state === 'failed') return 'critical';
  if (state === 'cancelled') return 'warn';
  return 'info';
}

function parameterSummary(job: MediaJob): string {
  if (job.modality === 'image') {
    const size = job.parameters?.size;
    const count = job.parameters?.count;
    return [size, typeof count === 'number' ? `${count} image${count === 1 ? '' : 's'}` : '']
      .filter(Boolean)
      .join(' / ');
  }
  if (job.modality === 'audio_generation') {
    const duration = job.parameters?.duration_seconds;
    const seed = job.parameters?.seed;
    return [
      typeof duration === 'number' ? `${duration.toFixed(0)} sec` : '',
      typeof seed === 'number' && seed >= 0 ? `seed ${seed}` : '',
    ].filter(Boolean).join(' / ');
  }
  return '';
}

function jobTime(job: MediaJob): string {
  if (!job.created_at_unix_ms) return '';
  return new Date(job.created_at_unix_ms).toLocaleString();
}

export const MediaJobsPanel: React.FC<MediaJobsPanelProps> = ({
  modalities = DICTATION_MODALITIES,
  title = 'Dictation jobs',
  emptyTitle = 'No dictation jobs yet',
  emptyDetail = 'Speech requests appear here while the gateway is processing them.',
  showEmpty = false,
  refreshToken = 0,
}) => {
  const [jobs, setJobs] = useState<MediaJob[]>([]);
  const [loadError, setLoadError] = useState('');
  const filterKey = modalities.join(',');

  const receiveJobs = useCallback((result: MediaJob[]) => {
    setJobs(result);
    setLoadError('');
  }, []);
  const failed = useCallback((error: unknown) => {
    setLoadError(error instanceof Error ? error.message : 'Media history is unavailable');
  }, []);
  usePolling(getMediaJobs, receiveJobs, failed, 1000, `${filterKey}:${refreshToken}`);

  const visibleJobs = useMemo(
    () => jobs.filter(job => modalities.includes(job.modality)).slice(0, 20),
    [filterKey, jobs],
  );

  if (visibleJobs.length === 0 && !showEmpty && !loadError) return null;
  return (
    <Panel>
      <SectionTitle
        title={title}
        aside={visibleJobs.length ? `${visibleJobs.length}` : 'idle'}
      />
      {loadError && (
        <p className="mt-3 border-l-2 border-danger-rose pl-3 text-sm text-danger-rose" role="alert">
          {loadError}
        </p>
      )}
      {visibleJobs.length === 0 ? (
        <div className="mt-3">
          <EmptyState title={emptyTitle} detail={emptyDetail} />
        </div>
      ) : (
        <div className="mt-3 divide-y divide-white/10 border-b border-white/10">
          {visibleJobs.map(job => {
            const parameters = parameterSummary(job);
            return (
              <article key={job.id} className="py-4">
                <div className="flex min-w-0 flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
                  <div className="min-w-0">
                    <p className="break-words text-sm font-medium text-text-primary">
                      {job.prompt || job.model}
                    </p>
                    <p className="mt-1 break-words text-xs text-text-muted">
                      {job.model}{parameters ? ` / ${parameters}` : ''}{jobTime(job) ? ` / ${jobTime(job)}` : ''}
                    </p>
                  </div>
                  <div className="flex shrink-0 items-center gap-2">
                    <Badge label={job.state} tone={statusTone(job.state)} />
                    {job.state === 'running' && (
                      <Button
                        tone="danger"
                        onClick={() => {
                          void cancelMediaJob(job.id).catch(error => {
                            setLoadError(error instanceof Error ? error.message : 'Could not cancel media job');
                          });
                        }}
                      >
                        Cancel
                      </Button>
                    )}
                  </div>
                </div>
                {job.state === 'running' && (
                  <div className="mt-3">
                    <ProgressBar percent={job.progress} tone="info" />
                    <p className="mt-1 text-right text-xs text-text-muted">{job.progress}%</p>
                  </div>
                )}
                {job.error && (
                  <p className="mt-3 border-l-2 border-danger-rose pl-3 text-xs text-danger-rose">
                    {job.error}
                  </p>
                )}
                {job.outputs?.length > 0 && (
                  <div
                    className={job.modality === 'image'
                      ? 'mt-3 grid gap-3 sm:grid-cols-2'
                      : 'mt-3 space-y-3'}
                  >
                    {job.outputs.map(output => (
                      <div key={output.url} className="min-w-0">
                        {output.content_type === 'image/png' ? (
                          <a href={mediaOutputUrl(output)} target="_blank" rel="noreferrer">
                            <img
                              src={mediaOutputUrl(output)}
                              alt={job.prompt || 'Generated image'}
                              loading="lazy"
                              className="max-h-[36rem] w-full bg-black object-contain"
                            />
                          </a>
                        ) : (
                          <audio
                            controls
                            preload="metadata"
                            src={mediaOutputUrl(output)}
                            className="w-full"
                          >
                            Audio playback is not supported by this browser.
                          </audio>
                        )}
                        <a
                          href={mediaOutputUrl(output)}
                          download={output.filename}
                          className="mt-2 inline-flex min-h-11 items-center text-xs font-medium text-queue-blue hover:underline sm:min-h-10"
                        >
                          Download {output.filename}
                        </a>
                      </div>
                    ))}
                  </div>
                )}
              </article>
            );
          })}
        </div>
      )}
    </Panel>
  );
};
