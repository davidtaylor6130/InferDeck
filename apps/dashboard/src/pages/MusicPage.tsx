import React, { useEffect, useMemo, useRef, useState } from 'react';
import { generateMusic } from '../api';
import { Badge, Button, Panel, SectionTitle } from '../components/ui';
import { useGateway } from '../gateway';
import type { ModelInfo } from '../types';
import { MediaJobsPanel } from './MediaJobsPanel';

const controlClass =
  'mt-1 min-h-11 w-full border border-white/15 bg-[#07101d] px-3 py-2 text-sm text-text-primary focus:border-queue-blue focus:outline-none sm:min-h-10';

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

  return (
    <div className="space-y-5">
      <Panel className="border-t-0 pt-0">
        <SectionTitle
          title="Generate music"
          aside={generators.length ? `${generators.length} model${generators.length === 1 ? '' : 's'}` : 'not configured'}
          action={<Badge label={state.label} tone={state.tone} />}
        />
        {generators.length === 0 ? (
          <p className="mt-4 border-l-2 border-danger-rose pl-3 text-sm text-danger-rose" role="alert">
            No music generation model is configured in the active profile.
          </p>
        ) : (
          <form className="mt-4 space-y-5" onSubmit={submit}>
            <div className="grid gap-5 lg:grid-cols-2">
              <label className="block text-xs font-medium text-text-secondary" htmlFor="music-prompt">
                Prompt
                <textarea
                  id="music-prompt"
                  value={prompt}
                  onChange={event => setPrompt(event.target.value)}
                  maxLength={4096}
                  rows={5}
                  placeholder="Describe the sound, mood, instruments, and tempo"
                  className={controlClass + ' resize-y'}
                />
              </label>
              <label className="block text-xs font-medium text-text-secondary" htmlFor="music-lyrics">
                Lyrics <span className="font-normal text-text-muted">(optional)</span>
                <textarea
                  id="music-lyrics"
                  value={lyrics}
                  onChange={event => setLyrics(event.target.value)}
                  maxLength={32_768}
                  rows={5}
                  placeholder="Leave blank for instrumental music"
                  className={controlClass + ' resize-y'}
                />
              </label>
            </div>
            <div className="grid gap-4 sm:grid-cols-2 lg:grid-cols-4">
              <label className="block text-xs font-medium text-text-secondary">
                Model
                <select
                  value={model}
                  onChange={event => setModel(event.target.value)}
                  className={controlClass}
                >
                  {generators.map(entry => (
                    <option key={entry.id} value={entry.id}>
                      {entry.id}{entry.runtime_available === false ? ' (runtime unavailable)' : ''}
                    </option>
                  ))}
                </select>
              </label>
              <label className="block text-xs font-medium text-text-secondary">
                Duration (seconds)
                <input
                  type="number"
                  min={10}
                  max={600}
                  step={1}
                  value={duration}
                  onChange={event => setDuration(Number(event.target.value))}
                  className={controlClass}
                />
              </label>
              <label className="block text-xs font-medium text-text-secondary">
                Seed
                <input
                  type="number"
                  min={-1}
                  max={4_294_967_295}
                  step={1}
                  value={seed}
                  onChange={event => setSeed(Number(event.target.value))}
                  className={controlClass}
                />
                <span className="mt-1 block font-normal text-text-muted">-1 chooses a random seed</span>
              </label>
              <div className="flex items-end">
                <Button type="submit" tone="blue" disabled={!canGenerate} className="w-full">
                  {running ? 'Generating music…' : 'Generate music'}
                </Button>
              </div>
            </div>
            <details className="border-t border-white/10 pt-3">
              <summary className="min-h-11 cursor-pointer py-3 text-xs font-medium text-text-secondary sm:min-h-10">
                Advanced generation settings
              </summary>
              <div className="grid gap-4 pb-2 sm:grid-cols-2">
                <label className="block text-xs font-medium text-text-secondary">
                  Steps
                  <input
                    type="number"
                    min={0}
                    max={100}
                    step={1}
                    value={steps}
                    onChange={event => setSteps(Number(event.target.value))}
                    className={controlClass}
                  />
                  <span className="mt-1 block font-normal text-text-muted">0 uses the runtime default</span>
                </label>
                <label className="block text-xs font-medium text-text-secondary">
                  Guidance scale
                  <input
                    type="number"
                    min={0}
                    max={50}
                    step={0.1}
                    value={guidanceScale}
                    onChange={event => setGuidanceScale(Number(event.target.value))}
                    className={controlClass}
                  />
                  <span className="mt-1 block font-normal text-text-muted">0 uses the runtime default</span>
                </label>
              </div>
            </details>
            <div>
              <p className="mb-3 text-xs text-text-muted">
                InferDeck loads the selected GPU model before generation. Saved WAV files remain in media history after a restart.
              </p>
              {running && (
                <Button
                  tone="danger"
                  className="w-full sm:w-auto"
                  onClick={() => controller.current?.abort()}
                >
                  Cancel request
                </Button>
              )}
              {connection !== 'connected' && (
                <p className="mt-3 text-sm text-warning-amber" role="status">
                  The gateway must be connected before generation can start.
                </p>
              )}
              {error && <p className="mt-3 text-sm text-danger-rose" role="alert">{error}</p>}
              {result && <p className="mt-3 text-sm text-success-green" role="status">{result}</p>}
            </div>
          </form>
        )}
      </Panel>
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
