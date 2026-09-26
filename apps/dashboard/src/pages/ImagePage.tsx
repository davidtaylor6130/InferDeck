import React, { useEffect, useMemo, useRef, useState } from 'react';
import { generateImages } from '../api';
import { Badge, Button, Panel, SectionTitle } from '../components/ui';
import { useGateway } from '../gateway';
import type { ModelInfo } from '../types';
import { MediaJobsPanel } from './MediaJobsPanel';

const controlClass =
  'mt-1 min-h-11 w-full border border-white/15 bg-[#07101d] px-3 py-2 text-sm text-text-primary focus:border-queue-blue focus:outline-none sm:min-h-10';

function imageModels(models: ModelInfo[]): ModelInfo[] {
  return models.filter(model =>
    model.modality === 'image' ||
    model.capabilities?.includes('image_generation'));
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

export const ImagePage: React.FC = () => {
  const { connection, models } = useGateway();
  const generators = useMemo(() => imageModels(models), [models]);
  const [model, setModel] = useState(() =>
    generators.find(entry => entry.runtime_available !== false)?.id ??
    generators[0]?.id ??
    '',
  );
  const [prompt, setPrompt] = useState('');
  const [size, setSize] = useState('512x512');
  const [count, setCount] = useState(1);
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
  const canGenerate =
    connection === 'connected' &&
    Boolean(selected) &&
    selected?.runtime_available !== false &&
    prompt.trim().length > 0 &&
    Number.isInteger(count) &&
    count >= 1 &&
    count <= 10 &&
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
      const generated = await generateImages(
        { model, prompt: prompt.trim(), size, n: count },
        activeController.signal,
      );
      setResult(
        generated.jobId
          ? `Image job ${generated.jobId} completed and was saved below.`
          : 'Image generation completed and was saved below.',
      );
      setRefreshToken(value => value + 1);
    } catch (reason) {
      const aborted =
        reason instanceof DOMException && reason.name === 'AbortError';
      setError(
        aborted
          ? 'Image generation was cancelled.'
          : reason instanceof Error
            ? reason.message
            : 'Image generation failed.',
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
          title="Generate image"
          aside={generators.length ? `${generators.length} model${generators.length === 1 ? '' : 's'}` : 'not configured'}
          action={<Badge label={state.label} tone={state.tone} />}
        />
        {generators.length === 0 ? (
          <p className="mt-4 border-l-2 border-danger-rose pl-3 text-sm text-danger-rose" role="alert">
            No image generation model is configured in the active profile.
          </p>
        ) : (
          <form className="mt-4 grid gap-5 lg:grid-cols-[minmax(0,1fr)_20rem]" onSubmit={submit}>
            <div className="min-w-0">
              <label className="block text-xs font-medium text-text-secondary" htmlFor="image-prompt">
                Prompt
              </label>
              <textarea
                id="image-prompt"
                value={prompt}
                onChange={event => setPrompt(event.target.value)}
                maxLength={32_000}
                rows={6}
                placeholder="Describe the image to generate"
                className={controlClass + ' resize-y'}
              />
            </div>
            <div className="grid content-start gap-4 sm:grid-cols-3 lg:grid-cols-1">
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
                Size
                <select
                  value={size}
                  onChange={event => setSize(event.target.value)}
                  className={controlClass}
                >
                  <option value="512x512">512 × 512</option>
                  <option value="768x768">768 × 768</option>
                  <option value="1024x1024">1024 × 1024</option>
                  <option value="768x512">768 × 512</option>
                  <option value="512x768">512 × 768</option>
                </select>
              </label>
              <label className="block text-xs font-medium text-text-secondary">
                Images
                <input
                  type="number"
                  min={1}
                  max={10}
                  value={count}
                  onChange={event => setCount(Number(event.target.value))}
                  className={controlClass}
                />
              </label>
            </div>
            <div className="lg:col-span-2">
              <p className="mb-3 text-xs text-text-muted">
                InferDeck loads the selected GPU model before generation. Saved outputs remain in media history after a restart.
              </p>
              <div className="flex flex-col gap-2 sm:flex-row">
                <Button type="submit" tone="blue" disabled={!canGenerate} className="w-full sm:w-auto">
                  {running ? 'Generating image…' : 'Generate image'}
                </Button>
                {running && (
                  <Button
                    tone="danger"
                    className="w-full sm:w-auto"
                    onClick={() => controller.current?.abort()}
                  >
                    Cancel request
                  </Button>
                )}
              </div>
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
        modalities={['image']}
        title="Image history"
        emptyTitle="No image attempts yet"
        emptyDetail="Generated images and failed attempts will appear here."
        showEmpty
        refreshToken={refreshToken}
      />
    </div>
  );
};
