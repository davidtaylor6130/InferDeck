import React, { useState } from 'react';
import type { PageId } from '../types';
import { Sidebar } from './Sidebar';
import { TopBar } from './TopBar';

interface AppShellProps {
  activePage: PageId;
  onNavigate: (page: PageId) => void;
  children: React.ReactNode;
  version: string;
  connected: boolean;
  lastUpdatedAt: Date | null;
  onMode: (mode: string) => void;
  onPauseQueue: () => void;
  onRestartOllama: () => void;
}

export const AppShell: React.FC<AppShellProps> = ({ activePage, onNavigate, children, version, connected, lastUpdatedAt, onMode, onPauseQueue, onRestartOllama }) => {
  const [collapsed, setCollapsed] = useState(false);
  const [mobileOpen, setMobileOpen] = useState(false);

  return (
    <div className="deck-grid-bg min-h-screen">
      <div className="flex min-h-screen">
        <Sidebar
          activePage={activePage}
          onNavigate={onNavigate}
          collapsed={collapsed}
          onToggleCollapse={() => setCollapsed(value => !value)}
          mobileOpen={mobileOpen}
          onCloseMobile={() => setMobileOpen(false)}
          version={version}
          connected={connected}
        />
        <div className="flex min-w-0 flex-1 flex-col">
          <TopBar
            onMenu={() => setMobileOpen(true)}
            onMode={onMode}
            onPauseQueue={onPauseQueue}
            onRestartOllama={onRestartOllama}
            lastUpdatedAt={lastUpdatedAt}
            connected={connected}
          />
          <main className="min-w-0 flex-1 px-4 py-5 xl:px-8">{children}</main>
        </div>
      </div>
    </div>
  );
};
