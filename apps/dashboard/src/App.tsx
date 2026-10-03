import React, { useEffect, useRef, useState } from 'react';
import { flushSync } from 'react-dom';
import {
  BeakerIcon,
  ChartBarIcon,
  ChevronRightIcon,
  CubeIcon,
  HomeIcon,
  KeyIcon,
  QueueListIcon,
  ShieldCheckIcon,
  SparklesIcon,
  XMarkIcon,
} from '@heroicons/react/24/outline';
import { getHealth, getPricing } from './api';
import { DashboardAccess, useDashboardAccess } from './components/DashboardAccess';
import { FeedbackProvider, useFeedback } from './components/Feedback';
import { ThemeSwitch } from './components/ThemeSwitch';
import { SectionSwitch } from './components/SectionSwitch';
import { Dot } from './components/ui';
import { SlidingIndicator, useSlidingIndicator } from './components/motion';
import { COST_STORAGE_KEY } from './cost';
import { sectionLabel } from './dashboardSections';
import { GatewayProvider, useGateway } from './gateway';
import { ApiSettingsPage } from './pages/ApiSettingsPage';
import { FutureWorkspacePage } from './pages/FutureWorkspacePage';
import { ImagePage } from './pages/ImagePage';
import { ModelDetailPage } from './pages/ModelDetailPage';
import { ModelsPage } from './pages/ModelsPage';
import { MusicPage } from './pages/MusicPage';
import { OperatePage } from './pages/OperatePage';
import { OverviewPage } from './pages/OverviewPage';
import { RequestsPage } from './pages/RequestsPage';
import { SystemPage } from './pages/SystemPage';
import { UsagePage } from './pages/UsagePage';
import { VideoPage } from './pages/VideoPage';
import { NAV_ITEMS, navIdForRoute, parseRoute, routeDepth, routeTitle, type NavId, type Route } from './routes';
import { compactModel, timeAgo } from './utils';
import { INFERDECK_VERSION } from './version';
import logoUrl from '../../../Assets/Logo.png';

const NAV_ICONS: Record<NavId, React.ComponentType<React.SVGProps<SVGSVGElement>>> = {
  home: HomeIcon,
  models: CubeIcon,
  generate: SparklesIcon,
  requests: QueueListIcon,
  usage: ChartBarIcon,
  health: ShieldCheckIcon,
  settings: KeyIcon,
  'post-training': BeakerIcon,
};

const supportsViewTransitions = typeof document !== 'undefined' && 'startViewTransition' in document;

const currentRoute = (): Route => (typeof window === 'undefined' ? { page: 'home' } : parseRoute(window.location.hash));

const App: React.FC = () => (
  <FeedbackProvider><DashboardAccess><GatewayProvider>
    <Shell />
  </GatewayProvider></DashboardAccess></FeedbackProvider>
);

const Shell: React.FC = () => {
  const [route, setRoute] = useState<Route>(currentRoute);
  const routeKey = JSON.stringify(route);

  const routeRef = useRef(route);
  routeRef.current = route;

  useEffect(() => {
    const onHashChange = () => {
      const next = currentRoute();
      const previous = routeRef.current;
      const doc = document as Document & { startViewTransition?: (update: () => void) => unknown };
      const depthChange = routeDepth(next) - routeDepth(previous);
      const sameArea = navIdForRoute(next) === navIdForRoute(previous);
      document.documentElement.dataset.nav = depthChange > 0 ? 'forward' : depthChange < 0 ? 'back' : sameArea ? 'lateral' : 'switch';
      if (doc.startViewTransition) doc.startViewTransition(() => flushSync(() => setRoute(next)));
      else setRoute(next);
    };
    window.addEventListener('hashchange', onHashChange);
    return () => window.removeEventListener('hashchange', onHashChange);
  }, []);

  useEffect(() => {
    document.getElementById('main-scroll')?.scrollTo({ top: 0 });
  }, [routeKey]);

  const active = navIdForRoute(route);
  const sidebar = useSlidingIndicator<HTMLElement>(active);

  return (
    <div className="app-shell flex h-dvh overflow-hidden bg-void-black">
      <aside className="chrome-sidebar hidden w-56 shrink-0 flex-col border-r border-border-slate bg-deck-navy md:flex">
        <div className="flex h-14 items-center gap-2.5 px-4">
          <img src={logoUrl} alt="" className="h-7 w-7 rounded-md object-cover" />
          <span className="text-base font-semibold text-text-primary">InferDeck</span>
        </div>
        <nav ref={sidebar.container} className="relative flex min-h-0 flex-1 flex-col gap-0.5 overflow-y-auto px-2.5 py-2" aria-label="Dashboard">
          <SlidingIndicator box={sidebar.box} animated={sidebar.animated} />
          {NAV_ITEMS.map(item => (
            <NavLink key={item.id} id={item.id} href={item.href} label={item.label} active={active} glide={sidebar.ready} />
          ))}
          <div className="mt-auto flex flex-col gap-0.5 border-t border-border-slate pt-2">
            <NavLink id="settings" href="#settings" label="API settings" active={active} glide={sidebar.ready} />
            <NavLink id="post-training" href="#post-training" label="Post training" active={active} glide={sidebar.ready} trailing={<span className="text-2xs text-text-muted">Planned</span>} />
          </div>
        </nav>
        <SidebarAccount />
      </aside>

      <div className="flex min-h-0 min-w-0 flex-1 flex-col overflow-hidden">
        <TopBar route={route} />
        <ConnectionBanner />
        <main id="main-scroll" className="min-h-0 min-w-0 flex-1 overflow-y-auto overscroll-contain">
          <HealthNotices />
          <div key={routeKey} className={`page-surface mx-auto max-w-[1200px] px-4 pb-28 pt-6 sm:px-6 md:pb-16 lg:px-8 ${supportsViewTransitions ? '' : 'animate-page-in'}`}>
            <RouteView route={route} />
          </div>
        </main>
        <TabBar active={active} />
      </div>
      <ModelEventToasts />
    </div>
  );
};

const RouteView: React.FC<{ route: Route }> = ({ route }) => {
  const { models } = useGateway();
  switch (route.page) {
    case 'home': return <OverviewPage />;
    case 'requests': return <RequestsPage />;
    case 'settings': return <ApiSettingsPage />;
    case 'post-training': return <FutureWorkspacePage area="post-training" />;
    case 'model': return <ModelDetailPage id={route.id} tab={route.tab} />;
    case 'generate': return (
      <>
        <SectionSwitch area="generate" value={route.kind} models={models} />
        {route.kind === 'image' ? <ImagePage /> : route.kind === 'music' ? <MusicPage /> : <VideoPage />}
      </>
    );
    default: return (
      <>
        <SectionSwitch area={route.page} value={route.section} models={models} />
        {route.page === 'models' && <OperatePage section={route.section} />}
        {route.page === 'store' && <ModelsPage section={route.section} repo={route.repo} />}
        {route.page === 'usage' && <UsagePage section={route.section} />}
        {route.page === 'health' && <SystemPage section={route.section} />}
      </>
    );
  }
};

const TAB_ITEMS: NavId[] = ['home', 'models', 'generate', 'usage', 'health'];

const TabBar: React.FC<{ active: NavId }> = ({ active }) => (
  <nav aria-label="Sections" className="chrome-tabbar fixed inset-x-0 bottom-0 z-30 border-t border-border-slate bg-deck-navy/95 pb-[env(safe-area-inset-bottom,0px)] backdrop-blur md:hidden">
    <div className="mx-auto grid max-w-lg grid-cols-5">
      {TAB_ITEMS.map(id => {
        const item = NAV_ITEMS.find(entry => entry.id === id)!;
        const Icon = NAV_ICONS[id];
        const current = active === id;
        return (
          <a key={id} href={item.href} aria-current={current ? 'page' : undefined} className={`flex min-h-[52px] flex-col items-center justify-center gap-0.5 text-2xs font-medium ${current ? 'text-queue-blue' : 'text-text-muted'}`}>
            <Icon className={`h-6 w-6 transition-transform duration-300 ease-[cubic-bezier(0.3,1.4,0.4,1)] ${current ? 'scale-110' : 'scale-100'}`} aria-hidden="true" />
            {item.label}
          </a>
        );
      })}
    </div>
  </nav>
);

const ModelEventToasts: React.FC = () => {
  const { activity } = useGateway();
  const { toast } = useFeedback();
  const seen = useRef<Set<string> | null>(null);
  useEffect(() => {
    if (!seen.current) {
      seen.current = new Set(activity.map(item => item.id));
      return;
    }
    for (const item of activity) {
      if (seen.current.has(item.id)) continue;
      seen.current.add(item.id);
      if (item.kind === 'swap') toast(item.label, { tone: item.tone, detail: item.detail });
    }
  }, [activity, toast]);
  return null;
};

const SidebarAccount: React.FC = () => {
  const access = useDashboardAccess();
  const { connection } = useGateway();
  const [error, setError] = useState('');
  return <div className="shrink-0 space-y-2 border-t border-border-slate p-2.5">
    <ThemeSwitch />
    {access?.remote ? (
      <details className="relative">
        <summary className="flex cursor-pointer list-none items-center gap-2 rounded px-2 py-1.5 transition-colors hover:bg-panel-slate">
          <Dot tone={connection === 'connected' ? 'good' : connection === 'offline' ? 'critical' : 'warn'} />
          <span className="min-w-0 flex-1 truncate text-sm text-text-secondary">Dashboard session</span>
          <ChevronRightIcon className="h-3 w-3 -rotate-90 text-text-muted" aria-hidden="true" />
        </summary>
        <nav aria-label="Account" className="absolute bottom-full left-0 z-40 mb-1 w-full overflow-hidden rounded-md border border-line-strong bg-panel-slate p-1 text-sm shadow-deck">
          <button className="flex w-full items-center gap-2 rounded px-2 py-1.5 text-left text-danger-rose hover:bg-elevated-slate" onClick={() => { void access.logout().catch(() => setError('Log out failed. Try again.')); }}>Log out</button>
        </nav>
      </details>
    ) : (
      <p className="flex items-center gap-2 px-2 py-1.5 text-sm text-text-secondary">
        <Dot tone={connection === 'connected' ? 'good' : connection === 'offline' ? 'critical' : 'warn'} />
        Local access
      </p>
    )}
    {error && <p role="alert" className="mt-2 text-xs text-danger-rose">{error}</p>}
    <p className="mt-1 px-2 text-2xs text-text-muted">InferDeck v{INFERDECK_VERSION}</p>
  </div>;
};

const TopBar: React.FC<{ route: Route }> = ({ route }) => {
  const { connection, stats, swap } = useGateway();
  const access = useDashboardAccess();
  const [logoutError, setLogoutError] = useState('');
  const [loggingOut, setLoggingOut] = useState(false);
  const loaded = stats?.loadedModel || '';
  const connectionTone = connection === 'connected' ? 'good' : connection === 'offline' ? 'critical' : 'warn';
  const connectionLabel = connection === 'connected' ? 'Live' : connection === 'connecting' ? 'Connecting' : connection === 'reconnecting' ? 'Reconnecting' : 'Offline';
  const healthTarget = 'section' in route ? `#health/${route.section}` : '#health/llm';
  const navId = navIdForRoute(route);
  const area = NAV_ITEMS.find(item => item.id === navId);
  const crumbs: Array<{ label: string; href?: string; mono?: boolean }> = [];
  if (route.page === 'model') {
    crumbs.push({ label: 'Models', href: '#models/llm' }, { label: route.id, mono: true });
  } else if (route.page === 'store') {
    crumbs.push({ label: 'Models', href: `#models/${route.section}` }, { label: `Get ${sectionLabel(route.section)} models`, href: route.repo ? `#store/${route.section}` : undefined });
    if (route.repo) crumbs.push({ label: route.repo.split('/').pop() || route.repo, mono: true });
  } else if ('section' in route) {
    crumbs.push({ label: area?.label ?? '' }, { label: sectionLabel(route.section) });
  } else if (route.page === 'generate') {
    crumbs.push({ label: 'Generate' }, { label: route.kind[0].toUpperCase() + route.kind.slice(1) });
  } else {
    crumbs.push({ label: routeTitle(route) });
  }

  return (
    <header className="chrome-header sticky top-0 z-20 border-b border-border-slate bg-void-black/90 px-4 pt-[env(safe-area-inset-top,0px)] backdrop-blur sm:px-6 lg:px-8">
      <div className="flex min-h-14 min-w-0 items-center justify-between gap-3">
        <nav aria-label="Breadcrumb" className="flex min-w-0 items-center gap-2 text-sm">
          <img src={logoUrl} alt="" className="h-6 w-6 rounded object-cover md:hidden" />
          {crumbs.map((crumb, index) => (
            <React.Fragment key={index}>
              {index > 0 && <span className="text-text-muted" aria-hidden="true">/</span>}
              {crumb.href
                ? <a href={crumb.href} className="hidden text-text-muted hover:text-text-primary sm:inline">{crumb.label}</a>
                : <span className={`truncate ${index === crumbs.length - 1 ? 'text-text-primary' : 'text-text-muted'} ${crumb.mono ? 'font-mono' : ''}`}>{crumb.label}</span>}
            </React.Fragment>
          ))}
        </nav>
        <div className="flex shrink-0 items-center gap-1">
          {swap.swapping ? (
            <span className="hidden items-center gap-2 px-2 text-xs text-queue-blue sm:inline-flex">
              <span className="h-3 w-3 animate-spin rounded-full border-2 border-queue-blue border-t-transparent" aria-hidden="true" />
              Switching to {compactModel(swap.target)}
            </span>
          ) : loaded ? (
            <a href="#home" className="hidden max-w-[280px] truncate px-2 font-mono text-xs text-text-muted hover:text-text-secondary lg:inline">
              {compactModel(loaded)}
            </a>
          ) : null}
          <a
            href={healthTarget}
            aria-label={`${connectionLabel}. Open Health and alerts`}
            title="Open Health"
            className="inline-flex min-h-11 min-w-11 items-center justify-center gap-1.5 rounded-full px-2.5 text-xs font-medium text-text-secondary hover:bg-elevated-slate md:min-h-8 md:min-w-0"
          >
            <Dot tone={connectionTone} />
            {connectionLabel}
          </a>
          <a href="#requests" aria-label="Requests" className="inline-flex h-11 w-11 items-center justify-center rounded-md text-text-secondary hover:bg-elevated-slate md:hidden"><QueueListIcon className="h-5 w-5" aria-hidden="true" /></a>
          <a href="#settings" aria-label="API settings" className="inline-flex h-11 w-11 items-center justify-center rounded-md text-text-secondary hover:bg-elevated-slate md:hidden"><KeyIcon className="h-5 w-5" aria-hidden="true" /></a>
          {access?.remote && (
            <button className="hidden min-h-11 rounded-md border border-line-strong bg-panel-slate shadow-card px-3 text-sm hover:bg-panel-slate md:inline-flex md:min-h-8 md:items-center" disabled={loggingOut} onClick={() => {
              setLoggingOut(true); setLogoutError('');
              void access.logout().catch(reason => { setLogoutError(reason instanceof Error ? reason.message : 'Log out failed. Try again.'); setLoggingOut(false); });
            }}>{loggingOut ? 'Logging out...' : 'Log out'}</button>
          )}
        </div>
      </div>
      {logoutError && <p role="alert" className="pb-2 text-sm text-danger-rose">{logoutError}</p>}
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
        next.push('Gateway health details are unavailable. Open Health for connection and error details.');
      } else if (!health.value.db_healthy) {
        next.push('Usage database is unhealthy. Request history and cost totals may be incomplete.');
      }
      if (pricing.status === 'rejected' || pricing.value.length === 0) {
        next.push('Server pricing is not configured. Cost estimates cannot be shared consistently across devices.');
      }
      try {
        const local = JSON.parse(localStorage.getItem(COST_STORAGE_KEY) || '{}') as Record<string, { userEdited?: boolean }>;
        if (Object.values(local).some(entry => entry?.userEdited)) {
          next.push('This browser has legacy local price overrides. They are ignored; move the values into each model\'s pricing settings, then clear the site data.');
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
    <aside className="mx-auto max-w-[1200px] px-4 pt-4 sm:px-6 lg:px-8" aria-label="Configuration notices">
      <div className="flex items-start gap-3 rounded-lg border border-border-slate border-l-warning-amber bg-panel-slate px-3.5 py-2.5 text-sm text-text-secondary shadow-card [border-left-width:3px]">
        <ul className="min-w-0 flex-1 space-y-0.5">
          {notices.map(notice => <li key={notice}>{notice}</li>)}
        </ul>
        <button
          type="button"
          aria-label="Dismiss configuration notices"
          title="Dismiss configuration notices"
          onClick={() => setDismissed(true)}
          className="inline-flex min-h-8 min-w-8 shrink-0 items-center justify-center rounded text-text-muted transition-colors hover:bg-elevated-slate hover:text-text-primary"
        >
          <XMarkIcon className="h-4 w-4" aria-hidden="true" />
        </button>
      </div>
    </aside>
  );
};

const NavLink: React.FC<{
  id: NavId;
  href: string;
  label: string;
  active: NavId;
  trailing?: React.ReactNode;
  glide?: boolean;
}> = ({ id, href, label, active, trailing, glide }) => {
  const current = active === id;
  const Icon = NAV_ICONS[id];
  return (
    <a
      href={href}
      aria-current={current ? 'page' : undefined}
      data-active={current}
      className={`relative flex min-h-9 items-center gap-2.5 rounded-md px-2.5 text-sm font-medium ${current
        ? `text-text-primary ${glide ? '' : 'bg-panel-slate shadow-card'}`
        : 'text-text-secondary hover:bg-panel-slate/60 hover:text-text-primary'}`}
    >
      <Icon className={`h-[18px] w-[18px] shrink-0 transition-colors duration-300 ${current ? 'text-queue-blue' : 'text-text-muted'}`} aria-hidden="true" />
      <span className="flex-1 truncate">{label}</span>
      {trailing}
    </a>
  );
};

const ConnectionBanner: React.FC = () => {
  const { connection, lastUpdatedAt, refresh } = useGateway();
  if (connection === 'connected') return null;
  return <div className="flex flex-wrap items-center gap-x-3 gap-y-1 border-b border-border-slate bg-panel-slate px-4 py-2 text-sm text-warning-amber sm:px-6 lg:px-8" role="status">
    <span className="h-3 w-3 animate-spin rounded-full border-2 border-warning-amber border-t-transparent" aria-hidden="true" />
    <span>{connection === 'connecting' ? 'Connecting to InferDeck.' : 'Gateway unavailable. Reconnecting.'}</span>
    {lastUpdatedAt && <span className="text-warning-amber/70">Data last updated {timeAgo(lastUpdatedAt)}.</span>}
    <button className="min-h-11 font-semibold underline-offset-2 hover:underline md:min-h-0" onClick={() => { void refresh(); }}>Retry connection</button>
  </div>;
};

export default App;
