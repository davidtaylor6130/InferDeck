import React from 'react';
import type { PageId } from '../types';
import { StatusBadge } from './StatusBadge';

interface SidebarProps {
  activePage: PageId;
  onNavigate: (page: PageId) => void;
  collapsed: boolean;
  onToggleCollapse: () => void;
  mobileOpen: boolean;
  onCloseMobile: () => void;
  version: string;
  connected: boolean;
}

const navItems: Array<{ id: PageId; label: string }> = [
  { id: 'overview', label: 'Overview' },
  { id: 'queue', label: 'Queue' },
  { id: 'jobs', label: 'Jobs' },
  { id: 'models', label: 'Models' },
  { id: 'services', label: 'Services' },
  { id: 'hardware', label: 'Hardware' },
  { id: 'logs', label: 'Logs' },
  { id: 'settings', label: 'Settings' },
];

export const Sidebar: React.FC<SidebarProps> = ({ activePage, onNavigate, collapsed, onToggleCollapse, mobileOpen, onCloseMobile, version, connected }) => {
  const content = (
    <aside className={`flex h-full flex-col border-r border-border-slate bg-deck-navy px-4 py-6 transition-all ${collapsed ? 'w-20' : 'w-[260px]'}`}>
      <div className="flex min-w-0 items-center gap-3">
        <div className="relative h-12 w-12 shrink-0 rounded-xl border border-infer-violet bg-infer-violet/10">
          <div className="absolute left-3 top-3 h-5 w-5 border-4 border-infer-violet" />
          <div className="absolute bottom-1 left-7 h-2 w-0.5 bg-ion-cyan" />
          <div className="absolute right-1 top-4 h-0.5 w-3 bg-ion-cyan" />
        </div>
        {!collapsed && (
          <div className="min-w-0">
            <h1 className="truncate text-2xl font-bold leading-tight text-text-primary">InferDeck</h1>
            <p className="truncate text-sm text-text-secondary">Local AI Control Plane</p>
          </div>
        )}
      </div>
      {!collapsed && <p className="mt-5 truncate font-mono text-sm text-ion-cyan" title="ai.homelab.com">ai.homelab.com</p>}

      <nav className="mt-6 space-y-2">
        {navItems.map((item) => {
          const selected = item.id === activePage;
          return (
            <button
              key={item.id}
              onClick={() => {
                onNavigate(item.id);
                onCloseMobile();
              }}
              title={item.label}
              className={`flex w-full items-center gap-3 rounded-lg border px-3 py-3 text-left transition ${
                selected
                  ? 'border-infer-violet/50 bg-infer-violet/20 text-text-primary shadow-[inset_4px_0_0_#8B5CF6]'
                  : 'border-transparent text-text-secondary hover:border-border-slate hover:bg-panel-slate hover:text-text-primary'
              }`}
            >
              <span className="h-5 w-5 shrink-0 rounded border border-current" />
              {!collapsed && <span className="truncate text-base">{item.label}</span>}
            </button>
          );
        })}
      </nav>

      <div className="mt-auto space-y-5 pt-6">
        {!collapsed && (
          <>
            <div>
              <p className="mb-2 text-[11px] uppercase tracking-wide text-text-muted">Current Mode</p>
              <div className="rounded-lg border border-infer-violet/40 bg-infer-violet/10 p-4">
                <p className="text-2xl font-bold text-infer-violet">AI Mode</p>
                <p className="text-sm text-text-secondary">Background jobs allowed</p>
              </div>
            </div>
            <div>
              <p className="text-[11px] uppercase tracking-wide text-text-muted">Gateway Version</p>
              <p className="mt-2 font-mono text-sm text-text-primary">v{version}</p>
            </div>
            <div>
              <p className="mb-2 text-[11px] uppercase tracking-wide text-text-muted">Connection</p>
              <StatusBadge label={connected ? 'Online' : 'Offline'} tone={connected ? 'online' : 'offline'} dot />
            </div>
          </>
        )}
        <button
          onClick={onToggleCollapse}
          className="flex w-full items-center justify-center gap-3 rounded-lg border border-border-slate bg-panel-slate px-4 py-3 text-text-secondary transition hover:bg-card-highlight hover:text-text-primary"
        >
          <span>{collapsed ? '›' : '‹'}</span>
          {!collapsed && <span>Collapse</span>}
        </button>
      </div>
    </aside>
  );

  return (
    <>
      <div className="hidden lg:block">{content}</div>
      {mobileOpen && (
        <div className="fixed inset-0 z-50 lg:hidden">
          <button className="absolute inset-0 bg-black/60" onClick={onCloseMobile} aria-label="Close navigation" />
          <div className="relative h-full">{content}</div>
        </div>
      )}
    </>
  );
};
