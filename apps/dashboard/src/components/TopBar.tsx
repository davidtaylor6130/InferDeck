import React from 'react';
import { Bars3Icon } from '@heroicons/react/24/outline';

interface TopBarProps {
  onMenu: () => void;
  onMode: (mode: string) => void;
  onPauseQueue: () => void;
  onRestartBackend: () => void;
  lastUpdatedAt: Date | null;
  connected: boolean;
}

export const TopBar: React.FC<TopBarProps> = ({ onMenu, connected }) => (
  <header className="border-b border-white/10 bg-[#050b14]/80 px-4 py-3 backdrop-blur xl:px-7 lg:hidden">
    <div className="flex items-center justify-between gap-3">
      <button className="rounded-lg border border-white/10 bg-white/[0.04] p-2 text-text-primary" onClick={onMenu} aria-label="Open navigation">
        <Bars3Icon className="h-5 w-5" />
      </button>
      <div className="flex items-center gap-2 text-xs text-text-secondary">
        <span>{connected ? 'Connected' : 'Offline'}</span>
        <span className={`h-2.5 w-2.5 rounded-full ${connected ? 'bg-success-green' : 'bg-danger-rose'}`} />
      </div>
    </div>
  </header>
);
