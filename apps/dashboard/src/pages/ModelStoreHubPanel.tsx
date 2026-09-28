import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useFeedback } from '../components/Feedback';
import { ArrowDownTrayIcon, ArrowLeftIcon, HeartIcon, LockClosedIcon, MagnifyingGlassIcon } from '@heroicons/react/20/solid';
import {
  controlStoreDownload,
  getStoreActivity,
  inspectStoreModel,
  installStoreModel,
  removeStoreModel,
  searchStore,
  unregisterConfiguredModel,
  type InstalledStoreModel,
  type StoreDownload,
  type StoreFile,
  type StoreModel,
} from '../api';
import { Badge, Button, EmptyState, Notice, PageHeader, Segmented, Switch } from '../components/ui';
import { modelBelongsToSection, sectionLabel, type DashboardSection } from '../dashboardSections';
import { useGateway } from '../gateway';
import { morphName, storeHref } from '../routes';
import { stagger } from '../components/motion';
import type { Tone } from '../types';
import { usePolling } from '../usePolling';
import { formatBytes, formatTokenCount, timeAgo } from '../utils';
import { ModelStoreDownloadsView } from './ModelStoreDownloadsView';
import { ModelStoreInstalledView } from './ModelStoreInstalledView';
import {
  ArtifactFit,
  defaultStoreModelName,
  estimateRepositoryVramMb,
  parameterLabel,
  storeInputClass,
  storeScope,
  storeSearchPlaceholder,
  type ServerSortKey,
  type StoreTab,
} from './modelStoreUi';

export { defaultStoreModelName } from './modelStoreUi';

const repoName = (id: string) => id.split('/').pop() || id;
const repoOwner = (id: string) => (id.includes('/') ? id.split('/')[0] : 'Hugging Face');

type ShelfId = 'trending' | 'popular' | 'coding' | 'vision' | 'small' | 'recent';
interface Shelf {
  id: ShelfId;
  title: string;
  blurb: string;
  pick: (models: StoreModel[]) => StoreModel[];
}

const byDownloads = (models: StoreModel[]) => [...models].sort((left, right) => right.downloads - left.downloads);
const byRecent = (models: StoreModel[]) => [...models].sort((left, right) =>
  Date.parse(right.lastModified || '') - Date.parse(left.lastModified || ''));

const SHELVES: Record<ShelfId, Shelf> = {
  trending: { id: 'trending', title: 'Trending compatible models', blurb: 'What people are downloading right now', pick: models => models },
  popular: { id: 'popular', title: 'Most downloaded', blurb: 'Proven favourites', pick: byDownloads },
  coding: { id: 'coding', title: 'For coding', blurb: 'Tuned for code and agent tools', pick: models => models.filter(model => /cod(e|er|ing)|devstral|swe|starcoder/i.test(model.id)) },
  vision: { id: 'vision', title: 'Understands images', blurb: 'Read screenshots, photos and documents', pick: models => models.filter(model => model.hasVision) },
  small: { id: 'small', title: 'Small and fast', blurb: 'Leave room for other models', pick: models => models.filter(model => estimateRepositoryVramMb(model.id) <= 8 * 1024) },
  recent: { id: 'recent', title: 'Recently updated', blurb: 'New releases and fresh quantisations', pick: byRecent },
};

const SECTION_SHELVES: Record<DashboardSection, ShelfId[]> = {
  llm: ['trending', 'coding', 'vision', 'small', 'popular', 'recent'],
  dictation: ['trending', 'popular', 'recent'],
  image: ['trending', 'popular', 'recent'],
  music: ['trending', 'popular', 'recent'],
  video: ['trending', 'popular', 'recent'],
};

export function recommendFile(files: StoreFile[], section: DashboardSection, vramTotalMb: number): StoreFile | undefined {
  const usable = files.filter(file => file.compatible && !(bundleRuntime(file) && file.artifactCount === undefined));
  if (!usable.length) return undefined;
  if (section !== 'llm' || !vramTotalMb) return usable[0];
  const bySize = [...usable].sort((left, right) => right.estimatedVramMb - left.estimatedVramMb);
  return bySize.find(file => file.estimatedVramMb <= vramTotalMb * 0.65) ?? bySize[bySize.length - 1];
}

function bundleRuntime(file: StoreFile): boolean {
  return file.runtime === 'sherpa_onnx' || file.runtime === 'ace_step_cpp' || file.runtime === 'ltx_video_cpp';
}

function fitFor(estimateMb: number, vramTotalMb: number): { label: string; tone: Tone } | null {
  if (!vramTotalMb || !estimateMb) return null;
  const share = estimateMb / vramTotalMb;
  if (share <= 0.65) return { label: 'Fits', tone: 'good' };
  if (share <= 0.85) return { label: 'Tight fit', tone: 'warn' };
  return { label: 'Too large', tone: 'critical' };
}

const toneDot: Record<Tone, string> = {
  good: 'bg-success-green',
  warn: 'bg-warning-amber',
  critical: 'bg-danger-rose',
  info: 'bg-queue-blue',
  violet: 'bg-infer-violet',
  idle: 'bg-line-strong',
};

export const ModelStoreHubPanel: React.FC<{ section: DashboardSection; repo?: string }> = ({ section, repo }) => {
  const { status } = useGateway();
  const { confirm, toast } = useFeedback();
  const initial = storeScope[section];
  const [activeTab, setActiveTab] = useState<StoreTab>('discover');
  const [query, setQuery] = useState('');
  const [submittedQuery, setSubmittedQuery] = useState('');
  const [shelf, setShelf] = useState<ShelfId | null>(null);
  const [runtime, setRuntime] = useState(initial.runtime);
  const [modality, setModality] = useState(initial.modality);
  const [includeGated, setIncludeGated] = useState(false);
  const [fitsOnly, setFitsOnly] = useState(true);
  const [results, setResults] = useState<StoreModel[]>([]);
  const [downloads, setDownloads] = useState<StoreDownload[]>([]);
  const [installed, setInstalled] = useState<Record<string, InstalledStoreModel>>({});
  const [library, setLibrary] = useState<InstalledStoreModel[]>([]);
  const [searchBusy, setSearchBusy] = useState(false);
  const [searchError, setSearchError] = useState('');
  const [error, setError] = useState('');
  const [serverSort, setServerSort] = useState<{ key: ServerSortKey; direction: 'asc' | 'desc' }>({ key: 'name', direction: 'asc' });
  const searchRequest = useRef(0);

  const receiveActivity = useCallback((activity: Awaited<ReturnType<typeof getStoreActivity>>) => {
    setDownloads(activity.downloads);
    setInstalled(activity.installed);
    setLibrary(Array.isArray(activity.library) ? activity.library : []);
  }, []);
  const activityFailed = useCallback(() => {}, []);
  const refresh = usePolling(getStoreActivity, receiveActivity, activityFailed, 1500);

  const executeSearch = useCallback(async (nextQuery: string, nextRuntime: string, nextModality: string, nextIncludeGated: boolean) => {
    const request = ++searchRequest.current;
    setSearchBusy(true);
    setSearchError('');
    try {
      const nextResults = await searchStore(nextQuery.trim(), nextRuntime, nextModality, 60, 'trending', nextIncludeGated);
      if (request === searchRequest.current) setResults(nextResults);
    } catch (reason) {
      if (request === searchRequest.current) {
        setResults([]);
        setSearchError(reason instanceof Error ? reason.message : String(reason));
      }
    } finally {
      if (request === searchRequest.current) setSearchBusy(false);
    }
  }, []);

  useEffect(() => {
    const scope = storeScope[section];
    setActiveTab('discover');
    setQuery('');
    setSubmittedQuery('');
    setShelf(null);
    setRuntime(scope.runtime);
    setModality(scope.modality);
    setIncludeGated(false);
    void executeSearch('', scope.runtime, scope.modality, false);
  }, [section, executeSearch]);

  const search = (nextQuery = query, nextRuntime = runtime, nextModality = modality, nextGated = includeGated) => {
    setSubmittedQuery(nextQuery.trim());
    setShelf(null);
    if (repo) window.location.hash = storeHref(section);
    void executeSearch(nextQuery, nextRuntime, nextModality, nextGated);
  };

  const control = async (id: number, action: 'cancel' | 'resume') => {
    setError('');
    try {
      await controlStoreDownload(id, action);
      await refresh();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    }
  };

  const retire = async (model: string, action: 'archive' | 'remove') => {
    const ok = await confirm(action === 'archive'
      ? { title: `Archive ${model}?`, detail: 'It moves to the archive folder and is removed from InferDeck. You can restore it later.', confirmLabel: 'Archive' }
      : { title: `Delete ${model}?`, detail: 'The downloaded files are deleted permanently. This cannot be undone.', confirmLabel: 'Delete', destructive: true });
    if (!ok) return;
    setError('');
    try {
      await removeStoreModel(model, action);
      await refresh();
      toast(action === 'archive' ? `${model} archived` : `${model} deleted`);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    }
  };

  const unregister = async (model: string) => {
    if (!(await confirm({ title: `Remove ${model} from InferDeck?`, detail: "Its files stay on disk. You can add it back later.", confirmLabel: "Remove", destructive: true }))) return;
    setError('');
    try {
      await unregisterConfiguredModel(model);
      await refresh();
      toast(`${model} removed`);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    }
  };

  const scopedLibrary = useMemo(() => {
    const entries = library.length
      ? library
      : Object.entries(installed).map(([name, entry]) => ({ ...entry, id: `managed:${name}`, name, configured: true, managed: true }));
    return entries.filter(entry => modelBelongsToSection(entry, section));
  }, [library, installed, section]);
  const scopedDownloads = useMemo(
    () => downloads.filter(download => modelBelongsToSection(download, section)),
    [downloads, section],
  );
  const activeDownloads = scopedDownloads.filter(item => item.state === 'downloading' || item.state === 'queued').length;
  const installedRepos = useMemo(() => {
    const repos = new Set<string>();
    for (const entry of [...scopedLibrary, ...scopedDownloads] as Array<{ repo?: string; name?: string }>) {
      if (entry.repo) repos.add(entry.repo.toLowerCase());
    }
    return repos;
  }, [scopedLibrary, scopedDownloads]);
  const gpu = (status?.hardware?.gpu ?? {}) as Record<string, unknown>;
  const vramTotalMb = Number(gpu.vramTotal ?? 0) / (1024 * 1024);
  const catalogue = useMemo(
    () => results.filter(model => section !== 'llm' || !fitsOnly || !vramTotalMb || estimateRepositoryVramMb(model.id) <= vramTotalMb * 0.85),
    [results, section, fitsOnly, vramTotalMb],
  );
  const label = sectionLabel(section);
  const cardProps = { section, vramTotalMb, installedRepos };

  return (
    <div className="space-y-6">
      {!repo && <PageHeader
        title={`Get ${label} models`}
        subtitle={section !== 'dictation' && vramTotalMb
          ? `Browse models from Hugging Face that run on this server's ${Math.round(vramTotalMb / 1024)} GB GPU.`
          : `Browse ${label.toLowerCase()} models from Hugging Face that run inside InferDeck.`}
        actions={<a href={`#models/${section}`} className="inline-flex min-h-10 items-center gap-1.5 rounded-md border border-line-strong bg-panel-slate shadow-card px-3 text-sm font-medium text-text-primary hover:bg-elevated-slate sm:min-h-8">Your models</a>}
      />}

      {!repo && <Segmented
        aria-label="Model Store views"
        value={activeTab}
        onChange={tab => setActiveTab(tab as StoreTab)}
        items={[
          { id: 'discover', label: 'Discover' },
          { id: 'downloads', label: activeDownloads ? `Downloads (${activeDownloads} active)` : 'Downloads', count: scopedDownloads.length },
          { id: 'installed', label: 'Installed', count: scopedLibrary.length },
        ]}
      />}

      {error && <Notice tone="critical" role="alert">{error}</Notice>}

      {activeTab === 'discover' && (repo ? (
        <StoreProduct
          key={repo}
          repo={repo}
          section={section}
          model={results.find(entry => entry.id === repo)}
          vramTotalMb={vramTotalMb}
          related={catalogue.filter(entry => entry.id !== repo && repoOwner(entry.id) === repoOwner(repo)).slice(0, 4)}
          cardProps={cardProps}
          onInstalled={name => { void refresh(); toast('Download started', { detail: `${name} appears in your models when it finishes.` }); setActiveTab('downloads'); }}
        />
      ) : (
        <>
          <section aria-label="Compatibility" className="space-y-3">
            <form className="flex gap-2" onSubmit={event => { event.preventDefault(); search(); }}>
              <label className="relative min-w-0 flex-1" htmlFor={`${section}-model-search`}>
                <span className="sr-only">Search by model, creator, or task</span>
                <MagnifyingGlassIcon className="pointer-events-none absolute left-3 top-1/2 h-4 w-4 -translate-y-1/2 text-text-muted" aria-hidden="true" />
                <input
                  id={`${section}-model-search`}
                  className="h-11 w-full pl-9 pr-3 text-base"
                  placeholder={storeSearchPlaceholder[section]}
                  value={query}
                  onChange={event => setQuery(event.target.value)}
                />
              </label>
              <Button type="submit" tone="blue" disabled={searchBusy} className="sm:min-h-11 sm:px-5">Search</Button>
            </form>
            <div className="flex flex-wrap items-center gap-x-5 gap-y-2 text-sm text-text-secondary">
              {section === 'dictation' && (
                <div className="inline-flex divide-x divide-line-strong overflow-hidden rounded-md border border-line-strong" role="group" aria-label="Speech service">
                  {[
                    { id: 'audio_transcription', label: 'Speech to text', runtime: 'whisper_cpp' },
                    { id: 'audio_speech', label: 'Text to speech', runtime: 'sherpa_onnx' },
                  ].map(option => (
                    <button
                      key={option.id}
                      type="button"
                      aria-pressed={modality === option.id}
                      onClick={() => { setModality(option.id); setRuntime(option.runtime); search(query, option.runtime, option.id); }}
                      className={`min-h-9 px-3 text-sm sm:min-h-7 ${modality === option.id ? 'bg-elevated-slate text-text-primary' : 'text-text-muted hover:text-text-primary'}`}
                    >
                      {option.label}
                    </button>
                  ))}
                </div>
              )}
              {section === 'llm' && vramTotalMb > 0 && (
                <span className="flex items-center gap-2">
                  <Switch checked={fitsOnly} label="Only show models that fit this GPU" onChange={setFitsOnly} />
                  Only models that fit this GPU
                </span>
              )}
              <details className="group">
                <summary className="cursor-pointer list-none text-xs text-text-muted hover:text-text-secondary">
                  <span className="inline-block transition-transform group-open:rotate-90" aria-hidden="true">›</span> Compatibility
                </summary>
                <div className="mt-2 flex flex-wrap items-center gap-x-5 gap-y-2">
                  <span className="text-text-muted">Runtime <span className="text-text-primary">{section === 'dictation' ? (runtime === 'whisper_cpp' ? 'whisper.cpp' : 'sherpa-onnx') : storeScope[section].runtimeLabel}</span></span>
                  <span className="flex items-center gap-2 text-text-muted">
                    <Switch checked label="Local runtime only" onChange={() => {}} disabled />
                    Local runtime only
                  </span>
                  <span className="flex items-center gap-2 text-text-muted">
                    <Switch
                      checked={includeGated}
                      label="Include gated"
                      onChange={next => { setIncludeGated(next); search(query, runtime, modality, next); }}
                    />
                    Include gated
                  </span>
                </div>
              </details>
            </div>
          </section>

          {searchError && (
            <Notice tone="critical" role="alert">
              <span>{searchError}</span>{' '}
              <button type="button" className="font-medium text-text-primary underline" onClick={() => search()}>Retry search</button>
            </Notice>
          )}

          {submittedQuery || shelf ? (
            <section aria-live="polite">
              <div className="flex flex-wrap items-baseline justify-between gap-3 pb-3">
                <div>
                  <button type="button" onClick={() => { setShelf(null); if (submittedQuery) { setQuery(''); search(''); } }} className="mb-2 inline-flex items-center gap-1 text-sm text-text-muted hover:text-text-primary">
                    <ArrowLeftIcon className="h-4 w-4" aria-hidden="true" /> All {label} models
                  </button>
                  <h2 className="text-lg font-semibold">{submittedQuery ? `Results for “${submittedQuery}”` : SHELVES[shelf!].title}</h2>
                </div>
                <span className="text-xs text-text-muted">{searchBusy ? 'Searching...' : `${(shelf ? SHELVES[shelf].pick(catalogue) : catalogue).length} models`}</span>
              </div>
              <CardGrid models={shelf ? SHELVES[shelf].pick(catalogue) : catalogue} busy={searchBusy} morph {...cardProps} empty={submittedQuery ? 'Try a model family, creator or task.' : 'Nothing here yet.'} />
            </section>
          ) : (
            <div className="space-y-10" aria-live="polite">
              {SECTION_SHELVES[section].map((id, index) => {
                const picked = SHELVES[id].pick(catalogue);
                if (index > 0 && !searchBusy && picked.length === 0) return null;
                const limit = index === 0 ? 8 : 4;
                return (
                  <section key={id}>
                    <div className="flex items-end justify-between gap-3 pb-3">
                      <div>
                        <h2 className="text-base font-semibold">{SHELVES[id].title}</h2>
                        <p className="text-sm text-text-muted">{SHELVES[id].blurb}</p>
                      </div>
                      {picked.length > limit && (
                        <button type="button" onClick={() => setShelf(id)} className="shrink-0 text-sm text-queue-blue hover:underline">See all {picked.length}</button>
                      )}
                    </div>
                    <CardGrid
                      models={picked.slice(0, limit)}
                      busy={searchBusy}
                      skeletons={limit}
                      morph={index === 0}
                      {...cardProps}
                      empty={results.length ? 'No model here fits this GPU. Turn off “Only models that fit this GPU” to see them all.' : 'No compatible models found.'}
                    />
                  </section>
                );
              })}
            </div>
          )}
        </>
      ))}

      {activeTab === 'downloads' && (
        <ModelStoreDownloadsView downloads={scopedDownloads} onControl={(id, action) => { void control(id, action); }} />
      )}

      {activeTab === 'installed' && (
        <ModelStoreInstalledView
          section={section}
          entries={scopedLibrary}
          sort={serverSort}
          onSort={setServerSort}
          onRetire={(model, action) => { void retire(model, action); }}
          onUnregister={model => { void unregister(model); }}
        />
      )}
    </div>
  );
};

type CardProps = { section: DashboardSection; vramTotalMb: number; installedRepos: Set<string> };

const CardGrid: React.FC<CardProps & { models: StoreModel[]; busy: boolean; empty: string; skeletons?: number; morph?: boolean }> = ({ models, busy, empty, skeletons = 8, morph, ...cardProps }) => {
  if (busy && !models.length) {
    return (
      <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-4" role="status">
        <span className="sr-only">Loading models...</span>
        {Array.from({ length: skeletons }, (_, index) => <div key={index} className="skeleton h-[148px] rounded-lg" />)}
      </div>
    );
  }
  if (!models.length) return <EmptyState title="No models to show" detail={empty} />;
  return (
    <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-4">
      {models.map((model, index) => <StoreCard key={model.id} model={model} morph={morph} index={index} {...cardProps} />)}
    </div>
  );
};

const StoreCard: React.FC<CardProps & { model: StoreModel; morph?: boolean; index: number }> = ({ model, section, vramTotalMb, installedRepos, morph, index }) => {
  const estimate = estimateRepositoryVramMb(model.id);
  const fit = section === 'llm' ? fitFor(estimate, vramTotalMb) : null;
  const params = parameterLabel(model.id);
  const tags = [
    params,
    section === 'llm' ? `~${Math.round(estimate / 1024)} GB` : '',
    model.hasVision ? 'Vision' : '',
    model.format ? model.format.toUpperCase() : '',
  ].filter(Boolean);
  const installed = installedRepos.has(model.id.toLowerCase());
  return (
    <a
      href={storeHref(section, model.id)}
      style={stagger(index)}
      className="group flex min-h-[148px] animate-item-in flex-col rounded-lg border border-border-slate bg-panel-slate p-3.5 shadow-card hover:border-line-strong hover:shadow-deck"
    >
      <div className="flex items-start justify-between gap-2">
        <p className="line-clamp-2 break-all font-mono text-sm font-medium leading-5 text-text-primary" title={model.id} style={morph ? { viewTransitionName: morphName('repo', model.id) } : undefined}>{repoName(model.id)}</p>
        {model.gated && <LockClosedIcon className="mt-0.5 h-3.5 w-3.5 shrink-0 text-warning-amber" aria-label="Gated" />}
      </div>
      <p className="mt-0.5 truncate text-xs text-text-muted">{repoOwner(model.id)}</p>
      {tags.length > 0 && (
        <div className="mt-2.5 flex flex-wrap gap-1">
          {tags.map(tag => <span key={tag} className="rounded bg-elevated-slate px-1.5 py-0.5 text-2xs text-text-secondary">{tag}</span>)}
        </div>
      )}
      <div className="mt-auto flex items-center justify-between gap-2 pt-3 text-xs text-text-muted">
        <span className="tabular inline-flex items-center gap-2.5">
          <span className="inline-flex items-center gap-0.5"><ArrowDownTrayIcon className="h-3 w-3" aria-hidden="true" />{formatTokenCount(model.downloads)}</span>
          <span className="inline-flex items-center gap-0.5"><HeartIcon className="h-3 w-3" aria-hidden="true" />{formatTokenCount(model.likes)}</span>
        </span>
        {installed ? (
          <span className="text-success-green">Installed</span>
        ) : fit ? (
          <span className="inline-flex items-center gap-1.5 text-text-secondary"><span className={`h-1.5 w-1.5 rounded-full ${toneDot[fit.tone]}`} aria-hidden="true" />{fit.label}</span>
        ) : model.lastModified ? (
          <span>{timeAgo(Date.parse(model.lastModified))}</span>
        ) : null}
      </div>
    </a>
  );
};

const StoreProduct: React.FC<{
  repo: string;
  section: DashboardSection;
  model?: StoreModel;
  vramTotalMb: number;
  related: StoreModel[];
  cardProps: CardProps;
  onInstalled: (name: string) => void;
}> = ({ repo, section, model, vramTotalMb, related, cardProps, onInstalled }) => {
  const [files, setFiles] = useState<StoreFile[]>([]);
  const [busy, setBusy] = useState(true);
  const [error, setError] = useState('');
  const [selected, setSelected] = useState<StoreFile | null>(null);
  const [name, setName] = useState('');
  const [installing, setInstalling] = useState(false);

  useEffect(() => {
    let active = true;
    setBusy(true);
    setError('');
    inspectStoreModel(repo).then(next => {
      if (!active) return;
      setFiles(next.filter(file => modelBelongsToSection(file, section)));
    }).catch(reason => {
      if (active) setError(reason instanceof Error ? reason.message : String(reason));
    }).finally(() => {
      if (active) setBusy(false);
    });
    return () => { active = false; };
  }, [repo, section]);

  const reviewFiles = useMemo(() => {
    const bundles = new Set(files.filter(file => file.format === 'bundle').map(file => file.runtime));
    return files
      .filter(file => !bundles.has(file.runtime) || file.format === 'bundle')
      .sort((left, right) => right.size - left.size);
  }, [files]);
  const recommended = useMemo(() => recommendFile(reviewFiles, section, vramTotalMb), [reviewFiles, section, vramTotalMb]);

  useEffect(() => {
    if (recommended && !selected) choose(recommended);
  // The recommendation becomes the default choice once per repository.
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [recommended]);

  const choose = (file: StoreFile) => {
    setSelected(file);
    setName(defaultStoreModelName(file));
  };

  const install = async (event: React.FormEvent) => {
    event.preventDefault();
    if (!selected || !name.trim()) return;
    setInstalling(true);
    setError('');
    try {
      await installStoreModel(selected, name.trim());
      onInstalled(name.trim());
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setInstalling(false);
    }
  };

  const params = parameterLabel(repo);
  const estimate = estimateRepositoryVramMb(repo);
  const kind = section === 'llm' ? 'language model' : `${sectionLabel(section).toLowerCase()} model`;
  const summary = `${params ? `A ${params} parameter ${kind}` : `A ${kind}`} from ${repoOwner(repo)}${model?.hasVision ? ' that can also read images' : ''}${model?.format ? `, packaged as ${model.format.toUpperCase()}` : ''}.`;
  const fit = section === 'llm' ? fitFor(selected?.estimatedVramMb || estimate, vramTotalMb) : null;

  return (
    <div className="space-y-10">
      <a href={storeHref(section)} className="inline-flex items-center gap-1 text-sm text-text-muted hover:text-text-primary">
        <ArrowLeftIcon className="h-4 w-4" aria-hidden="true" /> All {sectionLabel(section)} models
      </a>

      <div className="grid items-start gap-8 lg:grid-cols-[minmax(0,1fr)_340px]">
        <div className="min-w-0 space-y-8">
          <header>
            <h1 className="break-all font-mono text-2xl font-medium text-text-primary" style={{ viewTransitionName: morphName('repo', repo) }}>{repoName(repo)}</h1>
            <p className="mt-1 text-sm text-text-muted">by {repoOwner(repo)}</p>
            <p className="mt-4 max-w-[65ch] text-base text-text-secondary">{summary}</p>
            <div className="mt-4 flex flex-wrap gap-1.5">
              {model?.hasVision && <Badge label="Vision" tone="idle" />}
              {model?.gated && <Badge label="Gated: accept the licence on Hugging Face first" tone="warn" />}
              {section === 'llm' && <Badge label={`About ${Math.round(estimate / 1024)} GB`} tone="idle" />}
            </div>
            <dl className="mt-5 flex flex-wrap gap-x-8 gap-y-3 text-sm">
              {model && <Fact label="Downloads" value={formatTokenCount(model.downloads)} />}
              {model && <Fact label="Likes" value={formatTokenCount(model.likes)} />}
              {model?.lastModified && <Fact label="Updated" value={timeAgo(Date.parse(model.lastModified))} />}
              <Fact label="Licence" value={model?.license || 'See repository'} />
              <Fact label="Runs on" value={storeScope[section].runtimeLabel} />
            </dl>
          </header>

          <section>
            <h2 className="text-base font-semibold">Choose a verified variant</h2>
            <p className="mt-0.5 text-sm text-text-muted">Smaller files load faster and leave room for other models. Larger files keep more quality.</p>
            {busy ? (
              <div className="mt-3 space-y-1" role="status">
                <span className="sr-only">Checking repository files...</span>
                {[0, 1, 2, 3].map(index => <div key={index} className="skeleton h-11 rounded-md" />)}
              </div>
            ) : reviewFiles.length === 0 ? (
              <p className="mt-3 text-sm text-text-muted">This repository has no complete file that {storeScope[section].runtimeLabel} can run.</p>
            ) : (
              <div className="mt-3 overflow-x-auto rounded-lg border border-border-slate bg-panel-slate shadow-card" role="radiogroup" aria-label="Verified variants">
                <table className="w-full min-w-[520px] border-collapse text-left text-sm">
                  <thead>
                    <tr className="border-b border-border-slate text-xs text-text-muted">
                      <th className="w-8 px-3 py-2"><span className="sr-only">Selected</span></th>
                      <th className="px-3 py-2 font-medium">File</th>
                      <th className="px-3 py-2 text-right font-medium">Size</th>
                      <th className="px-3 py-2 font-medium">Fit</th>
                    </tr>
                  </thead>
                  <tbody>
                    {reviewFiles.map(file => {
                      const requiresBundle = bundleRuntime(file) && file.artifactCount === undefined;
                      const disabled = !file.compatible || requiresBundle;
                      const active = selected?.name === file.name;
                      return (
                        <tr
                          key={file.name}
                          role="radio"
                          aria-checked={active}
                          aria-disabled={disabled}
                          tabIndex={disabled ? -1 : 0}
                          onClick={() => { if (!disabled) choose(file); }}
                          onKeyDown={event => { if (!disabled && (event.key === 'Enter' || event.key === ' ')) { event.preventDefault(); choose(file); } }}
                          className={`border-b border-border-slate last:border-b-0 ${disabled ? 'opacity-45' : 'cursor-pointer hover:bg-panel-slate'} ${active ? 'bg-elevated-slate' : ''}`}
                        >
                          <td className="px-3 py-2"><span className={`block h-3.5 w-3.5 rounded-full border ${active ? 'border-[4px] border-queue-blue' : 'border-line-strong'}`} aria-hidden="true" /></td>
                          <td className="px-3 py-2">
                            <span className="block break-all font-mono text-xs text-text-primary">{file.variant || file.name}</span>
                            <span className="text-xs text-text-muted">
                              {file.quantization}
                              {file.name === recommended?.name ? ', recommended' : ''}
                              {requiresBundle ? ', included in bundle' : !file.compatible ? ', unverified' : ''}
                            </span>
                          </td>
                          <td className="tabular whitespace-nowrap px-3 py-2 text-right text-text-secondary">{formatBytes(file.size)}</td>
                          <td className="px-3 py-2"><ArtifactFit section={section} estimatedVramMb={file.estimatedVramMb} vramTotalMb={vramTotalMb} /></td>
                        </tr>
                      );
                    })}
                  </tbody>
                </table>
              </div>
            )}
          </section>
        </div>

        <aside className="rounded-md border border-border-slate bg-panel-slate p-4 lg:sticky lg:top-4" aria-label="Install">
          {error && <Notice tone="critical" role="alert" className="mb-3">{error}</Notice>}
          {selected ? (
            <form onSubmit={event => { void install(event); }}>
              <p className="text-xs text-text-muted">{selected.name === recommended?.name ? 'Recommended for this server' : 'Your choice'}</p>
              <p className="mt-1 break-all font-mono text-sm text-text-primary">{selected.variant || selected.name}</p>
              <p className="tabular mt-1 text-sm text-text-secondary">{formatBytes(selected.size)} download</p>
              {fit && (
                <p className="mt-2 flex items-center gap-2 text-sm text-text-secondary">
                  <span className={`h-2 w-2 rounded-full ${toneDot[fit.tone]}`} aria-hidden="true" />
                  {fit.tone === 'good' ? 'Fits this GPU with room to spare' : fit.tone === 'warn' ? 'Fits, but little room for long context' : 'Probably too large for this GPU'}
                </p>
              )}
              <label className="mt-4 block text-xs text-text-muted" htmlFor={`${section}-install-name`}>Name in InferDeck</label>
              <input id={`${section}-install-name`} className="mt-1 h-9 w-full px-2.5 font-mono text-sm" value={name} onChange={event => setName(event.target.value)} />
              <Button type="submit" tone="blue" loading={installing} disabled={!name.trim()} className="mt-4 w-full sm:min-h-10">
                {installing ? 'Starting download' : 'Install model'}
              </Button>
              <p className="mt-3 text-xs text-text-muted">Downloads run in the background. InferDeck checks the file before adding the model.</p>
            </form>
          ) : (
            <p className="text-sm text-text-muted">{busy ? 'Checking which files this server can run...' : 'No file here can be installed yet.'}</p>
          )}
          <a href={`https://huggingface.co/${repo}`} target="_blank" rel="noreferrer" className="mt-4 block border-t border-border-slate pt-3 text-sm text-queue-blue hover:underline">View on Hugging Face</a>
        </aside>
      </div>

      {related.length > 0 && (
        <section>
          <h2 className="pb-3 text-base font-semibold">More from {repoOwner(repo)}</h2>
          <CardGrid models={related} busy={false} empty="" {...cardProps} />
        </section>
      )}
    </div>
  );
};

const Fact: React.FC<{ label: string; value: string }> = ({ label, value }) => (
  <div>
    <dt className="text-xs text-text-muted">{label}</dt>
    <dd className="tabular text-text-primary">{value}</dd>
  </div>
);
