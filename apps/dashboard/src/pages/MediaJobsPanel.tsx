import React, { useCallback, useMemo, useState } from 'react';
import {
  cancelMediaJob,
  getMediaJobs,
  mediaOutputUrl,
  type MediaJob,
} from '../api';
import { ArrowDownTrayIcon, FilmIcon, MicrophoneIcon, MusicalNoteIcon, PhotoIcon, SparklesIcon } from '@heroicons/react/24/solid';
import { Badge, Button, EmptyState, GroupList, Notice, ProgressBar } from '../components/ui';

import { usePolling } from '../usePolling';

const DICTATION_MODALITIES = ['audio_speech', 'audio_transcription'];

interface MediaJobsPanelProps {
  modalities?: string[];
  title?: string;
  emptyTitle?: string;
  emptyDetail?: string;
  showEmpty?: boolean;
  refreshToken?: number;
  onJobsChange?: (jobs: MediaJob[]) => void;
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
  if (job.modality === 'video_generation') {
    const frames = job.parameters?.frames;
    const fps = job.parameters?.fps;
    return [typeof frames === 'number' ? `${frames} frames` : '', typeof fps === 'number' ? `${fps} fps` : ''].filter(Boolean).join(' / ');
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
  onJobsChange,
}) => {
  const [jobs, setJobs] = useState<MediaJob[]>([]);
  const [loadError, setLoadError] = useState('');
  const filterKey = modalities.join(',');

  const receiveJobs = useCallback((result: MediaJob[]) => {
    setJobs(result);
    onJobsChange?.(result);
    setLoadError('');
  }, [onJobsChange]);
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
    <section aria-label={title}>
      <div className="flex items-baseline justify-between gap-3 px-4 pb-1.5 pt-1">
        <h2 className="text-base font-semibold">{title}</h2>
        <span className="text-xs text-text-muted">{visibleJobs.length ? `${visibleJobs.length} recent` : 'idle'}</span>
      </div>
      {loadError && <Notice tone="critical" role="alert" className="mb-2">{loadError}</Notice>}
      {visibleJobs.length === 0 ? (
        <EmptyState icon={<SparklesIcon className="h-8 w-8" />} title={emptyTitle} detail={emptyDetail} />
      ) : (
        <GroupList>
          {visibleJobs.map(job => {
            const parameters = parameterSummary(job);
            const images = job.outputs?.filter(output => output.content_type === 'image/png') ?? [];
            const others = job.outputs?.filter(output => output.content_type !== 'image/png') ?? [];
            const Icon = job.modality === 'image' ? PhotoIcon : job.modality === 'video_generation' ? FilmIcon : job.modality === 'audio_generation' ? MusicalNoteIcon : MicrophoneIcon;
            return (
              <article key={job.id} className="px-3 py-3.5">
                <div className="flex min-w-0 items-start gap-3.5">
                  {images[0] ? (
                    <a href={mediaOutputUrl(images[0])} target="_blank" rel="noreferrer" className="shrink-0">
                      <img src={mediaOutputUrl(images[0])} alt="" loading="lazy" className="h-12 w-12 rounded-md bg-void-black object-cover" />
                    </a>
                  ) : (
                    <span className="flex h-12 w-12 shrink-0 items-center justify-center rounded-md bg-elevated-slate text-text-muted"><Icon className="h-5 w-5" /></span>
                  )}
                  <div className="min-w-0 flex-1">
                    <p className="line-clamp-2 break-words text-base font-medium text-text-primary">
                      {job.prompt || job.model}
                    </p>
                    <p className="mt-0.5 break-words text-xs text-text-muted">
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
                  <div className="mt-3 flex items-center gap-3 pl-[62px]">
                    <div className="flex-1"><ProgressBar percent={job.progress} tone="info" /></div>
                    <span className="tabular text-xs text-text-muted">{job.progress}%</span>
                  </div>
                )}
                {job.error && <p className="mt-2 pl-[62px] text-xs text-danger-rose">{job.error}</p>}
                {images.length > 0 && (
                  <div className="mt-3 grid grid-cols-2 gap-2 pl-[62px] sm:grid-cols-3 lg:grid-cols-4">
                    {images.map(output => (
                      <a key={output.url} href={mediaOutputUrl(output)} target="_blank" rel="noreferrer" className="group relative block overflow-hidden rounded-md bg-void-black">
                        <img
                          src={mediaOutputUrl(output)}
                          alt={job.prompt || 'Generated image'}
                          loading="lazy"
                          className="aspect-square w-full object-cover transition-transform duration-300 group-hover:scale-[1.03]"
                        />
                      </a>
                    ))}
                  </div>
                )}
                {others.length > 0 && (
                  <div className="mt-3 space-y-2 pl-[62px]">
                    {others.map(output => (
                      <div key={output.url} className="min-w-0">
                        {output.content_type === 'video/avi' ? (
                          <p className="text-xs text-text-muted">AVI output is download-only. Download the AVI to watch it.</p>
                        ) : output.content_type === 'video/mp4' ? (
                          <video controls preload="metadata" src={mediaOutputUrl(output)} className="max-h-[28rem] w-full rounded-md bg-void-black">Video playback is not supported by this browser.</video>
                        ) : (
                          <audio controls preload="metadata" src={mediaOutputUrl(output)} className="h-10 w-full">
                            Audio playback is not supported by this browser.
                          </audio>
                        )}
                      </div>
                    ))}
                  </div>
                )}
                {job.outputs?.length > 0 && (
                  <div className="mt-2 flex flex-wrap gap-x-4 gap-y-1 pl-[62px]">
                    {job.outputs.map(output => (
                      <a
                        key={`download:${output.url}`}
                        href={mediaOutputUrl(output)}
                        download={output.filename}
                        className="inline-flex min-h-8 items-center gap-1 text-xs font-medium text-queue-blue hover:underline"
                      >
                        <ArrowDownTrayIcon className="h-3.5 w-3.5" />Download {output.filename}
                      </a>
                    ))}
                  </div>
                )}
              </article>
            );
          })}
        </GroupList>
      )}
    </section>
  );
};
