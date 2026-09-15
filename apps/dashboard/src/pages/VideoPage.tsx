import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { generateVideo, mediaOutputUrl } from '../api';
import { Badge, Button } from '../components/ui';
import { useGateway } from '../gateway';
import type { ModelInfo } from '../types';
import type { MediaJob } from '../api';
import { MediaJobsPanel } from './MediaJobsPanel';

const controlClass =
  'mt-1 min-h-11 w-full border border-white/15 bg-[#07101d] px-3 py-2 text-sm text-text-primary focus:border-queue-blue focus:outline-none sm:min-h-10';

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

  return (
    <div className="space-y-5">
      <div className="mb-1 text-xs text-text-muted">Video / Generate</div>
      <div className="flex items-end justify-between border-b border-white/10 pb-5">
        <div><h1 className="text-2xl font-medium text-text-primary">Generate video</h1><p className="mt-1 text-sm text-text-muted">Generate locally from a text prompt.</p></div>
        <span className="hidden text-xs text-text-muted sm:block"><Badge label={state.label} tone={state.tone} /></span>
      </div>
      <div className="grid gap-6 lg:grid-cols-[minmax(400px,1.15fr)_minmax(320px,.85fr)]">
        <section>
          <div className="relative grid aspect-video place-items-center border border-white/10 bg-[#050505] text-center text-xs text-text-muted">{previewUrl ? <video controls preload="metadata" src={previewUrl} className="h-full w-full object-contain">Video playback is not supported by this browser.</video> : <><div className="pointer-events-none absolute inset-3 border border-dashed border-white/10" /><div className="relative"><strong className="mb-1 block font-normal text-text-secondary">No video preview yet</strong><span>Your generated video will appear here.</span></div></>}</div>
          <div className="flex justify-between border-b border-white/10 py-3 text-xs text-text-muted"><span>Output</span><span className="text-text-secondary">MP4 preview when available · AVI download</span></div>
          <div className="mt-5"><MediaJobsPanel modalities={[...VIDEO_JOB_MODALITIES]} title="Video history" onJobsChange={receiveSavedJobs} emptyTitle="No video attempts yet" emptyDetail="Your generated video will appear here." showEmpty refreshToken={refreshToken} /></div>
        </section>
        <section className="border-t border-white/10 pt-4 lg:border-t-0 lg:pt-0"><h2 className="mb-4 text-sm font-medium text-text-primary">Generation</h2>
          {generators.length === 0 ? <p className="mt-4 border-l-2 border-danger-rose pl-3 text-sm text-danger-rose" role="alert">No video generation model is configured in the active profile.</p> : (
            <form className="grid gap-3" onSubmit={submit}>
              <label className="block text-xs font-medium text-text-secondary" htmlFor="video-model">Model<select id="video-model" value={model} onChange={event => setModel(event.target.value)} className={controlClass}>{generators.map(entry => <option key={entry.id} value={entry.id}>{entry.id}{entry.runtime_available === false ? ' (runtime unavailable)' : ''}</option>)}</select></label>
              <label className="block text-xs font-medium text-text-secondary" htmlFor="video-prompt">Prompt<textarea id="video-prompt" value={prompt} onChange={event => setPrompt(event.target.value)} maxLength={32_000} rows={4} placeholder="Describe the shot, movement, lighting, and subject" className={controlClass + ' resize-y'} /></label>
              <label className="block text-xs font-medium text-text-secondary" htmlFor="video-negative-prompt">Negative prompt<textarea id="video-negative-prompt" value={negativePrompt} onChange={event => setNegativePrompt(event.target.value)} maxLength={32_000} rows={3} placeholder="Artifacts, flicker, warped motion" className={controlClass + ' resize-y'} /></label>
              <div className="grid gap-3 sm:grid-cols-2"><label className="block text-xs font-medium text-text-secondary">Width<input type="number" min={64} max={1280} step={32} value={width} onChange={event => setWidth(Number(event.target.value))} className={controlClass} /></label><label className="block text-xs font-medium text-text-secondary">Height<input type="number" min={64} max={720} step={32} value={height} onChange={event => setHeight(Number(event.target.value))} className={controlClass} /></label><label className="block text-xs font-medium text-text-secondary">Frames<input type="number" min={9} max={121} step={8} value={frames} onChange={event => setFrames(Number(event.target.value))} className={controlClass} /></label><label className="block text-xs font-medium text-text-secondary">FPS<input type="number" min={1} max={60} value={fps} onChange={event => setFps(Number(event.target.value))} className={controlClass} /></label><label className="block text-xs font-medium text-text-secondary">Steps<input type="number" min={1} max={50} value={steps} onChange={event => setSteps(Number(event.target.value))} className={controlClass} /></label><label className="block text-xs font-medium text-text-secondary">Seed<input type="number" min={-1} max={4_294_967_295} value={seed} onChange={event => setSeed(Number(event.target.value))} className={controlClass} /></label></div>
              <label className="block text-xs font-medium text-text-secondary">Guidance<input type="number" min={0} max={20} step={0.1} value={guidanceScale} onChange={event => setGuidanceScale(Number(event.target.value))} className={controlClass} /></label>
              <div className="mt-3 flex flex-wrap gap-2"><Button type="submit" tone="blue" disabled={!canGenerate}>{running ? 'Generating video…' : 'Generate video'}</Button>{running && <Button type="button" tone="danger" onClick={() => controller.current?.abort()}>Cancel request</Button>}</div>
              <p className="text-xs text-text-muted">InferDeck loads the selected model before generation. You can cancel the request at any time.</p>{connection !== 'connected' && <p className="text-sm text-warning-amber" role="status">The gateway must be connected before generation can start.</p>}{error && <p className="text-sm text-danger-rose" role="alert">{error}</p>}{result && <p className="text-sm text-success-green" role="status"><a href={result.url} download={result.filename} className="underline underline-offset-2">Download {result.filename}</a></p>}
            </form>
          )}
        </section>
      </div>
    </div>
  );};
