import React, { useEffect, useMemo, useRef, useState } from 'react';
import { generateMusic } from '../api';
import { Badge, Button, GroupList, Notice, PageHeader } from '../components/ui';
import { useGateway } from '../gateway';
import type { ModelInfo } from '../types';
import { MediaJobsPanel } from './MediaJobsPanel';


function musicModels(models: ModelInfo[]): ModelInfo[] {
  return models.filter(model =>
    model.modality === 'audio_generation' ||
    model.capabilities?.includes('audio_generation'));
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

export const MusicPage: React.FC = () => {
  const { connection, models } = useGateway();
  const generators = useMemo(() => musicModels(models), [models]);
  const [model, setModel] = useState(() =>
    generators.find(entry => entry.runtime_available !== false)?.id ??
    generators[0]?.id ??
    '',
  );
  const [prompt, setPrompt] = useState('');
  const [lyrics, setLyrics] = useState('');
  const [duration, setDuration] = useState(10);
  const [seed, setSeed] = useState(-1);
  const [steps, setSteps] = useState(0);
  const [guidanceScale, setGuidanceScale] = useState(0);
  const [running, setRunning] = useState(false);
  const [error, setError] = useState('');
  const [result, setResult] = useState('');
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

  useEffect(
    () => () => controller.current?.abort(),
    [],
  );

  const selected = generators.find(entry => entry.id === model);
  const state = readiness(selected);
  const validNumbers =
    Number.isFinite(duration) && duration >= 10 && duration <= 600 &&
    Number.isInteger(seed) && seed >= -1 && seed <= 4_294_967_295 &&
    Number.isInteger(steps) && steps >= 0 && steps <= 100 &&
    Number.isFinite(guidanceScale) &&
    guidanceScale >= 0 && guidanceScale <= 50;
  const canGenerate =
    connection === 'connected' &&
    Boolean(selected) &&
    selected?.runtime_available !== false &&
    prompt.trim().length > 0 &&
    validNumbers &&
    !running;

  const submit = async (event: React.FormEvent) => {
    event.preventDefault();
    if (!canGenerate) return;
    const activeController = new AbortController();
    controller.current = activeController;
    setRunning(true);
    setError('');
    setResult('');
    try {
      const generated = await generateMusic(
        {
          model,
          prompt: prompt.trim(),
          lyrics: lyrics.trim(),
          duration,
          seed,
          steps,
          guidance_scale: guidanceScale,
        },
        activeController.signal,
      );
      const details = [
        generated.jobId ? `job ${generated.jobId}` : '',
        generated.durationSeconds != null
          ? `${generated.durationSeconds.toFixed(1)} sec`
          : '',
        generated.seed != null ? `seed ${generated.seed}` : '',
      ].filter(Boolean).join(' / ');
      setResult(
        details
          ? `Music generation completed (${details}) and was saved below.`
          : 'Music generation completed and was saved below.',
      );
      setRefreshToken(value => value + 1);
    } catch (reason) {
      const aborted =
        reason instanceof DOMException && reason.name === 'AbortError';
      setError(
        aborted
          ? 'Music generation was cancelled.'
          : reason instanceof Error
            ? reason.message
            : 'Music generation failed.',
      );
      setRefreshToken(value => value + 1);
    } finally {
      if (controller.current === activeController) controller.current = null;
      setRunning(false);
    }
  };

  const fieldClass = 'tabular h-8 w-36 px-2.5 text-right text-sm';

  return (
    <div className="space-y-8">
      <PageHeader
        title="Generate music"
        subtitle={generators.length ? `${generators.length} model${generators.length === 1 ? '' : 's'} available. Saved WAV files remain in media history after a restart.` : 'No music model is configured.'}
        actions={<Badge label={state.label} tone={state.tone} />}
      />
      {generators.length === 0 ? (
        <Notice tone="critical" role="alert">No music generation model is configured in the active profile.</Notice>
      ) : (
        <form className="space-y-4" onSubmit={submit}>
          <div>
            <div className="grid gap-4 lg:grid-cols-2">
              <label className="block text-xs font-medium text-text-muted" htmlFor="music-prompt">
                Prompt
                <textarea
                  id="music-prompt"
                  value={prompt}
                  onChange={event => setPrompt(event.target.value)}
                  maxLength={4096}
                  rows={6}
                  placeholder="Describe the sound, mood, instruments, and tempo"
                  className="mt-1.5 w-full resize-y p-3 text-base leading-relaxed text-text-primary"
                />
              </label>
              <label className="block text-xs font-medium text-text-muted" htmlFor="music-lyrics">
                Lyrics <span className="font-normal">(optional)</span>
                <textarea
                  id="music-lyrics"
                  value={lyrics}
                  onChange={event => setLyrics(event.target.value)}
                  maxLength={32_768}
                  rows={6}
                  placeholder="Leave blank for instrumental music"
                  className="mt-1.5 w-full resize-y p-3 text-base leading-relaxed text-text-primary"
                />
              </label>
            </div>
            <div className="mt-4 grid items-end gap-4 md:grid-cols-[minmax(0,1fr)_minmax(0,1.3fr)_auto]">
              <label className="block text-xs font-medium text-text-muted">
                Model
                <select value={model} onChange={event => setModel(event.target.value)} className="mt-1.5 h-10 w-full px-3 text-sm text-text-primary">
                  {generators.map(entry => (
                    <option key={entry.id} value={entry.id}>
                      {entry.id}{entry.runtime_available === false ? ' (runtime unavailable)' : ''}
                    </option>
                  ))}
                </select>
              </label>
              <label className="block text-xs font-medium text-text-muted">
                <span className="flex justify-between">Duration (seconds)<span className="tabular font-semibold text-text-primary">{duration}s</span></span>
                <input
                  type="range"
                  min={10}
                  max={600}
                  step={5}
                  value={duration}
                  onChange={event => setDuration(Number(event.target.value))}
                  className="mt-3 h-1.5 w-full cursor-pointer"
                />
              </label>
              <button
                type="submit"
                disabled={!canGenerate}
                className="inline-flex min-h-10 items-center justify-center gap-2 rounded-md bg-queue-blue px-5 text-sm font-semibold text-on-accent hover:bg-queue-blue/90 disabled:opacity-40"
              >
                {running && <span className="h-3.5 w-3.5 animate-spin rounded-full border-2 border-on-accent border-t-transparent" aria-hidden="true" />}
                {running ? 'Generating music…' : 'Generate music'}
              </button>
            </div>
          </div>

          <details className="group">
            <summary className="flex min-h-10 cursor-pointer list-none items-center gap-2 text-sm text-text-secondary hover:text-text-primary">
              <span className="text-text-muted transition-transform group-open:rotate-90" aria-hidden="true">&#8250;</span>
              Advanced generation settings
            </summary>
            <GroupList className="mt-2">
              <label className="flex min-h-11 items-center justify-between gap-4 px-3 py-2">
                <span><span className="block text-base">Seed</span><span className="block text-xs text-text-muted">-1 chooses a random seed</span></span>
                <input type="number" min={-1} max={4_294_967_295} step={1} value={seed} onChange={event => setSeed(Number(event.target.value))} className={fieldClass} />
              </label>
              <label className="flex min-h-11 items-center justify-between gap-4 px-3 py-2">
                <span><span className="block text-base">Steps</span><span className="block text-xs text-text-muted">0 uses the runtime default</span></span>
                <input type="number" min={0} max={100} step={1} value={steps} onChange={event => setSteps(Number(event.target.value))} className={fieldClass} />
              </label>
              <label className="flex min-h-11 items-center justify-between gap-4 px-3 py-2">
                <span><span className="block text-base">Guidance scale</span><span className="block text-xs text-text-muted">0 uses the runtime default</span></span>
                <input type="number" min={0} max={50} step={0.1} value={guidanceScale} onChange={event => setGuidanceScale(Number(event.target.value))} className={fieldClass} />
              </label>
            </GroupList>
          </details>

          <div className="space-y-2">
            <p className="text-xs text-text-muted">InferDeck loads the selected GPU model before generation.</p>
            {running && <Button tone="danger" onClick={() => controller.current?.abort()}>Cancel request</Button>}
            {connection !== 'connected' && <Notice tone="warn" role="status">The gateway must be connected before generation can start.</Notice>}
            {error && <Notice tone="critical" role="alert">{error}</Notice>}
            {result && <Notice tone="good" role="status">{result}</Notice>}
          </div>
        </form>
      )}
      <MediaJobsPanel
        modalities={['audio_generation']}
        title="Music history"
        emptyTitle="No music attempts yet"
        emptyDetail="Generated WAV files and failed attempts will appear here."
        showEmpty
        refreshToken={refreshToken}
      />
    </div>
  );
};
