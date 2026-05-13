import React from 'react';
import { CommandButton } from './CommandButton';
import { StatusBadge } from './StatusBadge';

interface ModeSwitcherProps {
  currentMode: string;
  onChange: (mode: string) => void;
}

const modes = [
  { id: 'ai', label: 'AI Mode', detail: 'Background jobs allowed', tone: 'violet' as const },
  { id: 'gaming', label: 'Gaming', detail: 'Queue pauses for GPU priority', tone: 'orange' as const },
  { id: 'maintenance', label: 'Maintenance', detail: 'Controlled service changes', tone: 'amber' as const },
];

export const ModeSwitcher: React.FC<ModeSwitcherProps> = ({ currentMode, onChange }) => (
  <div className="grid gap-3 md:grid-cols-3">
    {modes.map(mode => (
      <div key={mode.id} className={`rounded-xl border p-4 ${currentMode === mode.id ? 'border-infer-violet bg-infer-violet/10' : 'border-border-slate bg-panel-slate'}`}>
        <div className="flex items-center justify-between gap-3">
          <div className="min-w-0">
            <h3 className="truncate text-lg font-semibold text-text-primary">{mode.label}</h3>
            <p className="truncate text-sm text-text-secondary">{mode.detail}</p>
          </div>
          {currentMode === mode.id && <StatusBadge label="Active" tone="queued" />}
        </div>
        <CommandButton tone={mode.tone} className="mt-4 w-full" onClick={() => onChange(mode.id)} disabled={currentMode === mode.id}>
          Switch
        </CommandButton>
      </div>
    ))}
  </div>
);
