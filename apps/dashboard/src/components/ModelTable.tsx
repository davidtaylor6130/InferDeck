import React from 'react';
import type { ModelRecord } from '../types';
import { formatBytes } from '../utils';
import { CommandButton } from './CommandButton';
import { CopyButton } from './CopyButton';
import { StatusBadge } from './StatusBadge';

interface ModelTableProps {
  models: ModelRecord[];
  loadedNames?: string[];
  onLoad: (name: string) => void;
  onUnload: (name: string) => void;
  onDelete?: (name: string) => void;
  onCopied?: (message: string) => void;
}

export const ModelTable: React.FC<ModelTableProps> = ({ models, loadedNames = [], onLoad, onUnload, onDelete, onCopied }) => (
  <div className="overflow-x-auto">
    <table className="min-w-[860px] w-full table-fixed text-left text-sm">
      <thead>
        <tr className="border-b border-border-slate bg-card-highlight/60 text-xs text-text-secondary">
          <th className="w-72 px-3 py-3 font-medium">Model</th>
          <th className="w-24 px-3 py-3 font-medium">Size</th>
          <th className="w-32 px-3 py-3 font-medium">Context</th>
          <th className="w-24 px-3 py-3 font-medium">Loaded</th>
          <th className="w-32 px-3 py-3 font-medium">Processor</th>
          <th className="w-56 px-3 py-3 font-medium">Actions</th>
        </tr>
      </thead>
      <tbody className="divide-y divide-border-slate/70">
        {models.map((model, index) => {
          const loaded = Boolean(model.loaded) || loadedNames.includes(model.name);
          return (
            <tr key={model.name} className="hover:bg-card-highlight/40">
              <td className="px-3 py-3">
                <div className="flex min-w-0 items-center gap-2">
                  <span className="truncate font-mono text-xs text-text-primary" title={model.path || model.name}>{model.name}</span>
                  <CopyButton value={model.name} label="Model name" onCopied={onCopied} />
                </div>
              </td>
              <td className="px-3 py-3">{formatBytes(model.size)}</td>
              <td className="px-3 py-3">{model.details?.parameter_size || 'N/A'}</td>
              <td className="px-3 py-3"><StatusBadge label={loaded ? 'Yes' : 'No'} tone={loaded ? 'online' : 'stopped'} /></td>
              <td className="truncate px-3 py-3">Radeon AI PRO R9700</td>
              <td className="px-3 py-3">
                <div className="flex flex-wrap gap-2">
                  <CommandButton tone="green" className="min-h-8 px-3 py-1 text-xs" onClick={() => onLoad(model.name)}>Load</CommandButton>
                  <CommandButton tone="neutral" className="min-h-8 px-3 py-1 text-xs" onClick={() => onUnload(model.name)}>Unload</CommandButton>
                  {onDelete && <CommandButton tone="rose" className="min-h-8 px-3 py-1 text-xs" onClick={() => onDelete(model.name)}>Delete</CommandButton>}
                </div>
              </td>
            </tr>
          );
        })}
      </tbody>
    </table>
  </div>
);
