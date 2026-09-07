import React, { useEffect, useState } from 'react';
import { ChevronRightIcon, XMarkIcon } from '@heroicons/react/24/outline';
import { getHealth, getPricing } from './api';
import { DashboardAccess, useDashboardAccess } from './components/DashboardAccess';
import { RequestsPage } from './pages/RequestsPage';
import { Badge } from './components/ui';
import { COST_STORAGE_KEY } from './cost';
import { DASHBOARD_SECTIONS, sectionLabel, type DashboardSection } from './dashboardSections';
import { GatewayProvider, useGateway } from './gateway';
import { OverviewPage } from './pages/OverviewPage';
import { ModelsPage } from './pages/ModelsPage';
import { OperatePage } from './pages/OperatePage';
import { UsagePage } from './pages/UsagePage';
import { SystemPage } from './pages/SystemPage';
import { FutureWorkspacePage } from './pages/FutureWorkspacePage';
import { ImagePage } from './pages/ImagePage';
import { MusicPage } from './pages/MusicPage';
import { ApiSettingsPage } from './pages/ApiSettingsPage';
import {
  loadCollapsedSidebarSections,
  SIDEBAR_SECTION_STORAGE_KEY,
  toggleCollapsedSidebarSection,
} from './sidebarPreferences';
import { compactModel, timeAgo } from './utils';
import { INFERDECK_VERSION } from './version';
import logoUrl from '../../../Assets/Logo.png';

export type PageId =
  | 'home'
  | 'requests'
  | 'settings'
  | 'llm/settings'
  | 'llm/models'
  | 'llm/usage'
  | 'llm/diagnostics'
  | 'dictation/settings'
  | 'dictation/models'
  | 'dictation/usage'
  | 'dictation/diagnostics'
  | 'image/generate'
  | 'image/settings'
  | 'image/models'
  | 'image/usage'
  | 'image/diagnostics'
  | 'music/generate'
  | 'music/settings'
  | 'music/models'
  | 'music/usage'
  | 'music/diagnostics'
  | 'post-training';

interface DashboardPage {
  id: PageId;
  label: string;
  section?: DashboardSection;
  preview?: boolean;
}

export const DASHBOARD_PAGES: ReadonlyArray<DashboardPage> = [
  { id: 'home', label: 'Home' },
  { id: 'requests', label: 'Requests' },
  { id: 'settings', label: 'API Settings' },
  { id: 'llm/settings', label: 'Model Settings', section: 'llm' },
  { id: 'llm/models', label: 'Model Store', section: 'llm' },
  { id: 'llm/usage', label: 'Usage', section: 'llm' },
  { id: 'llm/diagnostics', label: 'Health & alerts', section: 'llm' },
  { id: 'dictation/settings', label: 'Model Settings', section: 'dictation' },
  { id: 'dictation/models', label: 'Model Store', section: 'dictation' },
  { id: 'dictation/usage', label: 'Usage', section: 'dictation' },
  { id: 'dictation/diagnostics', label: 'Health & alerts', section: 'dictation' },
  { id: 'image/generate', label: 'Generate', section: 'image' },
  { id: 'image/settings', label: 'Model Settings', section: 'image' },
  { id: 'image/models', label: 'Model Store', section: 'image' },
  { id: 'image/usage', label: 'Usage', section: 'image' },
  { id: 'image/diagnostics', label: 'Health & alerts', section: 'image' },
  { id: 'music/generate', label: 'Generate', section: 'music' },
  { id: 'music/settings', label: 'Model Settings', section: 'music' },
  { id: 'music/models', label: 'Model Store', section: 'music' },
  { id: 'music/usage', label: 'Usage', section: 'music' },
  { id: 'music/diagnostics', label: 'Health & alerts', section: 'music' },
  { id: 'post-training', label: 'Post Training', preview: true },
];

const SECTION_SETTINGS_HELP: Record<DashboardSection, string> = {
  llm: 'Profiles, aliases, pricing, and model loading',
  dictation: 'Speech runtimes, costs, and model configuration',
  image: 'Image runtimes, model loading, and active profiles',
  music: 'Music runtimes, model loading, and active profiles',
};

const LEGACY_ROUTES: Record<string, PageId> = {
  overview: 'home',
  models: 'llm/models',
  usage: 'llm/usage',
  system: 'llm/diagnostics',
  'llm/operate': 'llm/settings',
  'dictation/operate': 'dictation/settings',
  image: 'image/generate',
  music: 'music/generate',
  'image/operate': 'image/settings',
  'music/operate': 'music/settings',
};

function pageFromHash(): PageId {
  const hash = window.location.hash.replace(/^#\/?/, '');
  const legacy = LEGACY_ROUTES[hash];
  if (legacy) return legacy;
  return (DASHBOARD_PAGES.some(page => page.id === hash) ? hash : 'home') as PageId;
}

const App: React.FC = () => (
  <DashboardAccess><GatewayProvider>
    <Shell />
  </GatewayProvider></DashboardAccess>
);

const Shell: React.FC = () => {
  const [page, setPage] = useState<PageId>(() => (typeof window === 'undefined' ? 'home' : pageFromHash()));
  const [collapsedSections, setCollapsedSections] =
    useState<DashboardSection[]>(() => {
      if (typeof window === 'undefined') return [];
      try {
        return loadCollapsedSidebarSections(
          window.localStorage.getItem(SIDEBAR_SECTION_STORAGE_KEY),
        );
      } catch {
        return [];
      }
    });

  useEffect(() => {
    const onHashChange = () => setPage(pageFromHash());
    window.addEventListener('hashchange', onHashChange);
    return () => window.removeEventListener('hashchange', onHashChange);
  }, []);

  const toggleSection = (section: DashboardSection) => {
    setCollapsedSections(current => {
      const next = toggleCollapsedSidebarSection(current, section);
      try {
        window.localStorage.setItem(
          SIDEBAR_SECTION_STORAGE_KEY,
          JSON.stringify(next),
        );
      } catch {
      }
      return next;
    });
  };

  return (
    <div className="app-shell flex h-dvh overflow-hidden">
      <aside className="hidden w-56 shrink-0 flex-col border-r border-border-slate bg-deck-navy px-3 py-5 md:flex">
        <div className="mb-5 flex items-center gap-3 px-2">
          <img src={logoUrl} alt="" className="h-9 w-9 rounded-md object-cover" />
          <div>
            <span className="text-base font-semibold text-text-primary">InferDeck</span>
            <span className="mt-0.5 block text-xs text-text-muted">Local inference</span>
          </div>
        </div>
        <nav className="flex min-h-0 flex-1 flex-col overflow-y-auto" aria-label="Dashboard">
          <NavLink id="home" label="Home" page={page} />
          <NavLink id="requests" label="Requests" page={page} />
          <NavLink id="settings" label="API Settings" page={page} />
          {DASHBOARD_SECTIONS.map(section => {
            const collapsed = collapsedSections.includes(section);
            const label = sectionLabel(section);
            const controls = 'sidebar-' + section + '-navigation';
            return (
              <div key={section} className="mt-4">
                <button
                  type="button"
                  aria-expanded={!collapsed}
                  aria-controls={controls}
                  aria-label={(collapsed ? 'Show ' : 'Hide ') + label + ' navigation'}
                  onClick={() => toggleSection(section)}
                  className="mb-1 flex min-h-9 w-full items-center justify-between rounded px-3 text-[11px] font-semibold uppercase tracking-[0.14em] text-text-muted hover:bg-white/[0.04] hover:text-text-primary focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-queue-blue"
                >
                  <span className="flex items-center gap-2"><span className={`h-2.5 w-1 rounded ${section === 'llm' ? 'bg-queue-blue' : section === 'dictation' ? 'bg-warning-amber' : section === 'image' ? 'bg-infer-violet' : 'bg-gaming-orange'}`} /><span className="text-text-secondary">{label}</span></span>
                  <span className="ml-auto mr-2 text-[10px] font-normal">{DASHBOARD_PAGES.filter(item => item.section === section).length}</span>
                  <ChevronRightIcon
                    aria-hidden="true"
                    className={'h-3.5 w-3.5 transition-transform ' + (collapsed ? '' : 'rotate-90')}
                  />
                </button>
                <div
                  id={controls}
                  hidden={collapsed}
                  className={sidebarNavigationClass(collapsed)}
                >
                  {DASHBOARD_PAGES.filter(item => item.section === section).map(({ id, label: itemLabel }) => (
                    <NavLink key={id} id={id} label={itemLabel} page={page} nested />
                  ))}
                </div>
              </div>
            );
          })}
          <div className="mt-5 border-t border-white/10 pt-4">
            <div className="mb-1 px-3 text-[11px] font-semibold uppercase tracking-[0.14em] text-text-muted">
              Planned
            </div>
            <div className="flex flex-col gap-1">
              {DASHBOARD_PAGES.filter(item => item.preview).map(({ id, label }) => (
                <NavLink key={id} id={id} label={label} page={page} nested />
              ))}
            </div>
          </div>
        </nav>
        <SidebarAccount />
      </aside>

      <div className="flex min-h-0 min-w-0 flex-1 flex-col overflow-hidden">
        <TopBar page={page} />
        <ConnectionBanner />
        <HealthNotices />
        <main className="min-h-0 min-w-0 flex-1 overflow-y-auto px-4 py-5 sm:px-6">
          <div className="mx-auto max-w-[1280px]">
            {page === 'home' && <OverviewPage />}
            {page === 'requests' && <RequestsPage />}
            {page === 'settings' && <ApiSettingsPage />}
            {page === 'llm/settings' && <OperatePage section="llm" />}
            {page === 'llm/models' && <ModelsPage section="llm" />}
            {page === 'llm/usage' && <UsagePage section="llm" />}
            {page === 'llm/diagnostics' && <SystemPage section="llm" />}
            {page === 'dictation/settings' && <OperatePage section="dictation" />}
            {page === 'dictation/models' && <ModelsPage section="dictation" />}
            {page === 'dictation/usage' && <UsagePage section="dictation" />}
            {page === 'dictation/diagnostics' && <SystemPage section="dictation" />}
            {page === 'image/generate' && <ImagePage />}
            {page === 'image/settings' && <OperatePage section="image" />}
            {page === 'image/models' && <ModelsPage section="image" />}
            {page === 'image/usage' && <UsagePage section="image" />}
            {page === 'image/diagnostics' && <SystemPage section="image" />}
            {page === 'music/generate' && <MusicPage />}
            {page === 'music/settings' && <OperatePage section="music" />}
            {page === 'music/models' && <ModelsPage section="music" />}
            {page === 'music/usage' && <UsagePage section="music" />}
            {page === 'music/diagnostics' && <SystemPage section="music" />}
            {page === 'post-training' && <FutureWorkspacePage area="post-training" />}
          </div>
        </main>
      </div>
    </div>
  );
};

export function sidebarNavigationClass(collapsed: boolean): string {
  return collapsed ? 'hidden' : 'flex flex-col gap-1';
}

const SidebarAccount: React.FC = () => {
  const access = useDashboardAccess();
  const [error, setError] = useState('');
  return <div className="shrink-0 pt-5">
    <details className="relative">
      <summary className="flex cursor-pointer list-none items-center gap-2.5 rounded-lg border border-white/10 bg-white/[0.04] px-2.5 py-2 hover:bg-white/[0.08]">
        <span className="flex h-8 w-8 shrink-0 items-center justify-center rounded-full border border-queue-blue/40 bg-queue-blue/15 text-queue-blue" aria-hidden="true"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8"><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/></svg></span>
        <span className="min-w-0 flex-1"><span className="block text-sm font-medium">{access?.remote ? 'Dashboard session' : 'Local access'}</span><span className="block truncate text-[11px] text-text-muted">{access?.remembered ? 'Remembered browser' : 'Dashboard administration'}</span></span>
        <ChevronRightIcon className="h-3.5 w-3.5 text-text-muted" />
      </summary>
      <nav aria-label="Account" className="absolute bottom-full left-0 z-40 mb-1 w-full border border-border-slate bg-[#07101d] p-1 text-sm shadow-2xl">
        <a href="#settings" className="block rounded px-3 py-2 hover:bg-white/[0.05]">API keys</a>
        {access?.remote && <button className="w-full px-3 py-2 text-left hover:bg-white/[0.05]" onClick={() => { void access.logout().catch(() => setError('Log out failed. Try again.')); }}>Log out</button>}
      </nav>
    </details>
    {error && <p role="alert" className="mt-2 text-xs text-danger-rose">{error}</p>}
    <p className="mt-2 px-1 text-[10px] text-text-muted">InferDeck v{INFERDECK_VERSION} / In-process runtime</p>
  </div>;
};

const TopBar: React.FC<{ page: PageId }> = ({ page }) => {
  const { connection, stats, swap } = useGateway();
  const access = useDashboardAccess();
  const [logoutError, setLogoutError] = useState('');
  const [loggingOut, setLoggingOut] = useState(false);
  const loaded = stats?.loadedModel || '';
  const pageInfo = DASHBOARD_PAGES.find(item => item.id === page);
  const connectionTone = connection === 'connected' ? 'good' : connection === 'offline' ? 'critical' : 'warn';
  const connectionLabel = connection === 'connected' ? 'Live' : connection === 'connecting' ? 'Connecting' : connection === 'reconnecting' ? 'Reconnecting' : 'Offline';
  const pageLabel = pageInfo?.section
    ? `${sectionLabel(pageInfo.section)} / ${pageInfo.label}`
    : pageInfo?.label;
  const healthTarget = pageInfo?.section ? `${pageInfo.section}/diagnostics` : 'llm/diagnostics';

  return (
    <header className="sticky top-0 z-20 border-b border-border-slate bg-deck-navy px-4 py-3 md:static sm:px-6">
      <div className="flex min-w-0 items-center justify-between gap-3">
        <div className="min-w-0">
          <span className="block text-[10px] font-medium uppercase tracking-[0.14em] text-text-muted md:hidden">InferDeck</span>
          <h1 className="truncate text-base font-semibold text-text-primary">{pageLabel}</h1>
        </div>
        <div className="flex shrink-0 items-center gap-2">
          {swap.swapping ? (
            <span className="hidden sm:inline"><Badge label={`Switching to ${compactModel(swap.target)}`} tone="info" /></span>
          ) : loaded ? (
            <span className="hidden text-xs text-text-secondary sm:inline">{compactModel(loaded)}</span>
          ) : (
            <span className="hidden text-xs text-text-muted sm:inline">No model loaded</span>
          )}
          <a
            href={`#${healthTarget}`}
            aria-label={`${connectionLabel}. Open Health and alerts`}
            title="Open Health & alerts"
            className="inline-flex min-h-11 min-w-11 items-center justify-center rounded focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-queue-blue md:min-h-0 md:min-w-0"
          >
            <Badge label={connectionLabel} tone={connectionTone} />
          </a>
          {access?.remote && <>
            <span className="hidden text-xs sm:inline">{access.remembered === true ? 'This browser is remembered' : access.remembered === false ? 'Signed in for this session' : 'Signed in'}</span>
            <button className="min-h-11 border border-white/25 px-3 text-sm" disabled={loggingOut} onClick={() => {
              setLoggingOut(true); setLogoutError('');
              void access.logout().catch(reason => { setLogoutError(reason instanceof Error ? reason.message : 'Log out failed. Try again.'); setLoggingOut(false); });
            }}>{loggingOut ? 'Logging out...' : 'Log out'}</button>
          </>}
          <details className="relative z-30">
            <summary className="flex min-h-11 cursor-pointer items-center rounded border border-white/15 bg-white/[0.06] px-3 py-2 text-xs font-medium text-text-primary transition-colors hover:bg-white/[0.12] sm:min-h-10">
              Settings
            </summary>
            <nav className="absolute right-0 z-40 mt-2 w-[min(18rem,calc(100vw-2rem))] border border-border-slate bg-[#07101d] shadow-deck" aria-label="Settings">
              <a
                href="#settings"
                onClick={event => event.currentTarget.closest('details')?.removeAttribute('open')}
                className="block border-b border-white/10 px-4 py-3 hover:bg-white/[0.05] focus-visible:outline focus-visible:outline-2 focus-visible:outline-inset focus-visible:outline-queue-blue"
              >
                <span className="block text-sm font-medium text-text-primary">API settings</span>
                <span className="mt-0.5 block text-xs text-text-muted">Public access and managed client priorities</span>
              </a>
              {DASHBOARD_SECTIONS.map(section => (
                <a
                  key={section}
                  href={`#${section}/settings`}
                  onClick={event => event.currentTarget.closest('details')?.removeAttribute('open')}
                  className="block border-b border-white/10 px-4 py-3 hover:bg-white/[0.05] focus-visible:outline focus-visible:outline-2 focus-visible:outline-inset focus-visible:outline-queue-blue"
                >
                  <span className="block text-sm font-medium text-text-primary">{sectionLabel(section)} settings</span>
                  <span className="mt-0.5 block text-xs text-text-muted">{SECTION_SETTINGS_HELP[section]}</span>
                </a>
              ))}
              <a
                href={`#${healthTarget}`}
                onClick={event => event.currentTarget.closest('details')?.removeAttribute('open')}
                className="block px-4 py-3 hover:bg-white/[0.05] focus-visible:outline focus-visible:outline-2 focus-visible:outline-inset focus-visible:outline-queue-blue"
              >
                <span className="block text-sm font-medium text-text-primary">Configuration & recovery</span>
                <span className="mt-0.5 block text-xs text-text-muted">Health, warnings, errors, and safe baseline recovery</span>
              </a>
            </nav>
          </details>
        </div>
      </div>
      {logoutError && <p role="alert" className="mt-2 text-sm text-danger-rose">{logoutError}</p>}
      <label className="mt-3 block md:hidden">
        <span className="sr-only">Dashboard page</span>
        <select
          aria-label="Dashboard page"
          className="min-h-11 w-full border-white/15 bg-[#07101d] px-3 text-sm text-text-primary sm:min-h-10"
          value={page}
          onChange={event => { window.location.hash = event.target.value; }}
        >
          <option value="home">Home</option>
          <option value="requests">Requests</option>
          <option value="settings">API Settings</option>
          {DASHBOARD_SECTIONS.map(section => (
            <optgroup key={section} label={sectionLabel(section)}>
              {DASHBOARD_PAGES.filter(item => item.section === section).map(({ id, label }) => <option key={id} value={id}>{label}</option>)}
            </optgroup>
          ))}
          <optgroup label="Planned">
            {DASHBOARD_PAGES.filter(item => item.preview).map(({ id, label }) => <option key={id} value={id}>{label}</option>)}
          </optgroup>
        </select>
      </label>
    </header>
  );
};

const HealthNotices: React.FC = () => {
  const { connection, models } = useGateway();
  const [notices, setNotices] = useState<string[]>([]);
  const [dismissed, setDismissed] = useState(false);

  useEffect(() => {
    let active = true;
    Promise.allSettled([getHealth(), getPricing()]).then(results => {
      if (!active) return;
      const next: string[] = [];
      const health = results[0];
      const pricing = results[1];
      if (health.status === 'rejected') {
        next.push('Gateway health details are unavailable. Open Health & alerts for connection and error details.');
      } else if (!health.value.db_healthy) {
        next.push('Usage database is unhealthy. Request history and cost totals may be incomplete.');
      }
      if (pricing.status === 'rejected' || pricing.value.length === 0) {
        next.push('Server pricing is not configured. Cost estimates cannot be shared consistently across devices.');
      }
      try {
        const local = JSON.parse(localStorage.getItem(COST_STORAGE_KEY) || '{}') as Record<string, { userEdited?: boolean }>;
        if (Object.values(local).some(entry => entry?.userEdited)) {
          next.push('This browser has legacy local price overrides. They are ignored; migrate the values into Model Settings, then clear the site data.');
        }
      } catch {
        next.push('This browser has unreadable legacy pricing data. It is ignored; clear the site data before trusting prior cost estimates.');
      }
      const unavailable = models.filter(model => model.runtime_available === false);
      if (unavailable.length) {
        next.push(`${unavailable.length} configured model runtime${unavailable.length === 1 ? ' is' : 's are'} unavailable on this build.`);
      }
      setNotices(next);
    });
    return () => { active = false; };
  }, [models]);

  if (connection !== 'connected' || dismissed || notices.length === 0) return null;
  return (
    <aside className="border-b border-warning-amber/30 bg-warning-amber/10 px-4 py-2 text-sm text-warning-amber sm:px-6" aria-label="Configuration notices">
      <div className="flex items-start justify-between gap-4">
        <ul className="space-y-1">
          {notices.map(notice => <li key={notice}>{notice}</li>)}
        </ul>
        <button
          type="button"
          aria-label="Dismiss configuration notices"
          title="Dismiss configuration notices"
          onClick={() => setDismissed(true)}
          className="inline-flex min-h-11 min-w-11 shrink-0 items-center justify-center rounded hover:bg-white/10 focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-warning-amber sm:min-h-0 sm:min-w-0 sm:p-1"
        >
          <XMarkIcon className="h-4 w-4" aria-hidden="true" />
        </button>
      </div>
    </aside>
  );
};

const NavLink: React.FC<{ id: PageId; label: string; page: PageId; nested?: boolean }> = ({ id, label, page, nested }) => (
  <a
    href={`#${id}`}
    aria-current={page === id ? 'page' : undefined}
    className={`relative rounded px-3 py-2 text-sm transition-colors ${nested ? 'pl-5' : ''} ${page === id
      ? 'bg-white/[0.07] font-medium text-text-primary'
      : 'text-text-secondary hover:bg-white/[0.04] hover:text-text-primary'}`}
  >
    {page === id && <span className="absolute inset-y-1.5 left-0 w-0.5 rounded bg-queue-blue" />}
    {label}
  </a>
);

const ConnectionBanner: React.FC = () => {
  const { connection, lastUpdatedAt, refresh } = useGateway();
  if (connection === 'connected') return null;
  return <div className="border-b border-white/20 px-4 py-3 text-sm sm:px-6" role="status">
    <span>{connection === 'connecting' ? 'Connecting to InferDeck.' : 'Gateway unavailable. Reconnecting.'}</span>
    {lastUpdatedAt && <span className="ml-2">Data last updated {timeAgo(lastUpdatedAt)}.</span>}
    <button className="ml-3 min-h-11 underline" onClick={() => { void refresh(); }}>Retry connection</button>
  </div>;
};

export default App;
