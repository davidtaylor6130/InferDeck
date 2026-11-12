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

const MAX_REFERENCES = 4;
// The image route accepts a 16 MiB body; leave room for the prompt and JSON framing.
const MAX_REFERENCE_CHARS = 15 * 1024 * 1024;

interface ReferenceImage {
  name: string;
  dataUrl: string;
}

async function readReference(file: File): Promise<ReferenceImage> {
  const bytes = new Uint8Array(await file.arrayBuffer());
  let binary = '';
  for (let offset = 0; offset < bytes.length; offset += 0x8000) {
    binary += String.fromCharCode(...bytes.subarray(offset, offset + 0x8000));
  }
  return { name: file.name, dataUrl: `data:${file.type};base64,${btoa(binary)}` };
}

/**
 * Adds PNG and JPEG files to the current references, enforcing the count and
 * request-size limits. Returns the new list and a message when files were dropped.
 */
export async function mergeReferences(
  current: ReferenceImage[],
  files: File[],
): Promise<{ references: ReferenceImage[]; error: string }> {
  const accepted = files.filter(file => file.type === 'image/png' || file.type === 'image/jpeg');
  const room = MAX_REFERENCES - current.length;
  const added = await Promise.all(accepted.slice(0, room).map(readReference));
  const references = [...current, ...added];
  const size = references.reduce((total, entry) => total + entry.dataUrl.length, 0);
  if (size > MAX_REFERENCE_CHARS) {
    return { references: current, error: 'Reference images are limited to 15 MB in total.' };
  }
  if (accepted.length < files.length) {
    return { references, error: 'Only PNG and JPEG images can be used as references.' };
  }
  if (accepted.length > room) {
    return { references, error: `Up to ${MAX_REFERENCES} reference images can be used.` };
  }
  return { references, error: '' };
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
  const [references, setReferences] = useState<ReferenceImage[]>([]);
  const [dropTarget, setDropTarget] = useState(false);
  const controller = useRef<AbortController | null>(null);
  const filePicker = useRef<HTMLInputElement | null>(null);

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
  const canEdit = Boolean(selected?.capabilities?.includes('image_editing'));
  const editing = canEdit && references.length > 0;
  const action = editing ? 'Edit image' : 'Generate image';

  useEffect(() => {
    if (!canEdit) setReferences([]);
  }, [canEdit]);

  const addReferences = async (files: FileList | null) => {
    if (!canEdit || !files?.length) return;
    try {
      const merged = await mergeReferences(references, Array.from(files));
      setReferences(merged.references);
      setError(merged.error);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'The image could not be read.');
    }
  };
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
        {
          model, prompt: prompt.trim(), size, n: count,
          ...(editing ? { reference_images: references.map(entry => entry.dataUrl) } : {}),
        },
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
        title={action}
        subtitle={generators.length ? `${generators.length} model${generators.length === 1 ? '' : 's'} available. InferDeck loads the selected GPU model before generation.` : 'No image model is configured.'}
        actions={<Badge label={state.label} tone={state.tone} />}
      />
      {generators.length === 0 ? (
        <Notice tone="critical" role="alert">No image generation model is configured in the active profile.</Notice>
      ) : (
        <form onSubmit={submit}>
          <div className="grid gap-5 lg:grid-cols-[minmax(0,1fr)_18rem]">
            <div
              className={`flex min-w-0 flex-col rounded-md border bg-panel-slate transition-[border-color,box-shadow] focus-within:border-queue-blue focus-within:shadow-[0_0_0_3px_rgb(var(--accent)/0.2)] ${dropTarget ? 'border-queue-blue shadow-[0_0_0_3px_rgb(var(--accent)/0.2)]' : 'border-line-strong'}`}
              onDragOver={event => {
                if (!canEdit) return;
                event.preventDefault();
                setDropTarget(true);
              }}
              onDragLeave={() => setDropTarget(false)}
              onDrop={event => {
                if (!canEdit) return;
                event.preventDefault();
                setDropTarget(false);
                void addReferences(event.dataTransfer.files);
              }}
            >
              <label className="flex min-h-0 flex-1" htmlFor="image-prompt">
                <span className="sr-only">Prompt</span>
                <textarea
                  id="image-prompt"
                  value={prompt}
                  onChange={event => setPrompt(event.target.value)}
                  maxLength={32_000}
                  rows={6}
                  placeholder={editing ? 'Describe the change' : 'Describe the image to generate'}
                  className="min-h-[150px] w-full flex-1 resize-y !border-0 !bg-transparent p-3 text-base leading-relaxed !shadow-none"
                />
              </label>
              <div className="flex min-h-[60px] items-center gap-2 border-t border-border-slate p-2">
                {references.map((entry, index) => (
                  <div key={`${entry.name}-${index}`} className="relative h-11 w-11 shrink-0 animate-item-in overflow-hidden rounded-md bg-elevated-slate">
                    <img src={entry.dataUrl} alt={`Reference ${index + 1}: ${entry.name}`} className="h-full w-full object-cover" />
                    <span className="tabular absolute bottom-0.5 left-0.5 min-w-[16px] rounded bg-black/60 px-1 text-center text-2xs text-white">{index + 1}</span>
                    <button
                      type="button"
                      aria-label={`Remove reference ${index + 1}`}
                      onClick={() => setReferences(current => current.filter((_, position) => position !== index))}
                      className="absolute right-0.5 top-0.5 h-[18px] w-[18px] rounded-full bg-black/60 text-xs leading-[18px] text-white"
                    >×</button>
                  </div>
                ))}
                <button
                  type="button"
                  disabled={!canEdit || references.length >= MAX_REFERENCES}
                  onClick={() => filePicker.current?.click()}
                  className="inline-flex h-8 items-center gap-1.5 rounded-md border border-line-strong px-2.5 text-xs font-medium text-text-secondary hover:bg-elevated-slate disabled:cursor-not-allowed disabled:opacity-45"
                >+ Reference image</button>
                <input
                  ref={filePicker}
                  type="file"
                  accept="image/png,image/jpeg"
                  multiple
                  hidden
                  onChange={event => {
                    void addReferences(event.target.files);
                    event.target.value = '';
                  }}
                />
                <span className="tabular ml-auto text-xs text-text-muted">
                  {!canEdit
                    ? 'This model cannot edit images'
                    : references.length
                      ? `${references.length} of ${MAX_REFERENCES}`
                      : 'Drop an image to edit it'}
                </span>
              </div>
            </div>
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
                  {running ? (editing ? 'Editing image…' : 'Generating image…') : action}
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
