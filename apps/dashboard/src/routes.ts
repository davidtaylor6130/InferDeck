import { DASHBOARD_SECTIONS, type DashboardSection } from './dashboardSections';

export type GenerateKind = 'image' | 'music' | 'video';
export type SectionArea = 'models' | 'store' | 'usage' | 'health';
export type ModelTab = 'resources' | 'sampling' | 'pricing' | 'runtime' | 'optimize' | 'yaml';

export type Route =
  | { page: 'home' }
  | { page: 'requests' }
  | { page: 'settings' }
  | { page: 'post-training' }
  | { page: SectionArea; section: DashboardSection; repo?: string }
  | { page: 'model'; id: string; tab?: ModelTab }
  | { page: 'generate'; kind: GenerateKind };

export type NavId = 'home' | 'models' | 'generate' | 'requests' | 'usage' | 'health' | 'settings' | 'post-training';

export const NAV_ITEMS: ReadonlyArray<{ id: NavId; label: string; href: string }> = [
  { id: 'home', label: 'Home', href: '#home' },
  { id: 'models', label: 'Models', href: '#models/llm' },
  { id: 'generate', label: 'Generate', href: '#generate/image' },
  { id: 'requests', label: 'Requests', href: '#requests' },
  { id: 'usage', label: 'Usage', href: '#usage/llm' },
  { id: 'health', label: 'Health', href: '#health/llm' },
];

export const GENERATE_KINDS: ReadonlyArray<GenerateKind> = ['image', 'music', 'video'];
const MODEL_TABS: ReadonlyArray<ModelTab> = ['resources', 'sampling', 'pricing', 'runtime', 'optimize', 'yaml'];

const LEGACY_AREA: Record<string, SectionArea | 'generate'> = {
  settings: 'models',
  operate: 'models',
  models: 'store',
  usage: 'usage',
  diagnostics: 'health',
  generate: 'generate',
};

const isSection = (value: string): value is DashboardSection =>
  (DASHBOARD_SECTIONS as ReadonlyArray<string>).includes(value);
const isKind = (value: string): value is GenerateKind =>
  (GENERATE_KINDS as ReadonlyArray<string>).includes(value);

export function parseRoute(hash: string): Route {
  const path = hash.replace(/^#\/?/, '');
  const [head = '', ...rest] = path.split('/');
  const tail = rest.join('/');
  if (head === 'requests' || head === 'settings' || head === 'post-training') return { page: head };
  if (head === 'overview') return { page: 'home' };
  if (head === 'system') return { page: 'health', section: 'llm' };
  if (head === 'store') {
    const [section = '', ...repo] = rest;
    const id = repo.length ? decodeURIComponent(repo.join('/')) : '';
    return id ? { page: 'store', section: isSection(section) ? section : 'llm', repo: id } : { page: 'store', section: isSection(section) ? section : 'llm' };
  }
  if (head === 'usage' || head === 'health' || head === 'models') {
    return { page: head, section: isSection(tail) ? tail : 'llm' };
  }
  if (head === 'generate') return { page: 'generate', kind: isKind(tail) ? tail : 'image' };
  if (head === 'image' && !tail) return { page: 'generate', kind: 'image' };
  if (head === 'music' && !tail) return { page: 'generate', kind: 'music' };
  if (head === 'model' && rest.length) {
    const tab = rest.length > 1 ? rest[rest.length - 1] : '';
    const id = decodeURIComponent(MODEL_TABS.includes(tab as ModelTab) ? rest.slice(0, -1).join('/') : tail);
    return { page: 'model', id, tab: MODEL_TABS.includes(tab as ModelTab) ? tab as ModelTab : undefined };
  }
  if (isSection(head) && LEGACY_AREA[tail]) {
    const area = LEGACY_AREA[tail];
    if (area === 'generate') return { page: 'generate', kind: isKind(head) ? head : 'image' };
    return { page: area, section: head };
  }
  return { page: 'home' };
}

export function routeHref(route: Route): string {
  if (route.page === 'model') return `#model/${encodeURIComponent(route.id)}${route.tab ? `/${route.tab}` : ''}`;
  if (route.page === 'generate') return `#generate/${route.kind}`;
  if ('section' in route) return `#${route.page}/${route.section}${route.repo ? `/${encodeURIComponent(route.repo)}` : ''}`;
  return `#${route.page}`;
}

export const storeHref = (section: DashboardSection, repo?: string) => routeHref({ page: 'store', section, repo });
export const modelHref = (id: string, tab?: ModelTab) => routeHref({ page: 'model', id, tab });

/** How deep a page sits, so navigation can push forward or slide back like a native stack. */
export function routeDepth(route: Route): number {
  if (route.page === 'model') return 1;
  if (route.page === 'store' && route.repo) return 2;
  if (route.page === 'store') return 1;
  return 0;
}

/** A CSS-safe view-transition-name so a list item can morph into its detail page title. */
export const morphName = (kind: 'model' | 'repo', id: string) =>
  `${kind}-${id.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '')}`;

export function navIdForRoute(route: Route): NavId {
  if (route.page === 'store' || route.page === 'model') return 'models';
  return route.page;
}

export function routeTitle(route: Route): string {
  if (route.page === 'home') return 'Home';
  if (route.page === 'requests') return 'Requests';
  if (route.page === 'settings') return 'API Settings';
  if (route.page === 'post-training') return 'Post Training';
  if (route.page === 'model') return route.id;
  if (route.page === 'store') return 'Get models';
  if (route.page === 'generate') return 'Generate';
  return NAV_ITEMS.find(item => item.id === route.page)?.label ?? 'InferDeck';
}
