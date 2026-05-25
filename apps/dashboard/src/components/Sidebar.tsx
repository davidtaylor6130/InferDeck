import React from 'react';
import {
  Cog6ToothIcon,
  CpuChipIcon,
  CubeIcon,
  DocumentTextIcon,
  QueueListIcon,
  ServerStackIcon,
  Squares2X2Icon,
  WrenchScrewdriverIcon,
} from '@heroicons/react/24/outline';
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

const navItems: Array<{ id: PageId; label: string; Icon: React.ComponentType<React.SVGProps<SVGSVGElement>> }> = [
  { id: 'overview', label: 'Overview', Icon: Squares2X2Icon },
  { id: 'workloads', label: 'Workloads', Icon: QueueListIcon },
  { id: 'models', label: 'Models', Icon: CubeIcon },
  { id: 'services', label: 'Services', Icon: ServerStackIcon },
  { id: 'hardware', label: 'Hardware', Icon: CpuChipIcon },
  { id: 'logs', label: 'Logs', Icon: DocumentTextIcon },
  { id: 'settings', label: 'Settings', Icon: Cog6ToothIcon },
];

export const Sidebar: React.FC<SidebarProps> = ({ activePage, onNavigate, collapsed, onToggleCollapse, mobileOpen, onCloseMobile, version, connected }) => {
  const content = (
    <aside className={`flex h-full flex-col border-r border-white/10 bg-[#07101d]/95 px-4 py-6 transition-all ${collapsed ? 'w-20' : 'w-[250px]'}`}>
      <div className="flex min-w-0 items-center gap-3">
        <div className="relative grid h-11 w-11 shrink-0 place-items-center rounded-xl border border-infer-violet/70 bg-infer-violet/10 shadow-[0_0_28px_rgba(139,92,246,0.18)]">
          <div className="h-5 w-5 border-4 border-infer-violet" />
          <div className="absolute right-2 top-4 h-0.5 w-3 bg-ion-cyan" />
          <div className="absolute bottom-2 left-7 h-2 w-0.5 bg-ion-cyan" />
        </div>
        {!collapsed && (
          <div className="min-w-0">
            <h1 className="truncate text-xl font-semibold leading-tight text-text-primary">InferDeck</h1>
            <p className="truncate text-xs text-text-secondary">Local AI Control Plane</p>
          </div>
        )}
      </div>

      <nav className="mt-8 space-y-1.5">
        {navItems.map(({ id, label, Icon }) => {
          const selected = id === activePage;
          return (
            <button
              key={id}
              onClick={() => {
                onNavigate(id);
                onCloseMobile();
              }}
              title={label}
              className={`flex w-full items-center gap-3 rounded-lg border px-3 py-3 text-left transition ${
                selected
                  ? 'border-infer-violet/40 bg-infer-violet/15 text-text-primary shadow-[inset_3px_0_0_#8B5CF6]'
                  : 'border-transparent text-text-secondary hover:border-white/10 hover:bg-white/[0.04] hover:text-text-primary'
              }`}
            >
              <Icon className="h-5 w-5 shrink-0" />
              {!collapsed && <span className="truncate text-sm font-medium">{label}</span>}
            </button>
          );
        })}
      </nav>

      <div className="mt-auto space-y-4 pt-6">
        {!collapsed && (
          <>
            <div className="rounded-lg border border-white/10 bg-white/[0.035] p-4">
              <div className="flex items-center justify-between gap-3">
                <p className="text-[11px] uppercase text-text-muted">Current Mode</p>
                <span className={`h-2 w-2 rounded-full ${connected ? 'bg-success-green' : 'bg-danger-rose'}`} />
              </div>
              <p className="mt-3 text-2xl font-semibold text-infer-violet">AI Mode</p>
              <p className="mt-1 text-xs text-text-secondary">Background jobs allowed</p>
            </div>
            <div className="flex items-center justify-between gap-3 text-xs text-text-muted">
              <span>v{version}</span>
              <StatusBadge label={connected ? 'Online' : 'Offline'} tone={connected ? 'online' : 'offline'} dot />
            </div>
          </>
        )}
        <button
          onClick={onToggleCollapse}
          className="flex w-full items-center justify-center gap-2 rounded-lg border border-white/10 bg-white/[0.035] px-3 py-2.5 text-sm text-text-secondary transition hover:bg-white/[0.06] hover:text-text-primary"
          title={collapsed ? 'Expand sidebar' : 'Collapse sidebar'}
        >
          <WrenchScrewdriverIcon className="h-4 w-4" />
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
