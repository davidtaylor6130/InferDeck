import React from 'react';
import type { PageProps } from '../types';
import { EmptyState } from '../components/EmptyState';
import { ModelTable } from '../components/ModelTable';
import { SectionCard } from '../components/SectionCard';
import { CommandButton } from '../components/CommandButton';

export const ModelsPage: React.FC<PageProps> = ({ state, actions }) => {
  return (
    <div className="space-y-5">
      <SectionCard
        title="Installed Models"
        eyebrow="GGUF model inventory and GPU load controls"
        action={<CommandButton tone="blue" onClick={actions.rescanModels}>Rescan</CommandButton>}
      >
        {state.modelsList.length ? (
          <ModelTable models={state.modelsList} loadedNames={state.runningModels.map(m => m.name)} onLoad={actions.loadModel} onUnload={actions.unloadModel} onCopied={actions.toast} />
        ) : (
          <EmptyState title="No GGUF models found." description="Place .gguf files in the configured models directory, then rescan the inventory." />
        )}
      </SectionCard>
    </div>
  );
};
