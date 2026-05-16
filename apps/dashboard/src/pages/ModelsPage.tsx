import React, { useState } from 'react';
import type { PageProps } from '../types';
import { EmptyState } from '../components/EmptyState';
import { ModelTable } from '../components/ModelTable';
import { SectionCard } from '../components/SectionCard';
import { CommandButton } from '../components/CommandButton';

export const ModelsPage: React.FC<PageProps> = ({ state, actions }) => {
  const [modelUrl, setModelUrl] = useState('');

  return (
    <div className="space-y-5">
      <SectionCard title="Download GGUF Model">
        <div className="flex flex-col gap-3 xl:flex-row">
          <input className="min-h-11 flex-1 rounded-lg border border-border-slate bg-deck-navy px-3 font-mono text-sm text-text-primary outline-none focus:border-infer-violet" placeholder="https://huggingface.co/.../model.gguf" value={modelUrl} onChange={event => setModelUrl(event.target.value)} onKeyDown={event => event.key === 'Enter' && modelUrl.trim() && (() => { actions.pullModel(modelUrl.trim()); setModelUrl(''); })()} />
          <CommandButton tone="violet" onClick={() => { if (modelUrl.trim()) { actions.pullModel(modelUrl.trim()); setModelUrl(''); } }}>Download</CommandButton>
        </div>
        <p className="mt-2 text-xs text-text-secondary">Enter a direct URL to a .gguf file. Models are placed in the configured models directory.</p>
      </SectionCard>
      <SectionCard title="Installed Models" eyebrow="GGUF model inventory and GPU load controls">
        {state.modelsList.length ? (
          <ModelTable models={state.modelsList} loadedNames={state.runningModels.map(m => m.name)} onLoad={actions.loadModel} onUnload={actions.unloadModel} onDelete={(name) => window.confirm(`Delete model ${name}?`) && actions.deleteModel(name)} onCopied={actions.toast} />
        ) : (
          <EmptyState title="No GGUF models found." description="Place .gguf files in the models directory or use the Download section above." />
        )}
      </SectionCard>
    </div>
  );
};
