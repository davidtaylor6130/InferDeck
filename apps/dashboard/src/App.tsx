import React, { useEffect, useState } from 'react';
import { XMarkIcon } from '@heroicons/react/24/outline';
import { getHealth, getPricing } from './api';
import { Badge } from './components/ui';
import { COST_STORAGE_KEY } from './cost';
import type { DashboardSection } from './dashboardSections';
import { GatewayProvider, useGateway } from './gateway';
import { OverviewPage } from './pages/OverviewPage';
import { ModelsPage } from './pages/ModelsPage';
import { OperatePage } from './pages/OperatePage';
import { UsagePage } from './pages/UsagePage';
import { SystemPage } from './pages/SystemPage';
import { compactModel, timeAgo } from './utils';
import { INFERDECK_VERSION } from './version';
import logoUrl from '../../../Assets/Logo.png';

export type PageId =
  | 'home'
  | 'llm/settings'
  | 'llm/models'
  | 'llm/usage'
  | 'llm/diagnostics'
  | 'dictation/settings'
  | 'dictation/models'
  | 'dictation/usage'
  | 'dictation/diagnostics';

interface DashboardPage {
  id: PageId;
  label: string;
  section?: DashboardSection;
}

export const DASHBOARD_PAGES: ReadonlyArray<DashboardPage> = [
  { id: 'home', label: 'Home' },
  { id: 'llm/settings', label: 'Model Settings', section: 'llm' },
  { id: 'llm/models', label: 'Model Store', section: 'llm' },
  { id: 'llm/usage', label: 'Usage', section: 'llm' },
  { id: 'llm/diagnostics', label: 'Diagnostics', section: 'llm' },
  { id: 'dictation/settings', label: 'Model Settings', section: 'dictation' },
  { id: 'dictation/models', label: 'Model Store', section: 'dictation' },
  { id: 'dictation/usage', label: 'Usage', section: 'dictation' },
  { id: 'dictation/diagnostics', label: 'Diagnostics', section: 'dictation' },
];

const LEGACY_ROUTES: Record<string, PageId> = {
  overview: 'home',
  models: 'llm/models',
  usage: 'llm/usage',
  system: 'llm/diagnostics',
  'llm/operate': 'llm/settings',
  'dictation/operate': 'dictation/settings',
};

function pageFromHash(): PageId {
  const hash = window.location.hash.replace(/^#\/?/, '');
  const legacy = LEGACY_ROUTES[hash];
  if (legacy) return legacy;
  return (DASHBOARD_PAGES.some(page => page.id === hash) ? hash : 'home') as PageId;
}

const App: React.FC = () => (
  <GatewayProvider>
    <Shell />
  </GatewayProvider>
);

const Shell: React.FC = () => {
  const [page, setPage] = useState<PageId>(() => (typeof window === 'undefined' ? 'home' : pageFromHash()));

  useEffect(() => {
    const onHashChange = () => setPage(pageFromHash());
    window.addEventListener('hashchange', onHashChange);
    return () => window.removeEventListener('hashchange', onHashChange);
  }, []);

  return (
    <div className="app-shell flex min-h-screen">
      <aside className="hidden w-52 shrink-0 flex-col border-r border-border-slate bg-deck-navy px-4 py-5 md:flex">
        <div className="mb-6 flex items-center gap-3 px-2">
          <img src={logoUrl} alt="" className="h-9 w-9 rounded-md object-cover" />
          <div>
            <span className="text-base font-semibold text-text-primary">InferDeck</span>
            <span className="mt-0.5 block text-xs text-text-muted">Local inference</span>
          </div>
        </div>
        <nav className="flex flex-col" aria-label="Dashboard">
          <NavLink id="home" label="Home" page={page} />
          {(['llm', 'dictation'] as DashboardSection[]).map(section => (
            <div key={section} className="mt-5">
              <div className="mb-1 px-3 text-[11px] font-semibold uppercase tracking-[0.14em] text-text-muted">
                {section === 'llm' ? 'LLM' : 'Dictation'}
              </div>
              <div className="flex flex-col gap-1">
                {DASHBOARD_PAGES.filter(item => item.section === section).map(({ id, label }) => (
                  <NavLink key={id} id={id} label={label} page={page} nested />
                ))}
              </div>
            </div>
          ))}
        </nav>
        <div className="mt-auto px-2 pt-6 text-xs text-text-muted">
          <span className="block">InferDeck v{INFERDECK_VERSION}</span>
          <span className="mt-0.5 block text-[10px]">In-process runtime</span>
        </div>
      </aside>

      <div className="flex min-w-0 flex-1 flex-col">
        <TopBar page={page} />
        <ConnectionBanner />
        <HealthNotices />
        <main className="min-w-0 flex-1 overflow-y-auto px-4 py-5 sm:px-6">
          <div className="mx-auto max-w-[1280px]">
            {page === 'home' && <OverviewPage />}
            {page === 'llm/settings' && <OperatePage section="llm" />}
            {page === 'llm/models' && <ModelsPage section="llm" />}
            {page === 'llm/usage' && <UsagePage section="llm" />}
            {page === 'llm/diagnostics' && <SystemPage section="llm" />}
            {page === 'dictation/settings' && <OperatePage section="dictation" />}
            {page === 'dictation/models' && <ModelsPage section="dictation" />}
            {page === 'dictation/usage' && <UsagePage section="dictation" />}
            {page === 'dictation/diagnostics' && <SystemPage section="dictation" />}
          </div>
        </main>
      </div>
    </div>
  );
};

const TopBar: React.FC<{ page: PageId }> = ({ page }) => {
  const { connection, stats, swap } = useGateway();
  const loaded = stats?.loadedModel || '';
  const pageInfo = DASHBOARD_PAGES.find(item => item.id === page);
  const connectionTone = connection === 'connected' ? 'good' : connection === 'offline' ? 'critical' : 'warn';
  const connectionLabel = connection === 'connected' ? 'Live' : connection === 'connecting' ? 'Connecting' : connection === 'reconnecting' ? 'Reconnecting' : 'Offline';

  return (
    <header className="border-b border-border-slate bg-deck-navy px-4 py-3 sm:px-6">
      <div className="flex min-w-0 items-center justify-between gap-3">
        <span className="text-base font-semibold text-text-primary md:hidden">InferDeck</span>
        <h1 className="hidden text-base font-semibold text-text-primary md:block">
          {pageInfo?.section ? `${pageInfo.section === 'llm' ? 'LLM' : 'Dictation'} / ${pageInfo.label}` : pageInfo?.label}
        </h1>
        <div className="flex shrink-0 items-center gap-2">
          {swap.swapping ? (
            <Badge label={`Switching to ${compactModel(swap.target)}`} tone="info" />
          ) : loaded ? (
            <span className="hidden text-xs text-text-secondary sm:inline">{compactModel(loaded)}</span>
          ) : (
            <span className="hidden text-xs text-text-muted sm:inline">No model loaded</span>
          )}
          <Badge label={connectionLabel} tone={connectionTone} />
        </div>
      </div>
      <span className="mt-1 block text-[10px] text-text-muted md:hidden">v{INFERDECK_VERSION}</span>
      <nav className="-mx-1 mt-3 flex min-w-0 gap-1 overflow-x-auto pb-1 md:hidden" aria-label="Dashboard sections">
        <a
          href="#home"
          aria-current={page === 'home' ? 'page' : undefined}
          className={`shrink-0 rounded px-2.5 py-1.5 text-xs ${page === 'home' ? 'bg-white/10 text-text-primary' : 'text-text-muted'}`}
        >
          Home
        </a>
        {(['llm', 'dictation'] as DashboardSection[]).map(section => {
          const active = pageInfo?.section === section;
          return (
            <a
              key={section}
              href={`#${section}/settings`}
              aria-current={active ? 'true' : undefined}
              className={`shrink-0 rounded px-2.5 py-1.5 text-xs ${active ? 'bg-white/10 text-text-primary' : 'text-text-muted'}`}
            >
              {section === 'llm' ? 'LLM' : 'Dictation'}
            </a>
          );
        })}
      </nav>
      {pageInfo?.section && (
        <nav className="-mx-1 mt-1 flex min-w-0 gap-1 overflow-x-auto pb-1 md:hidden" aria-label={`${pageInfo.section} pages`}>
          {DASHBOARD_PAGES.filter(item => item.section === pageInfo.section).map(({ id, label }) => (
          <a
            key={id}
            href={`#${id}`}
            aria-current={page === id ? 'page' : undefined}
            className={`shrink-0 rounded px-2.5 py-1.5 text-xs ${page === id ? 'bg-white/10 text-text-primary' : 'text-text-muted'}`}
          >
            {label}
          </a>
          ))}
        </nav>
      )}
    </header>
  );
};

const HealthNotices: React.FC = () => {
  const { models } = useGateway();
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
        next.push('Gateway health details are unavailable. Check Diagnostics for connection and log details.');
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

  if (dismissed || notices.length === 0) return null;
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
          className="rounded p-1 hover:bg-white/10 focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-warning-amber"
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
    className={`rounded px-3 py-2 text-sm transition-colors ${nested ? 'pl-5' : ''} ${page === id
      ? 'bg-white/[0.07] font-medium text-text-primary'
      : 'text-text-secondary hover:bg-white/[0.04] hover:text-text-primary'}`}
  >
    {label}
  </a>
);

const ConnectionBanner: React.FC = () => {
  const { connection, lastUpdatedAt } = useGateway();
  if (connection === 'connected') return null;
  const tone = connection === 'offline' ? 'border-danger-rose/40 bg-danger-rose/10 text-danger-rose' : 'border-warning-amber/40 bg-warning-amber/10 text-warning-amber';
  const message = connection === 'connecting'
    ? 'Connecting to the gateway…'
    : connection === 'reconnecting'
      ? 'Event stream interrupted — reconnecting.'
      : 'Gateway unreachable — retrying.';
  return (
    <div className={`border-b px-4 py-2 text-sm ${tone}`}>
      {message}
      {lastUpdatedAt && <span className="ml-2 opacity-80">Data last updated {timeAgo(lastUpdatedAt)}.</span>}
    </div>
  );
};

export default App;
