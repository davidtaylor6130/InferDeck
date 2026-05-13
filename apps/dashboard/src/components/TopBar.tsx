import React from 'react';
import { timeAgo } from '../utils';
import { CommandButton } from './CommandButton';

interface TopBarProps {
  onMenu: () => void;
  onMode: (mode: string) => void;
  onPauseQueue: () => void;
  onRestartOllama: () => void;
  lastUpdatedAt: Date | null;
  connected: boolean;
}

export const TopBar: React.FC<TopBarProps> = ({ onMenu, onMode, onPauseQueue, onRestartOllama, lastUpdatedAt, connected }) => (
  <header className="border-b border-border-slate bg-void-black/70 px-4 py-5 backdrop-blur xl:px-8">
    <div className="flex min-w-0 flex-col gap-4 2xl:flex-row 2xl:items-center 2xl:justify-between">
      <div className="flex min-w-0 items-start gap-3">
        <button className="mt-1 rounded-md border border-border-slate bg-panel-slate px-3 py-2 text-sm text-text-primary lg:hidden" onClick={onMenu}>
          Menu
        </button>
        <div className="min-w-0">
          <h2 className="truncate text-3xl font-bold leading-tight text-text-primary md:text-4xl">Operations Deck</h2>
          <p className="truncate text-base text-text-secondary">One GPU. Every AI workload.</p>
        </div>
      </div>
      <div className="flex min-w-0 flex-wrap items-center gap-3">
        <CommandButton tone="violet" onClick={() => onMode('ai')}>AI Mode</CommandButton>
        <CommandButton tone="orange" onClick={() => onMode('gaming')}>Gaming</CommandButton>
        <CommandButton tone="amber" onClick={() => onMode('maintenance')}>Maint.</CommandButton>
        <CommandButton tone="blue" onClick={onPauseQueue}>Pause Queue</CommandButton>
        <CommandButton tone="neutral" onClick={onRestartOllama}>Restart Ollama</CommandButton>
        <div className="flex min-w-0 items-center gap-3 pl-1 text-sm text-text-secondary">
          <span className="truncate">Updated {timeAgo(lastUpdatedAt)}</span>
          <span className={`h-3 w-3 shrink-0 rounded-full ${connected ? 'bg-success-green' : 'bg-danger-rose'}`} title={connected ? 'Online' : 'Offline'} />
        </div>
      </div>
    </div>
  </header>
);
