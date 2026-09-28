import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { generateVideo, mediaOutputUrl } from '../api';
import { Badge, Button, GroupList, Notice, PageHeader } from '../components/ui';
import { useGateway } from '../gateway';
import type { ModelInfo } from '../types';
import type { MediaJob } from '../api';
import { MediaJobsPanel } from './MediaJobsPanel';


export const VIDEO_JOB_MODALITIES = ['video_generation'] as const;

export const VIDEO_DEFAULTS = {
  width: 512,
  height: 320,
  frames: 33,
  fps: 24,
  steps: 20,
  seed: -1,
  guidanceScale: 6,
} as const;

export function videoModels(models: ModelInfo[]): ModelInfo[] {
  return models.filter(model =>
    model.modality === 'video' ||
    model.capabilities?.includes('video_generation'));
}

function readiness(model: ModelInfo | undefined): {
  label: string;
  tone: 'good' | 'info' | 'critical';
} {
  if (!model || model.runtime_available === false) {
    return { label: 'Runtime unavailable', tone: 'critical' };
  }
  if (model.loaded) return { label: 'Loaded', tone: 'good' };
  return { label: 'Loads on request', tone: 'info' };
}

function validVideoInput(input: VideoGenerationInput): boolean {
  return input.width >= 64 && input.width <= 1280 && input.width % 32 === 0 &&
    input.height >= 64 && input.height <= 720 && input.height % 32 === 0 &&
    input.frames >= 9 && input.frames <= 121 && (input.frames - 1) % 8 === 0 &&
    input.fps >= 1 && input.fps <= 60 && Number.isInteger(input.fps) &&
    input.steps >= 1 && input.steps <= 50 && Number.isInteger(input.steps) &&
    input.seed >= -1 && input.seed <= 4_294_967_295 && Number.isInteger(input.seed) &&
    input.guidanceScale >= 0 && input.guidanceScale <= 20;
}

type VideoGenerationInput = {
  width: number;
  height: number;
  frames: number;
  fps: number;
  steps: number;
  seed: number;
  guidanceScale: number;
};

export const VideoPage: React.FC = () => {
  const { connection, models } = useGateway();
  const generators = useMemo(() => videoModels(models), [models]);
  const [model, setModel] = useState('');
  const [prompt, setPrompt] = useState('');
  const [negativePrompt, setNegativePrompt] = useState('');
  const [width, setWidth] = useState<number>(VIDEO_DEFAULTS.width);
  const [height, setHeight] = useState<number>(VIDEO_DEFAULTS.height);
  const [frames, setFrames] = useState<number>(VIDEO_DEFAULTS.frames);
  const [fps, setFps] = useState<number>(VIDEO_DEFAULTS.fps);
  const [steps, setSteps] = useState<number>(VIDEO_DEFAULTS.steps);
  const [seed, setSeed] = useState<number>(VIDEO_DEFAULTS.seed);
  const [guidanceScale, setGuidanceScale] = useState<number>(VIDEO_DEFAULTS.guidanceScale);
  const [running, setRunning] = useState(false);
  const [error, setError] = useState('');
  const [result, setResult] = useState<{ url: string; filename: string; contentType: string } | null>(null);
  const [latestSaved, setLatestSaved] = useState<MediaJob | null>(null);
  const receiveSavedJobs = useCallback((jobs: MediaJob[]) => {
    const newest = jobs
      .filter(job => job.state === 'completed' && job.modality === 'video_generation' && job.outputs.some(output => output.content_type === 'video/mp4'))
      .sort((left, right) => right.created_at_unix_ms - left.created_at_unix_ms)[0] ?? null;
    setLatestSaved(newest);
  }, []);
  const [refreshToken, setRefreshToken] = useState(0);
  const controller = useRef<AbortController | null>(null);

  useEffect(() => {
    if (generators.some(entry => entry.id === model)) return;
    setModel(
      generators.find(entry => entry.runtime_available !== false)?.id ??
      generators[0]?.id ??
      '',
    );
  }, [generators, model]);

  useEffect(() => () => controller.current?.abort(), []);
  useEffect(() => () => { if (result) URL.revokeObjectURL(result.url); }, [result]);

  const selected = generators.find(entry => entry.id === model);
  const state = readiness(selected);
  const savedPreview = latestSaved?.outputs.find(output => output.content_type === 'video/mp4');
  const previewUrl = result?.contentType === 'video/mp4' ? result.url : savedPreview ? mediaOutputUrl(savedPreview) : null;
  const input = { width, height, frames, fps, steps, seed, guidanceScale };
  const canGenerate = connection === 'connected' && Boolean(selected) &&
    selected?.runtime_available !== false && prompt.trim().length > 0 &&
    validVideoInput(input) && !running;

  const submit = async (event: React.FormEvent) => {
    event.preventDefault();
    if (!canGenerate) return;
    const activeController = new AbortController();
    controller.current = activeController;
    setRunning(true);
    setError('');
    setResult(current => {
      if (current) URL.revokeObjectURL(current.url);
      return null;
    });
    try {
      const generated = await generateVideo(
        { model, prompt: prompt.trim(), negative_prompt: negativePrompt.trim(), ...input, guidance_scale: guidanceScale },
        activeController.signal,
      );
      const url = URL.createObjectURL(generated.video);
      setResult({ url, filename: generated.filename, contentType: generated.video.type });
      setRefreshToken(value => value + 1);
    } catch (reason) {
      const aborted = reason instanceof DOMException && reason.name === 'AbortError';
      setError(aborted ? 'Video generation was cancelled.' : reason instanceof Error ? reason.message : 'Video generation failed.');
      setRefreshToken(value => value + 1);
    } finally {
      if (controller.current === activeController) controller.current = null;
      setRunning(false);
    }
  };

  const numberRow = (label: string, value: number, set: (next: number) => void, props: React.InputHTMLAttributes<HTMLInputElement>, hint?: string) => (
    <label className="flex min-h-11 items-center justify-between gap-4 px-3 py-2">
      <span><span className="block text-sm">{label}</span>{hint && <span className="block text-xs text-text-muted">{hint}</span>}</span>
      <input type="number" value={value} onChange={event => set(Number(event.target.value))} className="tabular h-8 w-28 px-2.5 text-right text-sm" {...props} />
    </label>
  );

  return (
    <div className="space-y-8">
      <PageHeader
        title="Generate video"
        subtitle="Generate locally from a text prompt."
        actions={<Badge label={state.label} tone={state.tone} />}
      />
      <div className="grid items-start gap-6 lg:grid-cols-[minmax(0,1.2fr)_minmax(320px,0.8fr)]">
        <section className="min-w-0 space-y-5">
          <div className="relative grid aspect-video place-items-center overflow-hidden rounded-md border border-border-slate bg-deck-navy text-center">
            {previewUrl ? (
              <video controls preload="metadata" src={previewUrl} className="h-full w-full object-contain">Video playback is not supported by this browser.</video>
            ) : (
              <div>
                <strong className="block text-sm font-medium text-text-secondary">No video preview yet</strong>
                <span className="mt-0.5 block text-sm text-text-muted">Your generated video will appear here.</span>
              </div>
            )}
            {running && <div className="absolute inset-x-0 bottom-0 h-1 overflow-hidden bg-border-slate"><div className="h-full w-1/3 bg-queue-blue" /></div>}
          </div>
          <p className="text-xs text-text-muted">MP4 preview when available. AVI download.</p>
          <MediaJobsPanel modalities={[...VIDEO_JOB_MODALITIES]} title="Video history" onJobsChange={receiveSavedJobs} emptyTitle="No video attempts yet" emptyDetail="Your generated video will appear here." showEmpty refreshToken={refreshToken} />
        </section>
        <section className="min-w-0 lg:sticky lg:top-4" aria-label="Generation">
          {generators.length === 0 ? <Notice tone="critical" role="alert">No video generation model is configured in the active profile.</Notice> : (
            <form className="space-y-4" onSubmit={submit}>
              <div className="rounded-md border border-border-slate bg-panel-slate p-4">
                <label className="block text-xs font-medium text-text-muted" htmlFor="video-model">Model
                  <select id="video-model" value={model} onChange={event => setModel(event.target.value)} className="mt-1.5 h-10 w-full px-3 text-sm text-text-primary">
                    {generators.map(entry => <option key={entry.id} value={entry.id}>{entry.id}{entry.runtime_available === false ? ' (runtime unavailable)' : ''}</option>)}
                  </select>
                </label>
                <label className="mt-3 block text-xs font-medium text-text-muted" htmlFor="video-prompt">Prompt
                  <textarea id="video-prompt" value={prompt} onChange={event => setPrompt(event.target.value)} maxLength={32_000} rows={4} placeholder="Describe the shot, movement, lighting, and subject" className="mt-1.5 w-full resize-y p-3 text-base leading-relaxed text-text-primary" />
                </label>
                <label className="mt-3 block text-xs font-medium text-text-muted" htmlFor="video-negative-prompt">Negative prompt
                  <textarea id="video-negative-prompt" value={negativePrompt} onChange={event => setNegativePrompt(event.target.value)} maxLength={32_000} rows={2} placeholder="Artifacts, flicker, warped motion" className="mt-1.5 w-full resize-y p-3 text-base text-text-primary" />
                </label>
                <button
                  type="submit"
                  disabled={!canGenerate}
                  className="mt-4 inline-flex min-h-10 w-full items-center justify-center gap-2 rounded-md bg-queue-blue text-sm font-semibold text-on-accent hover:bg-queue-blue/90 disabled:opacity-40"
                >
                  {running && <span className="h-3.5 w-3.5 animate-spin rounded-full border-2 border-on-accent border-t-transparent" aria-hidden="true" />}
                  {running ? 'Generating video…' : 'Generate video'}
                </button>
                {running && <Button type="button" tone="danger" className="mt-2 w-full" onClick={() => controller.current?.abort()}>Cancel request</Button>}
              </div>
              <GroupList>
                {numberRow('Width', width, setWidth, { min: 64, max: 1280, step: 32 }, 'Multiple of 32')}
                {numberRow('Height', height, setHeight, { min: 64, max: 720, step: 32 }, 'Multiple of 32')}
                {numberRow('Frames', frames, setFrames, { min: 9, max: 121, step: 8 }, `${(frames / Math.max(1, fps)).toFixed(1)} s at ${fps} fps`)}
                {numberRow('FPS', fps, setFps, { min: 1, max: 60 })}
                {numberRow('Steps', steps, setSteps, { min: 1, max: 50 })}
                {numberRow('Seed', seed, setSeed, { min: -1, max: 4_294_967_295 }, '-1 chooses a random seed')}
                {numberRow('Guidance', guidanceScale, setGuidanceScale, { min: 0, max: 20, step: 0.1 })}
              </GroupList>
              <div className="space-y-2">
                <p className="px-1 text-xs text-text-muted">InferDeck loads the selected model before generation. You can cancel the request at any time.</p>
                {connection !== 'connected' && <Notice tone="warn" role="status">The gateway must be connected before generation can start.</Notice>}
                {error && <Notice tone="critical" role="alert">{error}</Notice>}
                {result && <Notice tone="good" role="status"><a href={result.url} download={result.filename} className="underline underline-offset-2">Download {result.filename}</a></Notice>}
              </div>
            </form>
          )}
        </section>
      </div>
    </div>
  );
};
