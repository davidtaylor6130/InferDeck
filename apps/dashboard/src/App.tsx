import React, { useEffect, useState } from 'react';
import {
  ChartBarIcon,
  CircleStackIcon,
  CpuChipIcon,
  Squares2X2Icon,
} from '@heroicons/react/24/outline';
import { Badge } from './components/ui';
import { GatewayProvider, useGateway } from './gateway';
import { OverviewPage } from './pages/OverviewPage';
import { ModelsPage } from './pages/ModelsPage';
import { UsagePage } from './pages/UsagePage';
import { SystemPage } from './pages/SystemPage';
import { compactModel, timeAgo } from './utils';

type PageId = 'overview' | 'models' | 'usage' | 'system';

const PAGES: Array<{ id: PageId; label: string; Icon: React.ComponentType<React.SVGProps<SVGSVGElement>> }> = [
  { id: 'overview', label: 'Overview', Icon: Squares2X2Icon },
  { id: 'models', label: 'Models', Icon: CircleStackIcon },
  { id: 'usage', label: 'Usage & Cost', Icon: ChartBarIcon },
  { id: 'system', label: 'System', Icon: CpuChipIcon },
];

function pageFromHash(): PageId {
  const hash = window.location.hash.replace(/^#\/?/, '');
  return (PAGES.some(page => page.id === hash) ? hash : 'overview') as PageId;
}

const App: React.FC = () => (
  <GatewayProvider>
    <Shell />
  </GatewayProvider>
);

const Shell: React.FC = () => {
  const [page, setPage] = useState<PageId>(() => (typeof window === 'undefined' ? 'overview' : pageFromHash()));

  useEffect(() => {
    const onHashChange = () => setPage(pageFromHash());
    window.addEventListener('hashchange', onHashChange);
    return () => window.removeEventListener('hashchange', onHashChange);
  }, []);

  return (
    <div className="deck-grid-bg flex min-h-screen">
      <aside className="hidden w-56 shrink-0 flex-col border-r border-white/10 bg-[#070d18] p-4 md:flex">
        <div className="mb-6 flex items-center gap-2 px-2">
          <span className="grid h-8 w-8 place-items-center rounded-lg bg-infer-violet/20 text-infer-violet">⌁</span>
          <span className="text-lg font-semibold text-text-primary">InferDeck</span>
        </div>
        <nav className="flex flex-col gap-1">
          {PAGES.map(({ id, label, Icon }) => (
            <a
              key={id}
              href={`#${id}`}
              className={`flex items-center gap-3 rounded-md px-3 py-2 text-sm transition ${page === id
                ? 'bg-white/[0.08] font-medium text-text-primary'
                : 'text-text-secondary hover:bg-white/[0.04] hover:text-text-primary'}`}
            >
              <Icon className="h-5 w-5" />
              {label}
            </a>
          ))}
        </nav>
        <div className="mt-auto px-2 text-xs text-text-muted">InferDeck 2.0 · llama.cpp in-process</div>
      </aside>

      <div className="flex min-w-0 flex-1 flex-col">
        <TopBar page={page} />
        <ConnectionBanner />
        <main className="min-w-0 flex-1 overflow-y-auto p-4">
          <div className="mx-auto max-w-[1400px]">
            {page === 'overview' && <OverviewPage />}
            {page === 'models' && <ModelsPage />}
            {page === 'usage' && <UsagePage />}
            {page === 'system' && <SystemPage />}
          </div>
        </main>
      </div>
    </div>
  );
};

const TopBar: React.FC<{ page: PageId }> = ({ page }) => {
  const { connection, stats, swap } = useGateway();
  const loaded = stats?.loadedModel || '';
  const connectionTone = connection === 'connected' ? 'good' : connection === 'offline' ? 'critical' : 'warn';
  const connectionLabel = connection === 'connected' ? 'Live' : connection === 'connecting' ? 'Connecting' : connection === 'reconnecting' ? 'Reconnecting' : 'Offline';

  return (
    <header className="flex flex-wrap items-center justify-between gap-3 border-b border-white/10 bg-[#070d18]/80 px-4 py-3">
      <div className="flex items-center gap-3 md:hidden">
        <span className="text-base font-semibold text-text-primary">InferDeck</span>
        <nav className="flex gap-1">
          {PAGES.map(({ id, label }) => (
            <a key={id} href={`#${id}`} className={`rounded px-2 py-1 text-xs ${page === id ? 'bg-white/10 text-text-primary' : 'text-text-muted'}`}>{label}</a>
          ))}
        </nav>
      </div>
      <h1 className="hidden text-lg font-semibold capitalize text-text-primary md:block">
        {PAGES.find(item => item.id === page)?.label}
      </h1>
      <div className="flex flex-wrap items-center gap-2">
        {swap.swapping ? (
          <Badge label={`Swapping → ${compactModel(swap.target)}`} tone="info" />
        ) : loaded ? (
          <Badge label={compactModel(loaded)} tone="good" />
        ) : (
          <Badge label="No model" tone="idle" />
        )}
        <Badge label={connectionLabel} tone={connectionTone} />
      </div>
    </header>
  );
};

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
