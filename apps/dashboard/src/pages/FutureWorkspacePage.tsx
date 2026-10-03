import React from 'react';
import { Badge, GroupHeader, GroupList, PageHeader } from '../components/ui';

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
    <div className="task-view space-y-5">
      <PageHeader
        title={content.title}
        subtitle={content.purpose}
        actions={<Badge label="Planned" tone="idle" />}
      />
      <section>
        <GroupHeader title="Planned workflow" />
        <GroupList>
          {content.steps.map((step, index) => (
            <div key={step} className="flex items-baseline gap-3 px-3 py-2.5 text-sm">
              <span className="tabular w-4 shrink-0 text-text-muted">{index + 1}</span>
              <span className="text-text-secondary">{step}</span>
            </div>
          ))}
        </GroupList>
        <p className="mt-2 text-xs text-text-muted">
          No controls are active here yet. InferDeck will expose them only when the matching in-process runtime and recovery path are ready.
        </p>
      </section>
    </div>
  );
};
