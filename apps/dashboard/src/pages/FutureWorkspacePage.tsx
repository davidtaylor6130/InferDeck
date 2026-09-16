import React from 'react';
import { Badge, Panel, SectionTitle } from '../components/ui';

export type FutureArea = 'post-training';

const AREAS: Record<FutureArea, { title: string; purpose: string; steps: string[] }> = {
  'post-training': {
    title: 'Post Training',
    purpose: 'Native GGUF quantisation is available through the control API. Dashboard controls and fine-tuning remain planned.',
    steps: ['Choose an unloaded managed GGUF source', 'Start a supported quantisation job through the API', 'Monitor the job and use the registered output model'],
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
