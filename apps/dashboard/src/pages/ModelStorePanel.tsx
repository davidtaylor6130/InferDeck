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
import { Badge, Button, EmptyState, Panel, ProgressBar, SectionTitle } from '../components/ui';
import { sectionLabel, type DashboardSection } from '../dashboardSections';
import { useGateway } from '../gateway';
import { formatBytes, formatDate, formatTokenCount } from '../utils';

const inputClass = 'min-h-10 rounded border border-white/10 bg-[#07101d] px-3 text-sm text-text-primary';
type ServerSortKey = 'name' | 'type' | 'configured' | 'runtime' | 'size';

const serverModelType = (entry: InstalledStoreModel) => {
  if (entry.hasVision) return 'Vision and text';
  if (entry.modality === 'audio_transcription') return 'Speech to text';
  if (entry.modality === 'audio_speech') return 'Text to speech';
  return entry.modality || 'Text';
};

const DISCOVERY: Record<DashboardSection, Array<{ label: string; query: string; runtime: string; modality: string }>> = {
  llm: [
    { label: 'Balanced 20–40B', query: 'Qwen3.6 GGUF', runtime: 'llama_cpp', modality: 'text' },
    { label: 'Vision', query: 'vision instruct GGUF', runtime: 'llama_cpp', modality: 'text' },
    { label: 'Coding', query: 'coder instruct GGUF', runtime: 'llama_cpp', modality: 'text' },
    { label: 'Fast 8B', query: 'instruct 8B GGUF', runtime: 'llama_cpp', modality: 'text' },
  ],
  dictation: [
    { label: 'Speech to text', query: 'ASR ONNX', runtime: '', modality: 'audio_transcription' },
    { label: 'Whisper', query: 'Whisper GGUF', runtime: 'whisper_cpp', modality: 'audio_transcription' },
    { label: 'Parakeet', query: 'Parakeet ONNX', runtime: '', modality: 'audio_transcription' },
    { label: 'Text to speech', query: 'TTS ONNX', runtime: '', modality: 'audio_speech' },
    { label: 'Kokoro', query: 'Kokoro ONNX', runtime: '', modality: 'audio_speech' },
  ],
};

export function defaultStoreModelName(file: Pick<StoreFile, 'repo' | 'name'>): string {
  return `${file.repo.split('/').pop() || 'model'}-${file.name.replace(/\.[^.]+$/, '').slice(-40)}`
    .replace(/[^A-Za-z0-9_.-]/g, '_');
}

export const ModelStorePanel: React.FC<{ section: DashboardSection }> = ({ section }) => {
  const { status } = useGateway();
  const initial = DISCOVERY[section][0];
  const [query, setQuery] = useState(initial.query);
  const [runtime, setRuntime] = useState(initial.runtime);
  const [modality, setModality] = useState(initial.modality);
  const [results, setResults] = useState<StoreModel[]>([]);
  const [files, setFiles] = useState<StoreFile[]>([]);
  const [selectedRepo, setSelectedRepo] = useState('');
  const [selectedFile, setSelectedFile] = useState<StoreFile | null>(null);
  const [modelName, setModelName] = useState('');
  const [downloads, setDownloads] = useState<StoreDownload[]>([]);
  const [installed, setInstalled] = useState<Record<string, InstalledStoreModel>>({});
  const [library, setLibrary] = useState<InstalledStoreModel[]>([]);
  const [recommendedOnly, setRecommendedOnly] = useState(true);
  const [vramCapacityGb, setVramCapacityGb] = useState('server');
  const [catalogSort, setCatalogSort] = useState<'downloads' | 'likes' | 'recent'>('downloads');
  const [searchBusy, setSearchBusy] = useState(false);
  const [inspectBusy, setInspectBusy] = useState(false);
  const [installBusy, setInstallBusy] = useState(false);
  const [searchError, setSearchError] = useState('');
  const [error, setError] = useState('');
  const [notice, setNotice] = useState('');
  const [serverSort, setServerSort] = useState<{ key: ServerSortKey; direction: 'asc' | 'desc' }>({ key: 'name', direction: 'asc' });
  const inspectRequest = useRef(0);

  const refresh = useCallback(async () => {
    try {
      const activity = await getStoreActivity();
      setDownloads(activity.downloads);
      setInstalled(activity.installed);
      setLibrary(Array.isArray(activity.library) ? activity.library : []);
    } catch {}
  }, []);

  const executeSearch = useCallback(async (nextQuery: string, nextRuntime: string, nextModality: string) => {
    setSearchBusy(true);
    setSearchError('');
    setError('');
    setNotice('');
    setResults([]);
    setFiles([]);
    setSelectedRepo('');
    setSelectedFile(null);
    try {
      setResults(await searchStore(nextQuery, nextRuntime, nextModality));
    } catch (reason) {
      setSearchError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setSearchBusy(false);
    }
  }, []);

  useEffect(() => {
    const recommendation = DISCOVERY[section][0];
    setQuery(recommendation.query);
    setRuntime(recommendation.runtime);
    setModality(recommendation.modality);
    void executeSearch(recommendation.query, recommendation.runtime, recommendation.modality);
  }, [section, executeSearch]);

  useEffect(() => {
    void refresh();
    const timer = setInterval(() => { void refresh(); }, 1500);
    return () => clearInterval(timer);
  }, [refresh]);

  const inspect = async (repo: string) => {
    const request = ++inspectRequest.current;
    setInspectBusy(true);
    setError('');
    setNotice('');
    setSelectedRepo(repo);
    setSelectedFile(null);
    try {
      const next = await inspectStoreModel(repo);
      if (request === inspectRequest.current) {
        setFiles(next.filter(file =>
          section === 'dictation'
            ? file.modality === 'audio_transcription' || file.modality === 'audio_speech'
            : file.modality !== 'audio_transcription' && file.modality !== 'audio_speech'));
      }
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      if (request === inspectRequest.current) setInspectBusy(false);
    }
  };

  const chooseFile = (file: StoreFile) => {
    setSelectedFile(file);
    setModelName(defaultStoreModelName(file));
    setError('');
    setNotice('');
  };

  const install = async (event: React.FormEvent) => {
    event.preventDefault();
    if (!selectedFile || !modelName.trim()) return;
    setInstallBusy(true);
    setError('');
    setNotice('');
    try {
      await installStoreModel(selectedFile, modelName.trim());
      setNotice(`${modelName.trim()} was added to the download queue.`);
      setSelectedFile(null);
      setModelName('');
      await refresh();
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
    if (!window.confirm(`Remove ${model} from InferDeck? Its external model files will remain on disk.`)) return;
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
      section === 'dictation'
        ? entry.modality === 'audio_transcription' || entry.modality === 'audio_speech'
        : entry.modality !== 'audio_transcription' && entry.modality !== 'audio_speech'),
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
    return entries.filter(entry =>
      section === 'dictation'
        ? entry.modality === 'audio_transcription' || entry.modality === 'audio_speech'
        : entry.modality !== 'audio_transcription' && entry.modality !== 'audio_speech');
  }, [library, scopedInstalled, section]);
  const sortedLibrary = useMemo(() => {
    const value = (entry: InstalledStoreModel) => {
      if (serverSort.key === 'name') return entry.name || entry.path || '';
      if (serverSort.key === 'type') return serverModelType(entry);
      if (serverSort.key === 'configured') return entry.configured ? 1 : 0;
      if (serverSort.key === 'runtime') return entry.runtime || '';
      return entry.size ?? 0;
    };
    return [...scopedLibrary].sort((left, right) => {
      const leftValue = value(left);
      const rightValue = value(right);
      const compared = typeof leftValue === 'number' && typeof rightValue === 'number'
        ? leftValue - rightValue
        : String(leftValue).localeCompare(String(rightValue), undefined, { numeric: true, sensitivity: 'base' });
      return serverSort.direction === 'asc' ? compared : -compared;
    });
  }, [scopedLibrary, serverSort]);
  const scopedDownloads = downloads.filter(download =>
    section === 'dictation'
      ? download.modality === 'audio_transcription' || download.modality === 'audio_speech'
      : download.modality !== 'audio_transcription' && download.modality !== 'audio_speech');
  const gpu = (status?.hardware?.gpu ?? {}) as Record<string, unknown>;
  const vramTotalMb = Number(gpu.vramTotal ?? 0) / (1024 * 1024);
  const popularityFloor = section === 'llm' ? 1_000 : 100;
  const selectedCapacityMb = vramCapacityGb === 'server'
    ? vramTotalMb
    : Number(vramCapacityGb) * 1024;
  const visibleResults = useMemo(() => results
    .filter(model => !recommendedOnly || model.recommended || model.downloads >= popularityFloor)
    .filter(model => section !== 'llm' || !selectedCapacityMb || estimateRepositoryVramMb(model.id) <= selectedCapacityMb * 0.85)
    .sort((left, right) => {
      if (catalogSort === 'likes') return right.likes - left.likes || right.downloads - left.downloads;
      if (catalogSort === 'recent') return Date.parse(right.lastModified || '') - Date.parse(left.lastModified || '') || right.downloads - left.downloads;
      return right.downloads - left.downloads || right.likes - left.likes;
    }), [results, recommendedOnly, popularityFloor, section, selectedCapacityMb, catalogSort]);
  const selectedModel = results.find(model => model.id === selectedRepo);
  const activeFilters = [
    recommendedOnly ? 'recommended' : 'all adoption levels',
    section === 'llm' ? (vramCapacityGb === 'server' ? 'fits this server' : vramCapacityGb === '0' ? 'any VRAM' : `up to ${vramCapacityGb} GB`) : '',
    catalogSort === 'downloads' ? 'most downloaded' : catalogSort === 'likes' ? 'most liked' : 'recently updated',
  ].filter(Boolean).join(' · ');

  const chooseDiscovery = (recommendation: typeof DISCOVERY.llm[number]) => {
    setQuery(recommendation.query);
    setRuntime(recommendation.runtime);
    setModality(recommendation.modality);
    void executeSearch(recommendation.query, recommendation.runtime, recommendation.modality);
  };

  return (
    <div className="space-y-5">
      {error && <p className="border-l-2 border-danger-rose bg-danger-rose/10 px-3 py-2 text-xs text-danger-rose" role="alert">{error}</p>}
      <section className="grid gap-5 xl:grid-cols-[minmax(0,1.15fr)_minmax(360px,0.85fr)]">
        <Panel className="border-t-0 pt-0">
          <SectionTitle
            title={`${sectionLabel(section)} Model Store`}
            aside={section === 'llm' && vramTotalMb ? `${Math.round(vramTotalMb / 1024)} GB VRAM detected` : 'Hugging Face catalogue'}
          />
          <p className="mt-2 max-w-2xl text-sm text-text-secondary">
            Find a compatible model, compare the files that fit this machine, then start a verified background install.
          </p>

          <form className="mt-5" onSubmit={event => { event.preventDefault(); void executeSearch(query, runtime, modality); }}>
            <label className="text-sm font-medium text-text-primary" htmlFor={`${section}-model-search`}>1. Find a model</label>
            <div className="mt-2 flex flex-col gap-2 sm:flex-row">
              <input
                id={`${section}-model-search`}
                className={`${inputClass} min-w-0 flex-1`}
                placeholder={section === 'dictation' ? 'Search speech models' : 'Search language models'}
                value={query}
                onChange={event => setQuery(event.target.value)}
              />
              <Button type="submit" tone="blue" disabled={searchBusy || !query.trim()} className="sm:min-w-24">
                {searchBusy ? 'Searching…' : 'Search'}
              </Button>
            </div>
          </form>

          <div className="mt-3 flex flex-wrap items-center gap-x-3 gap-y-2 text-xs">
            <span className="text-text-muted">Start with</span>
            {DISCOVERY[section].map(recommendation => (
              <button
                key={recommendation.label}
                type="button"
                className="rounded py-1 text-queue-blue hover:underline focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-queue-blue"
                onClick={() => chooseDiscovery(recommendation)}
              >
                {recommendation.label}
              </button>
            ))}
          </div>

          <details className="mt-4 border-y border-border-slate py-3">
            <summary className="cursor-pointer text-sm font-medium text-text-secondary">
              Advanced filters <span className="ml-1 text-xs font-normal text-text-muted">({activeFilters})</span>
            </summary>
            <div className="mt-3 grid gap-3 sm:grid-cols-2">
              <label className="text-xs text-text-muted">
                Runtime
                <select className={`${inputClass} mt-1 w-full`} value={runtime} onChange={event => setRuntime(event.target.value)}>
                  {section === 'llm' ? (
                    <>
                      <option value="llama_cpp">llama.cpp / GGUF</option>
                      <option value="">All compatible runtimes</option>
                    </>
                  ) : (
                    <>
                      <option value="whisper_cpp">whisper.cpp speech to text</option>
                      <option value="sherpa_onnx">sherpa-onnx speech</option>
                      <option value="">All speech runtimes</option>
                    </>
                  )}
                </select>
              </label>
              {section === 'dictation' && (
                <label className="text-xs text-text-muted">
                  Speech service
                  <select className={`${inputClass} mt-1 w-full`} value={modality} onChange={event => setModality(event.target.value)}>
                    <option value="audio_transcription">Speech to text</option>
                    <option value="audio_speech">Text to speech</option>
                    <option value="">All dictation models</option>
                  </select>
                </label>
              )}
              {section === 'llm' && (
                <label className="text-xs text-text-muted">
                  VRAM capacity
                  <select className={`${inputClass} mt-1 w-full`} value={vramCapacityGb} onChange={event => setVramCapacityGb(event.target.value)}>
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
              <label className="text-xs text-text-muted">
                Order
                <select className={`${inputClass} mt-1 w-full`} value={catalogSort} onChange={event => setCatalogSort(event.target.value as typeof catalogSort)}>
                  <option value="downloads">Most downloaded</option>
                  <option value="likes">Most liked</option>
                  <option value="recent">Recently updated</option>
                </select>
              </label>
              <label className="inline-flex min-h-10 items-center gap-2 text-xs text-text-secondary">
                <input type="checkbox" checked={recommendedOnly} onChange={event => setRecommendedOnly(event.target.checked)} />
                Hide low-adoption results
              </label>
              <div className="flex items-center sm:justify-end">
                <Button onClick={() => { setRecommendedOnly(false); setVramCapacityGb('0'); setCatalogSort('downloads'); }}>Clear filters</Button>
              </div>
            </div>
          </details>

          {searchError && (
            <div className="mt-3 flex flex-wrap items-center justify-between gap-2 border-l-2 border-danger-rose bg-danger-rose/10 px-3 py-2 text-xs text-danger-rose" role="alert">
              <span>{searchError}</span>
              <Button tone="danger" onClick={() => { void executeSearch(query, runtime, modality); }}>Retry search</Button>
            </div>
          )}

          <div className="mt-4" aria-live="polite">
            <div className="flex items-baseline justify-between gap-3">
              <h3 className="text-sm font-semibold text-text-primary">Search results</h3>
              <span className="text-xs text-text-muted">{searchBusy ? 'Searching…' : `${visibleResults.length} shown`}</span>
            </div>
            {visibleResults.length === 0 ? (
              <div className="mt-2">
                <EmptyState
                  title={searchBusy ? 'Checking compatible repositories…' : 'No matching models'}
                  detail={!searchBusy && results.length ? 'The current adoption or VRAM filters exclude every match. Open Advanced filters to broaden the results.' : 'Try a model family, task, or repository name.'}
                />
              </div>
            ) : (
              <div className="mt-2 divide-y divide-white/10 border-y border-white/10">
                {visibleResults.slice(0, 20).map(model => (
                  <button
                    key={model.id}
                    type="button"
                    aria-pressed={selectedRepo === model.id}
                    onClick={() => { void inspect(model.id); }}
                    className={`grid w-full gap-2 px-1 py-3 text-left transition-colors sm:grid-cols-[minmax(0,1fr)_auto] sm:items-center ${selectedRepo === model.id ? 'bg-white/[0.06]' : 'hover:bg-white/[0.03]'}`}
                  >
                    <span className="min-w-0">
                      <span className="block break-all font-mono text-sm text-text-primary">{model.id}</span>
                      <span className="mt-1 block text-xs text-text-muted">
                        {formatTokenCount(model.downloads)} downloads · {model.likes} likes
                        {section === 'llm' ? ` · about ${Math.round(estimateRepositoryVramMb(model.id) / 1024)} GB model file` : ''}
                        {model.lastModified ? ` · updated ${formatDate(model.lastModified)}` : ''}
                      </span>
                    </span>
                    <span className="flex flex-wrap items-center gap-2 sm:justify-end">
                      {model.hasVision && <Badge label="Vision" tone="violet" />}
                      {model.recommended && <Badge label="Recommended" tone="good" />}
                      <span className="text-xs font-medium text-queue-blue">Review files</span>
                    </span>
                  </button>
                ))}
              </div>
            )}
          </div>
        </Panel>

        <Panel className="xl:border-t-0 xl:pt-0">
          <SectionTitle title="2. Review and install" aside={selectedRepo || 'select a result'} />
          {!selectedRepo ? (
            <div className="mt-4"><EmptyState title="Select a model" detail="Compatible files and hardware fit will appear here without leaving the search results." /></div>
          ) : inspectBusy ? (
            <p className="mt-4 border-y border-dashed border-border-slate py-8 text-center text-sm text-text-muted" role="status">Inspecting repository files…</p>
          ) : files.length === 0 ? (
            <div className="mt-4"><EmptyState title="No compatible files found" detail="Choose another repository or broaden the search." /></div>
          ) : (
            <>
              <div className="mt-3 flex flex-wrap gap-2 text-xs text-text-muted">
                <span>{selectedModel?.runtime}</span>
                {selectedModel?.hasVision && <Badge label="Vision capable" tone="violet" />}
                <span>{files.length} compatible file{files.length === 1 ? '' : 's'}</span>
              </div>
              <div className="mt-3 divide-y divide-white/10 border-y border-white/10">
                {files.map(file => {
                  const requiresBundle = file.runtime === 'sherpa_onnx' && file.artifactCount === undefined;
                  const isBundle = file.runtime === 'sherpa_onnx' && (file.artifactCount ?? 0) > 1;
                  const active = selectedFile?.name === file.name;
                  return (
                    <div key={file.name} className={`py-3 ${active ? 'bg-white/[0.04]' : ''}`}>
                      <div className="flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between">
                        <div className="min-w-0">
                          <p className="break-all font-mono text-xs text-text-primary">{file.name}</p>
                          <p className="mt-1 text-xs text-text-muted">
                            {file.quantization} · {formatBytes(file.size)} · {file.runtime}
                            {isBundle ? ` · ${file.artifactCount} files` : ''}
                          </p>
                          <div className="mt-2"><ArtifactFit section={section} estimatedVramMb={file.estimatedVramMb} vramTotalMb={vramTotalMb} /></div>
                        </div>
                        <Button
                          tone={active ? 'green' : 'blue'}
                          disabled={!file.compatible || requiresBundle}
                          onClick={() => chooseFile(file)}
                        >
                          {requiresBundle ? 'Included in bundle' : file.compatible ? active ? 'Selected' : 'Choose file' : 'Unverified'}
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
              <h3 className="text-sm font-semibold text-text-primary">3. Name and install</h3>
              <p className="mt-1 text-xs text-text-muted">The verified download runs in the background and can be cancelled or resumed.</p>
              <label className="mt-3 block text-xs text-text-muted" htmlFor={`${section}-install-name`}>
                InferDeck model name
                <input
                  id={`${section}-install-name`}
                  className={`${inputClass} mt-1 w-full`}
                  value={modelName}
                  onChange={event => setModelName(event.target.value)}
                />
              </label>
              <div className="mt-3 flex flex-wrap gap-2">
                <Button type="submit" tone="blue" disabled={installBusy || !modelName.trim()}>{installBusy ? 'Starting…' : 'Install model'}</Button>
                <Button disabled={installBusy} onClick={() => { setSelectedFile(null); setModelName(''); }}>Choose another file</Button>
              </div>
            </form>
          )}
          {notice && <p className="mt-4 text-xs text-success-green" role="status">{notice}</p>}
        </Panel>
      </section>

      {scopedDownloads.length > 0 && (
        <Panel>
          <SectionTitle title="Download activity" aside="current and recent" />
          <div className="mt-3 space-y-3">
            {scopedDownloads.map(download => {
              const percent = download.bytesTotal ? download.bytesDownloaded / download.bytesTotal * 100 : 0;
              return (
                <div key={download.id} className="border-l border-border-slate pl-3">
                  <div className="flex flex-wrap items-center justify-between gap-2">
                    <span className="break-all font-mono text-xs text-text-primary">{download.modelName}</span>
                    <Badge label={download.state} tone={download.state === 'installed' ? 'good' : download.state === 'failed' ? 'critical' : 'info'} />
                  </div>
                  <div className="mt-2"><ProgressBar percent={percent} tone={download.state === 'failed' ? 'critical' : 'info'} /></div>
                  <div className="mt-2 flex flex-wrap items-center justify-between gap-2 text-xs text-text-muted">
                    <span>{formatBytes(download.bytesDownloaded)} / {formatBytes(download.bytesTotal)}{download.error ? ` · ${download.error}` : ''}</span>
                    {download.state === 'downloading' || download.state === 'queued'
                      ? <Button tone="danger" onClick={() => { void control(download.id, 'cancel'); }}>Cancel</Button>
                      : download.state === 'failed' || download.state === 'cancelled'
                        ? <Button onClick={() => { void control(download.id, 'resume'); }}>Resume</Button>
                        : null}
                  </div>
                </div>
              );
            })}
          </div>
        </Panel>
      )}

      <Panel>
        <details>
          <summary className="cursor-pointer text-base font-semibold text-text-primary">
            Models on this server <span className="text-xs font-normal text-text-muted">({scopedLibrary.length} detected)</span>
          </summary>
          <p className="mt-2 max-w-3xl text-xs text-text-muted">
            Configured external models can be removed from InferDeck without deleting their files. Managed downloads can also be archived or permanently deleted.
          </p>
          <div className="mt-3 flex flex-wrap items-end gap-2">
            <label className="text-xs text-text-muted">
              Sort by
              <select className={`${inputClass} mt-1`} value={serverSort.key} onChange={event => setServerSort(current => ({ ...current, key: event.target.value as ServerSortKey }))}>
                <option value="name">Name</option>
                <option value="type">Type</option>
                <option value="configured">Configured</option>
                <option value="runtime">Runtime</option>
                <option value="size">Disk size</option>
              </select>
            </label>
            <Button onClick={() => setServerSort(current => ({ ...current, direction: current.direction === 'asc' ? 'desc' : 'asc' }))}>
              {serverSort.direction === 'asc' ? 'Ascending' : 'Descending'}
            </Button>
          </div>
          {scopedLibrary.length === 0 ? (
            <div className="mt-3"><EmptyState title={`No downloaded ${sectionLabel(section)} models`} detail="No compatible artifacts were detected in the model library." /></div>
          ) : (
            <div className="mt-3 divide-y divide-white/10 border-y border-white/10">
              {sortedLibrary.map(entry => (
                <div key={entry.id || `${entry.name}:${entry.path}`} className="grid gap-3 py-3 md:grid-cols-[minmax(0,1fr)_minmax(220px,auto)] md:items-center">
                  <div className="min-w-0">
                    <p className="break-all font-mono text-sm text-text-primary">{entry.name || 'Unconfigured model artifact'}</p>
                    <p className="mt-1 break-all text-xs text-text-muted">{entry.path || 'Managed storage'}</p>
                    <div className="mt-2 flex flex-wrap items-center gap-2 text-xs text-text-secondary">
                      <Badge label={serverModelType(entry)} tone={entry.hasVision ? 'violet' : 'idle'} />
                      <Badge label={entry.configured ? 'Configured' : 'Not configured'} tone={entry.configured ? 'good' : 'warn'} />
                      <span>{entry.runtime || 'Unknown runtime'}</span>
                      <span>{formatBytes(entry.size ?? 0)}</span>
                      <span>{entry.managed ? 'InferDeck managed' : 'External'}</span>
                    </div>
                  </div>
                  <div className="flex flex-wrap gap-2 md:justify-end">
                    {entry.managed ? (
                      <>
                        <Button onClick={() => { if (entry.name) void retire(entry.name, 'archive'); }}>Archive</Button>
                        <Button tone="danger" onClick={() => { if (entry.name) void retire(entry.name, 'remove'); }}>Delete permanently</Button>
                      </>
                    ) : entry.configured && entry.name ? (
                      <Button tone="danger" onClick={() => { void unregister(entry.name!); }}>Remove from InferDeck</Button>
                    ) : (
                      <span className="text-xs text-text-muted">Detected on disk</span>
                    )}
                  </div>
                </div>
              ))}
            </div>
          )}
        </details>
      </Panel>
    </div>
  );
};

const ArtifactFit: React.FC<{
  section: DashboardSection;
  estimatedVramMb: number;
  vramTotalMb: number;
}> = ({ section, estimatedVramMb, vramTotalMb }) => {
  if (section === 'dictation') return <Badge label="CPU-friendly" tone="good" />;
  if (!vramTotalMb) return <Badge label={`${estimatedVramMb.toLocaleString()} MB · verify profile`} tone="warn" />;
  const share = estimatedVramMb / vramTotalMb;
  if (share <= 0.65) return <Badge label="Fits with headroom" tone="good" />;
  if (share <= 0.85) return <Badge label="Fits · tight at long context" tone="warn" />;
  return <Badge label="Not recommended for this VRAM" tone="critical" />;
};

function estimateRepositoryVramMb(modelId: string): number {
  const matches = Array.from(modelId.matchAll(/(?:^|[-_.])(\d+(?:\.\d+)?)b(?:$|[-_.])/gi));
  const parametersBillions = matches.length ? Math.max(...matches.map(match => Number(match[1]))) : 8;
  const lower = modelId.toLowerCase();
  const bytesPerParameter = lower.includes('q2') || lower.includes('iq2') ? 0.38
    : lower.includes('q3') || lower.includes('iq3') ? 0.5
      : lower.includes('q5') ? 0.75
        : lower.includes('q6') ? 0.88
          : lower.includes('q8') ? 1.1
            : lower.includes('f16') || lower.includes('bf16') ? 2.1
              : 0.65;
  return parametersBillions * 1024 * bytesPerParameter + 1024;
}
