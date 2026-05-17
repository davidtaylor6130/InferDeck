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

const navIcons: Record<PageId, string> = {
  overview: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="h-5 w-5"><rect x="3" y="3" width="7" height="7"/><rect x="14" y="3" width="7" height="7"/><rect x="14" y="14" width="7" height="7"/><rect x="3" y="14" width="7" height="7"/></svg>',
  queue: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="h-5 w-5"><line x1="8" y1="6" x2="21" y2="6"/><line x1="8" y1="12" x2="21" y2="12"/><line x1="8" y1="18" x2="21" y2="18"/><line x1="3" y1="6" x2="3.01" y2="6"/><line x1="3" y1="12" x2="3.01" y2="12"/><line x1="3" y1="18" x2="3.01" y2="18"/></svg>',
  jobs: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="h-5 w-5"><path d="M16 4h2a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2h2"/><rect x="8" y="2" width="8" height="4" rx="1" ry="1"/></svg>',
  models: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="h-5 w-5"><circle cx="12" cy="12" r="10"/><path d="M12 2a14.5 14.5 0 0 0 0 20 14.5 14.5 0 0 0 0-20"/><path d="M2 12h20"/></svg>',
  services: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="h-5 w-5"><rect x="2" y="2" width="20" height="8" rx="2" ry="2"/><rect x="2" y="14" width="20" height="8" rx="2" ry="2"/><line x1="6" y1="6" x2="6.01" y2="6"/><line x1="6" y1="18" x2="6.01" y2="18"/></svg>',
  hardware: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="h-5 w-5"><rect x="4" y="4" width="16" height="16" rx="2" ry="2"/><rect x="9" y="9" width="6" height="6"/><line x1="9" y1="1" x2="9" y2="4"/><line x1="15" y1="1" x2="15" y2="4"/><line x1="9" y1="20" x2="9" y2="23"/><line x1="15" y1="20" x2="15" y2="23"/><line x1="20" y1="9" x2="23" y2="9"/><line x1="20" y1="14" x2="23" y2="14"/><line x1="1" y1="9" x2="4" y2="9"/><line x1="1" y1="14" x2="4" y2="14"/></svg>',
  logs: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="h-5 w-5"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/><line x1="16" y1="13" x2="8" y2="13"/><line x1="16" y1="17" x2="8" y2="17"/></svg>',
  settings: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="h-5 w-5"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1 0 2.83 2 2 0 0 1-2.83 0l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-2 2 2 2 0 0 1-2-2v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83 0 2 2 0 0 1 0-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1-2-2 2 2 0 0 1 2-2h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 0-2.83 2 2 0 0 1 2.83 0l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 2-2 2 2 0 0 1 2 2v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 0 2 2 0 0 1 0 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 2 2 2 2 0 0 1-2 2h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg>',
};

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
              <span className="h-5 w-5 shrink-0" dangerouslySetInnerHTML={{ __html: navIcons[item.id] }} />
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
