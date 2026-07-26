import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  controlStoreDownload, getStoreActivity, inspectStoreModel, installStoreModel,
  removeStoreModel, searchStore, type InstalledStoreModel, type StoreDownload,
  type StoreFile, type StoreModel,
} from '../api';
import { Badge, Button, EmptyState, Panel, ProgressBar, SectionTitle } from '../components/ui';
import { sectionLabel, type DashboardSection } from '../dashboardSections';
import { useGateway } from '../gateway';
import { formatBytes, formatDate, formatTokenCount } from '../utils';

const inputClass = 'h-9 rounded border border-white/10 bg-[#07101d] px-2 text-sm text-text-primary';
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
  const [searchBusy, setSearchBusy] = useState(false);
  const [inspectBusy, setInspectBusy] = useState(false);
  const [error, setError] = useState('');
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

  const remove = async (model: string) => {
    if (!window.confirm(`Remove ${model} and its store-managed artifact?`)) return;
    setError('');
    try {
      await removeStoreModel(model);
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
  const scopedDownloads = downloads.filter(download =>
    section === 'dictation'
      ? download.modality === 'audio_transcription' || download.modality === 'audio_speech'
      : download.modality !== 'audio_transcription' && download.modality !== 'audio_speech');
  const popularityFloor = section === 'llm' ? 1_000 : 100;
  const visibleResults = useMemo(
    () => results.filter(model =>
      !recommendedOnly || model.recommended || model.downloads >= popularityFloor),
    [results, recommendedOnly, popularityFloor],
  );
  const gpu = (status?.hardware?.gpu ?? {}) as Record<string, unknown>;
  const vramTotalMb = Number(gpu.vramTotal ?? 0) / (1024 * 1024);
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
          title={`${sectionLabel(section)} model catalogue`}
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
        </div>
        {error && <p className="mt-2 text-xs text-danger-rose" role="alert">{error}</p>}

        {visibleResults.length === 0 ? (
          <div className="mt-4">
            <EmptyState
              title={searchBusy ? 'Checking Hugging Face...' : 'No recommended compatible models'}
              detail={!searchBusy && results.length ? 'Turn off Recommended only to show lower-adoption matches.' : undefined}
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
              Sherpa-onnx repositories require a verified model bundle, not one ONNX file. InferDeck shows the compatible artifacts for discovery, but disables individual downloads so it cannot create an incomplete STT or TTS installation.
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
                    const requiresBundle = file.runtime === 'sherpa_onnx';
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
                          {requiresBundle ? 'Bundle required' : file.compatible ? 'Download' : 'Unverified'}
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
          This combines the active profile, InferDeck-managed downloads, and compatible artifacts already present in the model library. External files are read-only.
        </p>
        {scopedLibrary.length === 0 ? (
          <div className="mt-3"><EmptyState title={`No downloaded ${sectionLabel(section)} models`} detail="No compatible artifacts were detected in the model library." /></div>
        ) : (
          <div className="mt-3 divide-y divide-white/5">
            {scopedLibrary.map(entry => (
              <div key={entry.id || `${entry.name}:${entry.path}`} className="grid gap-2 py-3 text-sm md:grid-cols-[1fr_auto_auto] md:items-center">
                <div className="min-w-0">
                  <p className="truncate font-mono text-text-primary">{entry.name || 'Unconfigured model artifact'}</p>
                  <p className="truncate text-xs text-text-muted" title={entry.path}>
                    {entry.path || 'Managed storage'} · {formatBytes(entry.size ?? 0)}
                    {entry.artifactCount ? ` · ${entry.artifactCount} artifact${entry.artifactCount === 1 ? '' : 's'}` : ''}
                    {entry.quantization && entry.quantization !== 'unknown' ? ` · ${entry.quantization}` : ''}
                  </p>
                </div>
                <span className="flex flex-wrap items-center gap-1">
                  {entry.hasVision && <Badge label="◈ Vision" tone="violet" />}
                  <Badge label={entry.configured ? 'Configured' : 'On disk'} tone={entry.configured ? 'good' : 'warn'} />
                  <Badge label={`${entry.runtime || 'runtime'} · ${entry.modality || 'text'}`} tone="idle" />
                </span>
                {entry.managed
                  ? <Button tone="danger" onClick={() => { if (entry.name) void remove(entry.name); }}>Remove</Button>
                  : <span className="text-right text-xs text-text-muted">Read-only</span>}
              </div>
            ))}
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
