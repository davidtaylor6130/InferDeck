import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
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
import { Badge, Button, EmptyState, Panel, SectionTitle } from '../components/ui';
import { modelBelongsToSection, sectionLabel, type DashboardSection } from '../dashboardSections';
import { useGateway } from '../gateway';
import { usePolling } from '../usePolling';
import { formatBytes, formatDate, formatTokenCount } from '../utils';
import { ModelStoreDownloadsView } from './ModelStoreDownloadsView';
import { ModelStoreInstalledView } from './ModelStoreInstalledView';
import {
  ArtifactFit,
  defaultStoreModelName,
  estimateRepositoryVramMb,
  storeInputClass,
  storeScope,
  storeSearchPlaceholder,
  type CatalogueSort,
  type ServerSortKey,
  type StoreTab,
} from './modelStoreUi';

export { defaultStoreModelName } from './modelStoreUi';

export const ModelStoreHubPanel: React.FC<{ section: DashboardSection }> = ({ section }) => {
  const { status } = useGateway();
  const initial = storeScope[section];
  const [activeTab, setActiveTab] = useState<StoreTab>('discover');
  const [query, setQuery] = useState('');
  const [runtime, setRuntime] = useState(initial.runtime);
  const [modality, setModality] = useState(initial.modality);
  const [catalogSort, setCatalogSort] = useState<CatalogueSort>('trending');
  const [includeGated, setIncludeGated] = useState(false);
  const [vramCapacityGb, setVramCapacityGb] = useState('server');
  const [results, setResults] = useState<StoreModel[]>([]);
  const [files, setFiles] = useState<StoreFile[]>([]);
  const [selectedRepo, setSelectedRepo] = useState('');
  const [selectedFile, setSelectedFile] = useState<StoreFile | null>(null);
  const [modelName, setModelName] = useState('');
  const [downloads, setDownloads] = useState<StoreDownload[]>([]);
  const [installed, setInstalled] = useState<Record<string, InstalledStoreModel>>({});
  const [library, setLibrary] = useState<InstalledStoreModel[]>([]);
  const [searchBusy, setSearchBusy] = useState(false);
  const [inspectBusy, setInspectBusy] = useState(false);
  const [installBusy, setInstallBusy] = useState(false);
  const [searchError, setSearchError] = useState('');
  const [error, setError] = useState('');
  const [serverSort, setServerSort] = useState<{
    key: ServerSortKey;
    direction: 'asc' | 'desc';
  }>({ key: 'name', direction: 'asc' });
  const searchRequest = useRef(0);
  const inspectRequest = useRef(0);

  const receiveActivity = useCallback((activity: Awaited<ReturnType<typeof getStoreActivity>>) => {
    setDownloads(activity.downloads);
    setInstalled(activity.installed);
    setLibrary(Array.isArray(activity.library) ? activity.library : []);
  }, []);
  const activityFailed = useCallback(() => {}, []);
  const refresh = usePolling(getStoreActivity, receiveActivity, activityFailed, 1500);

  const executeSearch = useCallback(async (
    nextQuery: string,
    nextRuntime: string,
    nextModality: string,
    nextSort: CatalogueSort,
    nextIncludeGated: boolean,
  ) => {
    const request = ++searchRequest.current;
    setSearchBusy(true);
    setSearchError('');
    setError('');
    setFiles([]);
    setSelectedRepo('');
    setSelectedFile(null);
    try {
      const nextResults = await searchStore(
        nextQuery.trim(), nextRuntime, nextModality, 50,
        nextSort, nextIncludeGated,
      );
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
    ++inspectRequest.current;
    setActiveTab('discover');
    setQuery('');
    setRuntime(scope.runtime);
    setModality(scope.modality);
    setCatalogSort('trending');
    setIncludeGated(false);
    setVramCapacityGb('server');
    void executeSearch('', scope.runtime, scope.modality, 'trending', false);
  }, [section, executeSearch]);

  const inspect = async (repo: string) => {
    const request = ++inspectRequest.current;
    setInspectBusy(true);
    setError('');
    setSelectedRepo(repo);
    setSelectedFile(null);
    setFiles([]);
    try {
      const nextFiles = await inspectStoreModel(repo);
      if (request === inspectRequest.current) {
        setFiles(nextFiles.filter(file => modelBelongsToSection(file, section)));
      }
    } catch (reason) {
      if (request === inspectRequest.current) {
        setError(reason instanceof Error ? reason.message : String(reason));
      }
    } finally {
      if (request === inspectRequest.current) setInspectBusy(false);
    }
  };

  const chooseFile = (file: StoreFile) => {
    setSelectedFile(file);
    setModelName(defaultStoreModelName(file));
    setError('');
  };

  const install = async (event: React.FormEvent) => {
    event.preventDefault();
    if (!selectedFile || !modelName.trim()) return;
    setInstallBusy(true);
    setError('');
    try {
      await installStoreModel(selectedFile, modelName.trim());
      setSelectedFile(null);
      setModelName('');
      await refresh();
      setActiveTab('downloads');
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setInstallBusy(false);
    }
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
    const message = action === 'archive'
      ? `Archive ${model}? It will be moved to the configured archive directory and unregistered.`
      : `Permanently delete ${model} and its store-managed artifact? This cannot be undone.`;
    if (!window.confirm(message)) return;
    setError('');
    try {
      await removeStoreModel(model, action);
      await refresh();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    }
  };

  const unregister = async (model: string) => {
    if (!window.confirm(
      `Remove ${model} from InferDeck? Its external model files will remain on disk.`,
    )) return;
    setError('');
    try {
      await unregisterConfiguredModel(model);
      await refresh();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    }
  };

  const scopedInstalled = useMemo(
    () => Object.entries(installed).filter(([, entry]) =>
      modelBelongsToSection(entry, section)),
    [installed, section],
  );
  const scopedLibrary = useMemo(() => {
    const entries = library.length
      ? library
      : scopedInstalled.map(([name, entry]) => ({
          ...entry,
          id: `managed:${name}`,
          name,
          configured: true,
          managed: true,
        }));
    return entries.filter(entry => modelBelongsToSection(entry, section));
  }, [library, scopedInstalled, section]);
  const scopedDownloads = useMemo(
    () => downloads.filter(download => modelBelongsToSection(download, section)),
    [downloads, section],
  );
  const gpu = (status?.hardware?.gpu ?? {}) as Record<string, unknown>;
  const vramTotalMb = Number(gpu.vramTotal ?? 0) / (1024 * 1024);
  const selectedCapacityMb = vramCapacityGb === 'server'
    ? vramTotalMb
    : Number(vramCapacityGb) * 1024;
  const visibleResults = useMemo(
    () => results.filter(model => section !== 'llm' || !selectedCapacityMb ||
      estimateRepositoryVramMb(model.id) <= selectedCapacityMb * 0.85),
    [results, section, selectedCapacityMb],
  );
  const selectedModel = results.find(model => model.id === selectedRepo);
  const reviewFiles = useMemo(() => {
    const bundleRuntimes = new Set(
      files.filter(file => file.format === 'bundle').map(file => file.runtime),
    );
    return files.filter(file =>
      !bundleRuntimes.has(file.runtime) || file.format === 'bundle');
  }, [files]);

  const runCurrentSearch = () => {
    void executeSearch(
      query, runtime, modality, catalogSort, includeGated,
    );
  };

  return (
    <div className="space-y-5">
      {error && (
        <p
          className="border-l-2 border-danger-rose bg-danger-rose/10 px-3 py-2 text-xs text-danger-rose"
          role="alert"
        >
          {error}
        </p>
      )}
      <Panel className="border-t-0 pt-0">
        <SectionTitle
          title={`${sectionLabel(section)} Model Store`}
          aside={section !== 'dictation' && vramTotalMb
            ? `${Math.round(vramTotalMb / 1024)} GB VRAM detected`
            : 'Hugging Face catalogue'}
        />
        <p className="mt-2 max-w-3xl text-sm text-text-secondary">
          Browse popular local models, inspect verified files, and install them without leaving InferDeck.
        </p>
        <div
          className="mt-4 flex gap-1 overflow-x-auto border-b border-border-slate"
          role="tablist"
          aria-label="Model Store views"
        >
          {([
            ['discover', 'Discover', visibleResults.length],
            ['downloads', 'Downloads', scopedDownloads.length],
            ['installed', 'Installed', scopedLibrary.length],
          ] as Array<[StoreTab, string, number]>).map(([tab, label, count]) => (
            <button
              key={tab}
              type="button"
              role="tab"
              aria-selected={activeTab === tab}
              className={`min-h-11 shrink-0 border-b-2 px-3 text-sm font-medium transition-colors ${
                activeTab === tab
                  ? 'border-queue-blue text-text-primary'
                  : 'border-transparent text-text-muted hover:text-text-secondary'
              }`}
              onClick={() => setActiveTab(tab)}
            >
              {label} <span className="ml-1 text-xs text-text-muted">{count}</span>
            </button>
          ))}
        </div>
      </Panel>

      {activeTab === 'discover' && (
        <section className="grid gap-5 xl:grid-cols-[minmax(190px,0.38fr)_minmax(0,1fr)_minmax(320px,0.72fr)]">
          <Panel className="xl:border-t-0 xl:pt-0">
            <SectionTitle title="Compatibility" aside="local inference" />
            <div className="mt-4 space-y-4">
              <label className="block text-xs text-text-muted">
                Runtime
                {section === 'dictation' ? (
                  <select
                    className={`${storeInputClass} mt-1 w-full`}
                    value={runtime}
                    onChange={event => {
                      const nextRuntime = event.target.value;
                      setRuntime(nextRuntime);
                      if (nextRuntime === 'whisper_cpp') {
                        setModality('audio_transcription');
                      }
                    }}
                  >
                    <option value="whisper_cpp">whisper.cpp</option>
                    <option value="sherpa_onnx">sherpa-onnx</option>
                  </select>
                ) : (
                  <span className="mt-1 block min-h-11 border-y border-white/10 py-3 text-sm text-text-secondary">
                    {storeScope[section].runtimeLabel}
                  </span>
                )}
              </label>
              {section === 'dictation' && (
                <label className="block text-xs text-text-muted">
                  Speech service
                  <select
                    className={`${storeInputClass} mt-1 w-full`}
                    value={modality}
                    onChange={event => {
                      const nextModality = event.target.value;
                      setModality(nextModality);
                      if (nextModality === 'audio_speech') {
                        setRuntime('sherpa_onnx');
                      }
                    }}
                  >
                    <option value="audio_transcription">Speech to text</option>
                    <option value="audio_speech">Text to speech</option>
                  </select>
                </label>
              )}
              {section === 'llm' && (
                <label className="block text-xs text-text-muted">
                  VRAM capacity
                  <select
                    className={`${storeInputClass} mt-1 w-full`}
                    value={vramCapacityGb}
                    onChange={event => setVramCapacityGb(event.target.value)}
                  >
                    <option value="server">Fits this server</option>
                    <option value="8">Up to 8 GB</option>
                    <option value="16">Up to 16 GB</option>
                    <option value="24">Up to 24 GB</option>
                    <option value="32">Up to 32 GB</option>
                    <option value="48">Up to 48 GB</option>
                    <option value="0">Any VRAM size</option>
                  </select>
                </label>
              )}
              <label className="block text-xs text-text-muted">
                Sort
                <select
                  className={`${storeInputClass} mt-1 w-full`}
                  value={catalogSort}
                  onChange={event =>
                    setCatalogSort(event.target.value as CatalogueSort)}
                >
                  <option value="trending">Trending</option>
                  <option value="downloads">Most downloaded</option>
                  <option value="likes">Most liked</option>
                  <option value="recent">Recently updated</option>
                </select>
              </label>
              <label className="flex min-h-11 items-center gap-2 text-xs text-text-secondary">
                <input type="checkbox" checked disabled />
                Local runtime only
              </label>
              <label className="flex min-h-11 items-center gap-2 text-xs text-text-secondary">
                <input
                  type="checkbox"
                  checked={includeGated}
                  onChange={event => setIncludeGated(event.target.checked)}
                />
                Include gated
              </label>
              <p className="text-xs text-text-muted">
                Only repositories with complete native InferDeck artifacts are shown.
              </p>
              <Button tone="blue" className="w-full" onClick={runCurrentSearch}>
                Apply filters
              </Button>
            </div>
          </Panel>

          <Panel className="xl:border-t-0 xl:pt-0">
            <SectionTitle
              title={query.trim() ? 'Compatible models' : 'Trending compatible models'}
              aside={`${visibleResults.length} found`}
            />
            <form
              className="mt-4"
              onSubmit={event => {
                event.preventDefault();
                runCurrentSearch();
              }}
            >
              <label className="text-xs text-text-muted" htmlFor={`${section}-model-search`}>
                Search by model, creator, or task
              </label>
              <div className="mt-2 flex flex-col gap-2 sm:flex-row">
                <input
                  id={`${section}-model-search`}
                  className={`${storeInputClass} min-w-0 flex-1`}
                  placeholder={storeSearchPlaceholder[section]}
                  value={query}
                  onChange={event => setQuery(event.target.value)}
                />
                <Button
                  type="submit"
                  tone="blue"
                  disabled={searchBusy}
                  className="sm:min-w-24"
                >
                  {searchBusy ? 'Searching...' : 'Search'}
                </Button>
              </div>
            </form>
            {searchError && (
              <div
                className="mt-3 flex flex-wrap items-center justify-between gap-2 border-l-2 border-danger-rose bg-danger-rose/10 px-3 py-2 text-xs text-danger-rose"
                role="alert"
              >
                <span>{searchError}</span>
                <Button tone="danger" onClick={runCurrentSearch}>Retry search</Button>
              </div>
            )}
            <div className="mt-4" aria-live="polite">
              {visibleResults.length === 0 ? (
                <EmptyState
                  title={searchBusy ? 'Checking compatible repositories...' : 'No matching models'}
                  detail={!searchBusy && results.length
                    ? 'The VRAM capacity filter excludes every compatible result.'
                    : 'Try a model family, creator, or task.'}
                />
              ) : (
                <div className="divide-y divide-white/10 border-y border-white/10">
                  {visibleResults.map(model => (
                    <button
                      key={model.id}
                      type="button"
                      aria-pressed={selectedRepo === model.id}
                      onClick={() => { void inspect(model.id); }}
                      className={`grid w-full gap-2 px-1 py-3 text-left transition-colors sm:grid-cols-[minmax(0,1fr)_auto] sm:items-center ${
                        selectedRepo === model.id
                          ? 'bg-white/[0.06]'
                          : 'hover:bg-white/[0.03]'
                      }`}
                    >
                      <span className="min-w-0">
                        <span className="block break-all font-mono text-sm text-text-primary">
                          {model.id}
                        </span>
                        <span className="mt-1 block text-xs text-text-muted">
                          {formatTokenCount(model.downloads)} downloads / {model.likes} likes
                          {model.lastModified
                            ? ` / updated ${formatDate(model.lastModified)}`
                            : ''}
                        </span>
                        <span className="mt-1 block text-xs text-text-secondary">
                          {model.format || 'local artifact'} / {model.compatibleArtifacts ?? 1} compatible
                          {model.license ? ` / ${model.license}` : ''}
                        </span>
                      </span>
                      <span className="flex flex-wrap items-center gap-2 sm:justify-end">
                        {model.hasVision && <Badge label="Vision" tone="violet" />}
                        {model.gated && <Badge label="Gated" tone="warn" />}
                        {(model.trendingScore ?? 0) > 0
                          ? <Badge label="Trending" tone="good" />
                          : model.recommended
                            ? <Badge label="Popular" tone="good" />
                            : null}
                        <span className="text-xs font-medium text-queue-blue">
                          View variants
                        </span>
                      </span>
                    </button>
                  ))}
                </div>
              )}
            </div>
          </Panel>

          <Panel className="xl:border-t-0 xl:pt-0">
            <SectionTitle
              title="Choose a verified variant"
              aside={selectedRepo || 'select a model'}
            />
            {!selectedRepo ? (
              <div className="mt-4">
                <EmptyState
                  title="Select a model"
                  detail="Compatible files and hardware fit appear here."
                />
              </div>
            ) : inspectBusy ? (
              <p
                className="mt-4 border-y border-dashed border-border-slate py-8 text-center text-sm text-text-muted"
                role="status"
              >
                Inspecting repository files...
              </p>
            ) : reviewFiles.length === 0 ? (
              <div className="mt-4">
                <EmptyState
                  title="No verified variants"
                  detail="This repository has no complete artifact supported by the selected runtime."
                />
              </div>
            ) : (
              <>
                <div className="mt-3 flex flex-wrap gap-2 text-xs text-text-muted">
                  <span>{selectedModel?.runtime}</span>
                  {selectedModel?.license && <span>{selectedModel.license}</span>}
                  {selectedModel?.hasVision && <Badge label="Vision capable" tone="violet" />}
                  <span>{reviewFiles.length} choice{reviewFiles.length === 1 ? '' : 's'}</span>
                </div>
                <div className="mt-3 divide-y divide-white/10 border-y border-white/10">
                  {reviewFiles.map(file => {
                    const bundleRuntime =
                      file.runtime === 'sherpa_onnx' || file.runtime === 'ace_step_cpp';
                    const requiresBundle = bundleRuntime && file.artifactCount === undefined;
                    const isBundle = bundleRuntime && (file.artifactCount ?? 0) > 1;
                    const active = selectedFile?.name === file.name;
                    return (
                      <div key={file.name} className={active ? 'bg-white/[0.04] py-3' : 'py-3'}>
                        <div className="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
                          <div className="min-w-0">
                            <p className="break-all font-mono text-xs text-text-primary">
                              {file.variant || file.name}
                            </p>
                            <p className="mt-1 text-xs text-text-muted">
                              {file.quantization} / {formatBytes(file.size)} / {file.runtime}
                              {isBundle ? ` / ${file.artifactCount} files` : ''}
                            </p>
                            <div className="mt-2">
                              <ArtifactFit
                                section={section}
                                estimatedVramMb={file.estimatedVramMb}
                                vramTotalMb={vramTotalMb}
                              />
                            </div>
                          </div>
                          <Button
                            tone={active ? 'green' : 'blue'}
                            disabled={!file.compatible || requiresBundle}
                            onClick={() => chooseFile(file)}
                          >
                            {requiresBundle
                              ? 'Included in bundle'
                              : file.compatible
                                ? active ? 'Selected' : 'Choose'
                                : 'Unverified'}
                          </Button>
                        </div>
                      </div>
                    );
                  })}
                </div>
              </>
            )}
            {selectedFile && (
              <form className="mt-4 border-l-2 border-queue-blue pl-4" onSubmit={install}>
                <h3 className="text-sm font-semibold text-text-primary">Install model</h3>
                <p className="mt-1 text-xs text-text-muted">
                  InferDeck verifies the download before registering it.
                </p>
                <label className="mt-3 block text-xs text-text-muted" htmlFor={`${section}-install-name`}>
                  InferDeck model name
                  <input
                    id={`${section}-install-name`}
                    className={`${storeInputClass} mt-1 w-full`}
                    value={modelName}
                    onChange={event => setModelName(event.target.value)}
                  />
                </label>
                <div className="mt-3 flex flex-wrap gap-2">
                  <Button
                    type="submit"
                    tone="blue"
                    disabled={installBusy || !modelName.trim()}
                  >
                    {installBusy ? 'Starting...' : 'Install model'}
                  </Button>
                  <Button
                    disabled={installBusy}
                    onClick={() => {
                      setSelectedFile(null);
                      setModelName('');
                    }}
                  >
                    Cancel selection
                  </Button>
                </div>
              </form>
            )}
          </Panel>
        </section>
      )}

      {activeTab === 'downloads' && (
        <ModelStoreDownloadsView
          downloads={scopedDownloads}
          onControl={(id, action) => { void control(id, action); }}
        />
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
