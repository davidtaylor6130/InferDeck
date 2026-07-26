import React, { useEffect, useState } from 'react';
import { Badge } from './components/ui';
import type { DashboardSection } from './dashboardSections';
import { GatewayProvider, useGateway } from './gateway';
import { OverviewPage } from './pages/OverviewPage';
import { ModelsPage } from './pages/ModelsPage';
import { OperatePage } from './pages/OperatePage';
import { UsagePage } from './pages/UsagePage';
import { SystemPage } from './pages/SystemPage';
import { compactModel, timeAgo } from './utils';
import { INFERDECK_VERSION } from './version';

export type PageId =
  | 'home'
  | 'llm/operate'
  | 'llm/models'
  | 'llm/usage'
  | 'llm/diagnostics'
  | 'dictation/operate'
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
  { id: 'llm/operate', label: 'Operate', section: 'llm' },
  { id: 'llm/models', label: 'Models', section: 'llm' },
  { id: 'llm/usage', label: 'Usage', section: 'llm' },
  { id: 'llm/diagnostics', label: 'Diagnostics', section: 'llm' },
  { id: 'dictation/operate', label: 'Operate', section: 'dictation' },
  { id: 'dictation/models', label: 'Models', section: 'dictation' },
  { id: 'dictation/usage', label: 'Usage', section: 'dictation' },
  { id: 'dictation/diagnostics', label: 'Diagnostics', section: 'dictation' },
];

const LEGACY_ROUTES: Record<string, PageId> = {
  overview: 'home',
  models: 'llm/models',
  usage: 'llm/usage',
  system: 'llm/diagnostics',
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
        <div className="mb-6 px-2">
          <span className="text-base font-semibold text-text-primary">InferDeck</span>
          <span className="mt-0.5 block text-xs text-text-muted">Local inference</span>
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
        <main className="min-w-0 flex-1 overflow-y-auto px-4 py-5 sm:px-6">
          <div className="mx-auto max-w-[1280px]">
            {page === 'home' && <OverviewPage />}
            {page === 'llm/operate' && <OperatePage section="llm" />}
            {page === 'llm/models' && <ModelsPage section="llm" />}
            {page === 'llm/usage' && <UsagePage section="llm" />}
            {page === 'llm/diagnostics' && <SystemPage section="llm" />}
            {page === 'dictation/operate' && <OperatePage section="dictation" />}
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
              href={`#${section}/operate`}
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
