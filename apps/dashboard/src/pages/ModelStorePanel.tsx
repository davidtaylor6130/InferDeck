import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  controlStoreDownload, getStoreActivity, inspectStoreModel, installStoreModel,
  removeStoreModel, searchStore, unregisterConfiguredModel, type InstalledStoreModel, type StoreDownload,
  type StoreFile, type StoreModel,
} from '../api';
import { Badge, Button, EmptyState, Panel, ProgressBar, SectionTitle } from '../components/ui';
import { sectionLabel, type DashboardSection } from '../dashboardSections';
import { useGateway } from '../gateway';
import { formatBytes, formatDate, formatTokenCount } from '../utils';

const inputClass = 'h-9 rounded border border-white/10 bg-[#07101d] px-2 text-sm text-text-primary';
type ServerSortKey = 'name' | 'type' | 'configured' | 'runtime' | 'size';

const serverModelType = (entry: InstalledStoreModel) => {
  if (entry.hasVision) return 'Vision + text';
  if (entry.modality === 'audio_transcription') return 'Speech to text';
  if (entry.modality === 'audio_speech') return 'Text to speech';
  return entry.modality || 'Text';
};
const DISCOVERY: Record<DashboardSection, Array<{ label: string; query: string; runtime: string; modality: string }>> = {
  llm: [
    { label: 'Recommended 20–40B', query: 'Qwen3.6 GGUF', runtime: 'llama_cpp', modality: 'text' },
    { label: 'Multimodal', query: 'vision instruct GGUF', runtime: 'llama_cpp', modality: 'text' },
    { label: 'Coding', query: 'coder instruct GGUF', runtime: 'llama_cpp', modality: 'text' },
    { label: 'Efficient', query: 'instruct 8B GGUF', runtime: 'llama_cpp', modality: 'text' },
  ],
  dictation: [
    { label: 'Recommended STT', query: 'ASR ONNX', runtime: '', modality: 'audio_transcription' },
    { label: 'Whisper GGUF', query: 'Whisper GGUF', runtime: 'whisper_cpp', modality: 'audio_transcription' },
    { label: 'Parakeet ONNX', query: 'Parakeet ONNX', runtime: '', modality: 'audio_transcription' },
    { label: 'Recommended TTS', query: 'TTS ONNX', runtime: '', modality: 'audio_speech' },
    { label: 'Kokoro ONNX', query: 'Kokoro ONNX', runtime: '', modality: 'audio_speech' },
  ],
};

export const ModelStorePanel: React.FC<{ section: DashboardSection }> = ({ section }) => {
  const { status } = useGateway();
  const initial = DISCOVERY[section][0];
  const [query, setQuery] = useState(initial.query);
  const [runtime, setRuntime] = useState(initial.runtime);
  const [modality, setModality] = useState(initial.modality);
  const [results, setResults] = useState<StoreModel[]>([]);
  const [files, setFiles] = useState<StoreFile[]>([]);
  const [selectedRepo, setSelectedRepo] = useState('');
  const [downloads, setDownloads] = useState<StoreDownload[]>([]);
  const [installed, setInstalled] = useState<Record<string, InstalledStoreModel>>({});
  const [library, setLibrary] = useState<InstalledStoreModel[]>([]);
  const [recommendedOnly, setRecommendedOnly] = useState(true);
  const [vramCapacityGb, setVramCapacityGb] = useState('server');
  const [catalogSort, setCatalogSort] = useState<'downloads' | 'likes' | 'recent'>('downloads');
  const [searchBusy, setSearchBusy] = useState(false);
  const [inspectBusy, setInspectBusy] = useState(false);
  const [error, setError] = useState('');
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

  const runSearch = useCallback(async (
    nextQuery = query,
    nextRuntime = runtime,
    nextModality = modality,
  ) => {
    setSearchBusy(true);
    setError('');
    setFiles([]);
    setSelectedRepo('');
    try {
      setResults(await searchStore(nextQuery, nextRuntime, nextModality));
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setSearchBusy(false);
    }
  }, [query, runtime, modality]);

  useEffect(() => {
    const recommendation = DISCOVERY[section][0];
    setQuery(recommendation.query);
    setRuntime(recommendation.runtime);
    setModality(recommendation.modality);
    void runSearch(recommendation.query, recommendation.runtime, recommendation.modality);
  // runSearch deliberately omitted so switching filters does not retrigger discovery.
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [section]);

  useEffect(() => {
    void refresh();
    const timer = setInterval(() => { void refresh(); }, 1500);
    return () => clearInterval(timer);
  }, [refresh]);

  const inspect = async (repo: string) => {
    const request = ++inspectRequest.current;
    setInspectBusy(true);
    setError('');
    setSelectedRepo(repo);
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

  const install = async (file: StoreFile) => {
    const fallback = `${file.repo.split('/').pop() || 'model'}-${file.name.replace(/\.[^.]+$/, '').slice(-40)}`
      .replace(/[^A-Za-z0-9_.-]/g, '_');
    const modelName = window.prompt('InferDeck model name', fallback);
    if (!modelName) return;
    setError('');
    try {
      await installStoreModel(file, modelName);
      await refresh();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
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
  const changeServerSort = (key: ServerSortKey) => {
    setServerSort(current => current.key === key
      ? { key, direction: current.direction === 'asc' ? 'desc' : 'asc' }
      : { key, direction: 'asc' });
  };
  const sortHeading = (key: ServerSortKey, label: string) => (
    <button
      type="button"
      className="rounded py-2 text-left font-medium hover:text-text-primary focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-queue-blue"
      onClick={() => { changeServerSort(key); }}
    >
      {label}{serverSort.key === key ? ` (${serverSort.direction})` : ''}
    </button>
  );
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

  const chooseDiscovery = (recommendation: typeof DISCOVERY.llm[number]) => {
    setQuery(recommendation.query);
    setRuntime(recommendation.runtime);
    setModality(recommendation.modality);
    void runSearch(recommendation.query, recommendation.runtime, recommendation.modality);
  };

  return (
    <div className="space-y-4">
      <Panel>
        <SectionTitle
          title={`${sectionLabel(section)} Model Store`}
          aside={section === 'llm' && vramTotalMb
            ? `Hugging Face · ${Math.round(vramTotalMb / 1024)} GB VRAM profile`
            : 'live Hugging Face discovery'}
        />
        <p className="mt-2 max-w-3xl text-sm text-text-secondary">
          Popular compatible repositories are ranked by real downloads, then checked against this server when you inspect an artifact.
          {section === 'llm'
            ? ' Recommendations favour quality while preserving VRAM headroom for context and parallel slots.'
            : ' Speech discovery covers transcription and neural speech synthesis runtimes; sherpa models are installed as complete runtime bundles.'}
        </p>
        <div className="mt-3 flex flex-wrap gap-2">
          {DISCOVERY[section].map(recommendation => (
            <Button key={recommendation.label} onClick={() => chooseDiscovery(recommendation)}>{recommendation.label}</Button>
          ))}
        </div>
        <div className="mt-3 flex flex-wrap gap-2">
          <input
            aria-label="Search model catalogue"
            className={`${inputClass} min-w-[240px] flex-1`}
            placeholder={section === 'dictation' ? 'Search speech models' : 'Search language models'}
            value={query}
            onChange={event => setQuery(event.target.value)}
            onKeyDown={event => { if (event.key === 'Enter') void runSearch(); }}
          />
          <select aria-label="Runtime" className={inputClass} value={runtime} onChange={event => setRuntime(event.target.value)}>
            {section === 'llm' ? (
              <>
                <option value="llama_cpp">llama.cpp / GGUF</option>
                <option value="">All compatible runtimes</option>
              </>
            ) : (
              <>
                <option value="whisper_cpp">whisper.cpp STT</option>
                <option value="sherpa_onnx">sherpa-onnx speech</option>
                <option value="">All speech runtimes</option>
              </>
            )}
          </select>
          {section === 'dictation' && (
            <select aria-label="Speech service" className={inputClass} value={modality} onChange={event => setModality(event.target.value)}>
              <option value="audio_transcription">Speech to text</option>
              <option value="audio_speech">Text to speech</option>
              <option value="">All dictation models</option>
            </select>
          )}
          <Button tone="blue" disabled={searchBusy || !query.trim()} onClick={() => { void runSearch(); }}>
            {searchBusy ? 'Searching...' : 'Search'}
          </Button>
          <label className="inline-flex min-h-9 items-center gap-2 border border-white/10 px-3 text-xs text-text-secondary">
            <input
              type="checkbox"
              checked={recommendedOnly}
              onChange={event => setRecommendedOnly(event.target.checked)}
            />
            Recommended only
          </label>
          {section === 'llm' && (
            <select aria-label="VRAM capacity" className={inputClass} value={vramCapacityGb} onChange={event => setVramCapacityGb(event.target.value)}>
              <option value="server">Fits this server</option>
              <option value="8">Up to 8 GB VRAM</option>
              <option value="16">Up to 16 GB VRAM</option>
              <option value="24">Up to 24 GB VRAM</option>
              <option value="32">Up to 32 GB VRAM</option>
              <option value="48">Up to 48 GB VRAM</option>
              <option value="0">Any VRAM size</option>
            </select>
          )}
          <select aria-label="Popularity order" className={inputClass} value={catalogSort} onChange={event => setCatalogSort(event.target.value as typeof catalogSort)}>
            <option value="downloads">Most downloaded</option>
            <option value="likes">Most liked</option>
            <option value="recent">Recently updated</option>
          </select>
        </div>
        <div className="mt-2 flex flex-wrap items-center gap-2 text-xs text-text-muted">
          <span>Active filters: {recommendedOnly ? 'recommended' : 'all adoption levels'} · {catalogSort.replace('downloads', 'download popularity').replace('likes', 'like popularity').replace('recent', 'recently updated')}{section === 'llm' ? ` · ${vramCapacityGb === 'server' ? 'this server VRAM' : vramCapacityGb === '0' ? 'any VRAM' : `${vramCapacityGb} GB VRAM`}` : ''}</span>
          <button type="button" className="rounded text-queue-blue hover:underline focus-visible:outline focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-queue-blue" onClick={() => { setRecommendedOnly(false); setVramCapacityGb('0'); setCatalogSort('downloads'); }}>Clear filters</button>
        </div>
        {error && <p className="mt-2 text-xs text-danger-rose" role="alert">{error}</p>}

        {visibleResults.length === 0 ? (
          <div className="mt-4">
            <EmptyState
              title={searchBusy ? 'Checking Hugging Face...' : 'No recommended compatible models'}
              detail={!searchBusy && results.length ? 'The active adoption or VRAM filters exclude these matches. Clear filters to show every compatible result.' : undefined}
            />
          </div>
        ) : (
          <div className="mt-4 grid gap-2 lg:grid-cols-2">
            {visibleResults.slice(0, 20).map(model => (
              <button
                key={model.id}
                type="button"
                onClick={() => { void inspect(model.id); }}
                className="border border-white/10 bg-[#07101d] p-3 text-left hover:border-queue-blue/50"
              >
                <div className="flex items-center justify-between gap-2">
                  <span className="truncate font-mono text-sm text-text-primary">{model.id}</span>
                  <span className="flex shrink-0 items-center gap-1">
                    {model.hasVision && <Badge label="◈ Vision" tone="violet" />}
                    <Badge
                      label={model.recommended ? 'Recommended' : model.modality || 'text'}
                      tone={model.recommended ? 'good' : selectedRepo === model.id ? 'info' : 'idle'}
                    />
                  </span>
                </div>
                <p className="mt-1 text-xs text-text-muted">
                  {model.runtime} · {formatTokenCount(model.downloads)} downloads · {model.likes} likes
                  {section === 'llm' ? ` · ~${Math.round(estimateRepositoryVramMb(model.id) / 1024)} GB artifact estimate` : ''}
                  {model.lastModified ? ` · updated ${formatDate(model.lastModified)}` : ''}
                </p>
              </button>
            ))}
          </div>
        )}
      </Panel>

      {selectedRepo && (
        <Panel>
          <SectionTitle
            title="Available artifacts"
            aside={`${selectedRepo}${selectedModel?.hasVision ? ' · multimodal repository' : ''}`}
          />
          {section === 'llm' && (
            <p className="mt-2 text-xs text-text-muted">
              VRAM fit uses artifact size plus a safety reserve. Context length, KV-cache precision, and parallel slots consume additional memory and are verified in the active profile.
            </p>
          )}
          {section === 'dictation' && files.some(file => file.runtime === 'sherpa_onnx') && (
            <p className="mt-2 text-xs text-text-muted">
              Sherpa-onnx repositories install as one verified bundle. InferDeck downloads every required model, token, and vocabulary artifact into staging, then publishes the complete bundle atomically.
            </p>
          )}
          {files.length === 0 ? (
            <div className="mt-3"><EmptyState title={inspectBusy ? 'Inspecting repository...' : 'No compatible artifacts found'} /></div>
          ) : (
            <div className="mt-3 overflow-x-auto">
              <table className="w-full min-w-[760px] text-left text-sm">
                <thead>
                  <tr className="border-b border-white/10 text-xs uppercase tracking-wide text-text-muted">
                    <th className="py-2 pr-3 font-medium">File</th>
                    <th className="pr-3 font-medium">Runtime</th>
                    <th className="pr-3 font-medium">Size</th>
                    <th className="pr-3 font-medium">{section === 'llm' ? 'Hardware fit' : 'Execution'}</th>
                    <th className="font-medium"></th>
                  </tr>
                </thead>
                <tbody className="divide-y divide-white/5">
                  {files.map(file => {
                    const requiresBundle = file.runtime === 'sherpa_onnx' && file.artifactCount === undefined;
                    const isBundle = file.runtime === 'sherpa_onnx' && (file.artifactCount ?? 0) > 1;
                    return (
                    <tr key={file.name}>
                      <td className="max-w-[380px] truncate py-2 pr-3 font-mono text-xs text-text-primary" title={file.name}>{file.name}</td>
                      <td className="pr-3 text-text-secondary">{file.runtime} · {file.modality} · {file.quantization}</td>
                      <td className="pr-3 text-text-secondary">{formatBytes(file.size)}</td>
                      <td className="pr-3 text-text-secondary">
                        <ArtifactFit
                          section={section}
                          estimatedVramMb={file.estimatedVramMb}
                          vramTotalMb={vramTotalMb}
                        />
                      </td>
                      <td className="text-right">
                        <Button
                          tone="blue"
                          disabled={!file.compatible || requiresBundle}
                          onClick={() => { void install(file); }}
                        >
                          {requiresBundle ? 'Included in bundle' : file.compatible ? isBundle ? `Install ${file.artifactCount} artifacts` : 'Download' : 'Unverified'}
                        </Button>
                      </td>
                    </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          )}
        </Panel>
      )}

      <Panel>
        <SectionTitle title="Models on this server" aside={`${scopedLibrary.length} detected`} />
        <p className="mt-2 text-xs text-text-muted">
          Configured external models can be removed from InferDeck without deleting their files. Managed downloads also support archive and permanent deletion.
        </p>
        {scopedLibrary.length === 0 ? (
          <div className="mt-3"><EmptyState title={`No downloaded ${sectionLabel(section)} models`} detail="No compatible artifacts were detected in the model library." /></div>
        ) : (
          <div className="mt-3 overflow-x-auto">
            <table className="w-full min-w-[980px] text-left text-sm">
              <thead>
                <tr className="border-b border-white/10 text-xs uppercase tracking-wide text-text-muted">
                  <th aria-sort={serverSort.key === 'name' ? `${serverSort.direction}ending` : 'none'} className="pr-4">{sortHeading('name', 'Name')}</th>
                  <th aria-sort={serverSort.key === 'type' ? `${serverSort.direction}ending` : 'none'} className="pr-4">{sortHeading('type', 'Type')}</th>
                  <th aria-sort={serverSort.key === 'configured' ? `${serverSort.direction}ending` : 'none'} className="pr-4">{sortHeading('configured', 'Configured')}</th>
                  <th aria-sort={serverSort.key === 'runtime' ? `${serverSort.direction}ending` : 'none'} className="pr-4">{sortHeading('runtime', 'Runtime')}</th>
                  <th aria-sort={serverSort.key === 'size' ? `${serverSort.direction}ending` : 'none'} className="pr-4">{sortHeading('size', 'Disk size')}</th>
                  <th className="py-2 pr-4 font-medium">Storage</th>
                  <th className="py-2 text-right font-medium">Actions</th>
                </tr>
              </thead>
              <tbody className="divide-y divide-white/5">
                {sortedLibrary.map(entry => (
                  <tr key={entry.id || `${entry.name}:${entry.path}`}>
                    <td className="max-w-[340px] py-3 pr-4">
                      <p className="truncate font-mono text-text-primary">{entry.name || 'Unconfigured model artifact'}</p>
                      <p className="truncate text-xs text-text-muted" title={entry.path}>{entry.path || 'Managed storage'}</p>
                    </td>
                    <td className="pr-4"><Badge label={serverModelType(entry)} tone={entry.hasVision ? 'violet' : 'idle'} /></td>
                    <td className="pr-4"><Badge label={entry.configured ? 'Yes' : 'No'} tone={entry.configured ? 'good' : 'warn'} /></td>
                    <td className="pr-4 font-mono text-xs text-text-secondary">{entry.runtime || 'Unknown'}</td>
                    <td className="pr-4 tabular-nums text-text-secondary">{formatBytes(entry.size ?? 0)}</td>
                    <td className="pr-4 text-xs text-text-secondary">
                      {entry.managed ? 'InferDeck managed' : 'External'}
                      {entry.artifactCount ? ` · ${entry.artifactCount} artifact${entry.artifactCount === 1 ? '' : 's'}` : ''}
                      {entry.quantization && entry.quantization !== 'unknown' ? ` · ${entry.quantization}` : ''}
                    </td>
                    <td className="py-3 text-right">
                      {entry.managed
                        ? <span className="flex justify-end gap-2"><Button onClick={() => { if (entry.name) void retire(entry.name, 'archive'); }}>Archive</Button><Button tone="danger" onClick={() => { if (entry.name) void retire(entry.name, 'remove'); }}>Delete permanently</Button></span>
                        : entry.configured && entry.name
                          ? <Button tone="danger" onClick={() => { void unregister(entry.name!); }}>Remove from InferDeck</Button>
                          : <span className="text-xs text-text-muted">Detected on disk</span>}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </Panel>

      {scopedDownloads.length > 0 && (
        <Panel>
          <SectionTitle title="Download activity" aside="current and recent" />
          <div className="mt-3 space-y-3">
            {scopedDownloads.map(download => {
              const percent = download.bytesTotal ? download.bytesDownloaded / download.bytesTotal * 100 : 0;
              return (
                <div key={download.id} className="border-l border-border-slate pl-3">
                  <div className="flex items-center justify-between gap-2">
                    <span className="truncate font-mono text-xs text-text-primary">{download.modelName}</span>
                    <Badge label={download.state} tone={download.state === 'installed' ? 'good' : download.state === 'failed' ? 'critical' : 'info'} />
                  </div>
                  <div className="mt-2"><ProgressBar percent={percent} tone={download.state === 'failed' ? 'critical' : 'info'} /></div>
                  <div className="mt-2 flex items-center justify-between gap-2 text-xs text-text-muted">
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
