import React, { useState } from 'react';
import type { PageProps } from '../types';
import { EmptyState } from '../components/EmptyState';
import { ModelTable } from '../components/ModelTable';
import { SectionCard } from '../components/SectionCard';
import { CommandButton } from '../components/CommandButton';

const quickModels = ['qwen3.6:35b-a3b-q4_K_M', 'qwen3.6:27b-q4_K_M', 'gpt-oss:20b', 'qwen3-coder:30b', 'mistral-small3.2:24b'];

export const ModelsPage: React.FC<PageProps> = ({ state, actions }) => {
  const [modelName, setModelName] = useState('');
  const pull = (name: string) => {
    if (!name.trim()) return;
    actions.pullModel(name.trim());
    setModelName('');
  };

  return (
    <div className="space-y-5">
      <SectionCard title="Pull Model">
        <div className="flex flex-col gap-3 xl:flex-row">
          <input className="min-h-11 flex-1 rounded-lg border border-border-slate bg-deck-navy px-3 font-mono text-sm text-text-primary outline-none focus:border-infer-violet" placeholder="model:tag" value={modelName} onChange={event => setModelName(event.target.value)} onKeyDown={event => event.key === 'Enter' && pull(modelName)} />
          <CommandButton tone="violet" onClick={() => pull(modelName)}>Pull</CommandButton>
        </div>
        <div className="mt-4 flex flex-wrap gap-2">
          {quickModels.map(model => <CommandButton key={model} tone="blue" className="font-mono text-xs" onClick={() => pull(model)}>{model}</CommandButton>)}
        </div>
      </SectionCard>
      <SectionCard title="Installed Models" eyebrow="Ollama inventory and GPU load controls">
        {state.modelsList.length ? (
          <ModelTable models={state.modelsList} loadedNames={state.runningModels.map(m => m.name)} onLoad={actions.loadModel} onUnload={actions.unloadModel} onDelete={(name) => window.confirm(`Delete model ${name}?`) && actions.deleteModel(name)} onCopied={actions.toast} />
        ) : (
          <EmptyState title="No installed models reported." description="Pull a model or check the Ollama backend connection at 127.0.0.1:11435." />
        )}
      </SectionCard>
    </div>
  );
};
