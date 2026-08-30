import React from 'react';
import { Badge, Panel, SectionTitle } from '../components/ui';

export type FutureArea = 'image' | 'music' | 'post-training';

const AREAS: Record<FutureArea, { title: string; purpose: string; steps: string[] }> = {
  image: {
    title: 'Image',
    purpose: 'A future workspace for local image generation using the same model residency, queue, progress, and cancellation controls as the rest of InferDeck.',
    steps: ['Choose an installed image model', 'Submit a prompt and review resource requirements', 'Track generation and collect the finished image'],
  },
  music: {
    title: 'Music',
    purpose: 'A future workspace for queued local music generation with visible model state, duration, progress, and output history.',
    steps: ['Choose an installed audio model', 'Set the prompt and duration', 'Track generation and collect the finished audio'],
  },
  'post-training': {
    title: 'Post Training',
    purpose: 'A future workspace for LoRA training and quantisation jobs scheduled around interactive inference workloads.',
    steps: ['Choose a source model and training input', 'Review storage, memory, and time requirements', 'Run, pause, and inspect the resulting artefact'],
  },
};

export const FutureWorkspacePage: React.FC<{ area: FutureArea }> = ({ area }) => {
  const content = AREAS[area];
  return (
    <div className="task-view">
      <Panel>
        <div className="flex flex-wrap items-start justify-between gap-3">
          <SectionTitle title={content.title} />
          <Badge label="Planned" tone="idle" />
        </div>
        <p className="mt-3 max-w-2xl text-sm text-text-secondary">{content.purpose}</p>
        <div className="mt-6 border-t border-border-slate pt-4">
          <h3 className="text-sm font-semibold text-text-primary">Planned workflow</h3>
          <ol className="mt-3 space-y-3">
            {content.steps.map((step, index) => (
              <li key={step} className="grid grid-cols-[2rem_1fr] items-start gap-2 text-sm text-text-secondary">
                <span className="font-mono text-text-muted">{index + 1}</span>
                <span>{step}</span>
              </li>
            ))}
          </ol>
        </div>
        <p className="mt-6 border-l-2 border-border-slate pl-3 text-xs text-text-muted">
          No controls are active here yet. InferDeck will expose them only when the matching in-process runtime and recovery path are ready.
        </p>
      </Panel>
    </div>
  );
};
