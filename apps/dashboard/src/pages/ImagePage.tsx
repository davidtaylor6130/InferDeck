import React, { useEffect, useMemo, useRef, useState } from 'react';
import { generateImages } from '../api';
import { Badge, Button, Notice, PageHeader } from '../components/ui';
import { useGateway } from '../gateway';
import type { ModelInfo } from '../types';
import { MediaJobsPanel } from './MediaJobsPanel';


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

function preferredImageModel(models: ModelInfo[]): string {
  return models.find(entry => entry.personal_default &&
    entry.runtime_available !== false)?.id ??
    models.find(entry => entry.runtime_available !== false)?.id ??
    models[0]?.id ?? '';
}

export const ImagePage: React.FC = () => {
  const { connection, models } = useGateway();
  const generators = useMemo(() => imageModels(models), [models]);
  const [model, setModel] = useState(() =>
    preferredImageModel(generators),
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
    setModel(preferredImageModel(generators));
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

  const sizes = ['512x512', '768x768', '1024x1024', '768x512', '512x768'];

  return (
    <div className="space-y-8">
      <PageHeader
        title="Generate image"
        subtitle={generators.length ? `${generators.length} model${generators.length === 1 ? '' : 's'} available. InferDeck loads the selected GPU model before generation.` : 'No image model is configured.'}
        actions={<Badge label={state.label} tone={state.tone} />}
      />
      {generators.length === 0 ? (
        <Notice tone="critical" role="alert">No image generation model is configured in the active profile.</Notice>
      ) : (
        <form onSubmit={submit}>
          <div className="grid gap-5 lg:grid-cols-[minmax(0,1fr)_18rem]">
            <label className="block min-w-0" htmlFor="image-prompt">
              <span className="sr-only">Prompt</span>
              <textarea
                id="image-prompt"
                value={prompt}
                onChange={event => setPrompt(event.target.value)}
                maxLength={32_000}
                rows={7}
                placeholder="Describe the image to generate"
                className="h-full min-h-[180px] w-full resize-y p-3 text-base leading-relaxed"
              />
            </label>
            <div className="flex flex-col gap-4">
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
              <div>
                <span className="block text-xs font-medium text-text-muted">Size</span>
                <div className="mt-1.5 grid grid-cols-5 gap-1.5" role="radiogroup" aria-label="Size">
                  {sizes.map(option => {
                    const [w, h] = option.split('x').map(Number);
                    const active = size === option;
                    return (
                      <button
                        key={option}
                        type="button"
                        role="radio"
                        aria-checked={active}
                        title={option.replace('x', ' × ')}
                        onClick={() => setSize(option)}
                        className={`flex h-14 flex-col items-center justify-center gap-1 rounded-md border text-2xs font-medium transition-colors ${active ? 'border-queue-blue bg-elevated-slate text-text-primary' : 'border-line-strong text-text-muted hover:text-text-secondary'}`}
                      >
                        <span className="rounded border-2 border-current" style={{ width: w / 48, height: h / 48 }} />
                        {w === h ? w : `${w}×${h}`}
                      </button>
                    );
                  })}
                </div>
              </div>
              <div className="flex items-center justify-between">
                <span className="text-xs font-medium text-text-muted">Images</span>
                <div className="flex items-center rounded-md border border-line-strong">
                  <button type="button" aria-label="Fewer images" className="h-8 w-8 text-base leading-none text-text-secondary hover:bg-elevated-slate" onClick={() => setCount(value => Math.max(1, value - 1))}>−</button>
                  <input type="number" min={1} max={10} value={count} onChange={event => setCount(Number(event.target.value))} aria-label="Images" className="tabular h-8 w-10 !rounded-none !border-y-0 !border-line-strong !bg-transparent text-center text-sm" />
                  <button type="button" aria-label="More images" className="h-8 w-8 text-base leading-none text-text-secondary hover:bg-elevated-slate" onClick={() => setCount(value => Math.min(10, value + 1))}>+</button>
                </div>
              </div>
              <div className="mt-auto flex flex-col gap-2">
                <button
                  type="submit"
                  disabled={!canGenerate}
                  className="inline-flex min-h-10 w-full items-center justify-center gap-2 rounded-md bg-queue-blue text-sm font-semibold text-on-accent hover:bg-queue-blue/90 disabled:opacity-40"
                >
                  {running && <span className="h-3.5 w-3.5 animate-spin rounded-full border-2 border-on-accent border-t-transparent" aria-hidden="true" />}
                  {running ? 'Generating image…' : 'Generate image'}
                </button>
                {running && <Button tone="danger" className="w-full" onClick={() => controller.current?.abort()}>Cancel request</Button>}
              </div>
            </div>
          </div>
          <div className="mt-3 space-y-2">
            <p className="text-xs text-text-muted">Saved outputs remain in media history after a restart.</p>
            {connection !== 'connected' && <Notice tone="warn" role="status">The gateway must be connected before generation can start.</Notice>}
            {error && <Notice tone="critical" role="alert">{error}</Notice>}
            {result && <Notice tone="good" role="status">{result}</Notice>}
          </div>
        </form>
      )}
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
