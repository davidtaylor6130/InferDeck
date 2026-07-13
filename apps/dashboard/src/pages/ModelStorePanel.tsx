import React, { useCallback, useEffect, useState } from 'react';
import {
  controlStoreDownload, getStoreActivity, inspectStoreModel, installStoreModel,
  removeStoreModel, searchStore, type StoreDownload, type StoreFile, type StoreModel,
} from '../api';
import { Badge, Button, EmptyState, Panel, ProgressBar, SectionTitle } from '../components/ui';
import { formatBytes, formatTokenCount } from '../utils';

const inputClass = 'h-9 rounded-md border border-white/10 bg-[#07101d] px-2 text-sm text-text-primary';

export const ModelStorePanel: React.FC = () => {
  const [query, setQuery] = useState('');
  const [runtime, setRuntime] = useState('');
  const [results, setResults] = useState<StoreModel[]>([]);
  const [files, setFiles] = useState<StoreFile[]>([]);
  const [selectedRepo, setSelectedRepo] = useState('');
  const [downloads, setDownloads] = useState<StoreDownload[]>([]);
  const [installed, setInstalled] = useState<string[]>([]);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');

  const refresh = useCallback(async () => {
    try {
      const activity = await getStoreActivity();
      setDownloads(activity.downloads);
      setInstalled(Object.keys(activity.installed));
    } catch {}
  }, []);

  useEffect(() => {
    void refresh();
    const timer = setInterval(() => { void refresh(); }, 1500);
    return () => clearInterval(timer);
  }, [refresh]);

  const search = async () => {
    setBusy(true);
    setError('');
    setFiles([]);
    try {
      setResults(await searchStore(query, runtime));
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  };

  const inspect = async (repo: string) => {
    setBusy(true);
    setError('');
    setSelectedRepo(repo);
    try {
      setFiles(await inspectStoreModel(repo));
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  };

  const install = async (file: StoreFile) => {
    const fallback = `${file.repo.split('/').pop() || 'model'}-${file.name.replace(/\.[^.]+$/, '').slice(-40)}`
      .replace(/[^A-Za-z0-9_.-]/g, '_');
    const modelName = window.prompt('Registered model name', fallback);
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
    if (!window.confirm(`Remove ${model} and its installed artifact?`)) return;
    setError('');
    try {
      await removeStoreModel(model);
      await refresh();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    }
  };

  return (
    <Panel>
      <SectionTitle title="Model store" aside="Hugging Face" />
      <div className="mt-3 flex flex-wrap gap-2">
        <input
          aria-label="Search model store"
          className={`${inputClass} min-w-[240px] flex-1`}
          placeholder="Search compatible models"
          value={query}
          onChange={event => setQuery(event.target.value)}
          onKeyDown={event => { if (event.key === 'Enter') void search(); }}
        />
        <select className={inputClass} value={runtime} onChange={event => setRuntime(event.target.value)}>
          <option value="">All runtimes</option>
          <option value="llama_cpp">llama.cpp</option>
          <option value="stable_diffusion_cpp">stable-diffusion.cpp</option>
          <option value="whisper_cpp">whisper.cpp</option>
          <option value="tts_native">Native TTS</option>
        </select>
        <Button tone="blue" disabled={busy || !query.trim()} onClick={() => { void search(); }}>Search</Button>
      </div>
      {error && <p className="mt-2 text-xs text-danger-rose">{error}</p>}

      {results.length > 0 && (
        <div className="mt-3 grid gap-2 lg:grid-cols-2">
          {results.map(model => (
            <button key={model.id} type="button" onClick={() => { void inspect(model.id); }}
              className="rounded-md border border-white/10 bg-[#07101d] p-3 text-left hover:border-queue-blue/40">
              <div className="flex items-center justify-between gap-2">
                <span className="truncate font-mono text-sm text-text-primary">{model.id}</span>
                <Badge label={model.modality || 'text'} tone={selectedRepo === model.id ? 'info' : 'idle'} />
              </div>
              <p className="mt-1 text-xs text-text-muted">{model.runtime} · {formatTokenCount(model.downloads)} downloads · {model.likes} likes</p>
            </button>
          ))}
        </div>
      )}

      {selectedRepo && (
        <div className="mt-4">
          <h3 className="text-sm font-semibold text-text-primary">Compatible artifacts in {selectedRepo}</h3>
          {files.length === 0 ? <div className="mt-2"><EmptyState title={busy ? 'Loading artifacts…' : 'No compatible artifacts found'} /></div> : (
            <div className="mt-2 overflow-x-auto">
              <table className="w-full min-w-[720px] text-left text-sm">
                <thead><tr className="border-b border-white/10 text-xs text-text-muted"><th className="py-2 pr-3">File</th><th className="pr-3">Runtime</th><th className="pr-3">Size</th><th className="pr-3">Estimated VRAM</th><th></th></tr></thead>
                <tbody className="divide-y divide-white/5">{files.map(file => (
                  <tr key={file.name}>
                    <td className="max-w-[380px] truncate py-2 pr-3 font-mono text-xs text-text-primary" title={file.name}>{file.name}</td>
                    <td className="pr-3 text-text-secondary">{file.runtime} · {file.modality} · {file.quantization}</td>
                    <td className="pr-3 text-text-secondary">{formatBytes(file.size)}</td>
                    <td className="pr-3 text-text-secondary">{file.estimatedVramMb.toLocaleString()} MB</td>
                    <td className="text-right"><Button tone="blue" onClick={() => { void install(file); }}>Install</Button></td>
                  </tr>
                ))}</tbody>
              </table>
            </div>
          )}
        </div>
      )}

      {(downloads.length > 0 || installed.length > 0) && (
        <div className="mt-4 grid gap-4 xl:grid-cols-2">
          <div>
            <h3 className="text-sm font-semibold text-text-primary">Download activity</h3>
            <div className="mt-2 space-y-2">{downloads.map(download => {
              const percent = download.bytesTotal ? download.bytesDownloaded / download.bytesTotal * 100 : 0;
              return (
                <div key={download.id} className="rounded-md border border-white/10 bg-[#07101d] p-3">
                  <div className="flex items-center justify-between gap-2"><span className="truncate text-xs text-text-primary">{download.modelName}</span><Badge label={download.state} tone={download.state === 'installed' ? 'good' : download.state === 'failed' ? 'critical' : 'info'} /></div>
                  <div className="mt-2"><ProgressBar percent={percent} tone={download.state === 'failed' ? 'critical' : 'info'} /></div>
                  <div className="mt-2 flex items-center justify-between gap-2 text-xs text-text-muted">
                    <span>{formatBytes(download.bytesDownloaded)} / {formatBytes(download.bytesTotal)}{download.error ? ` · ${download.error}` : ''}</span>
                    {download.state === 'downloading' || download.state === 'queued'
                      ? <Button tone="danger" onClick={() => { void control(download.id, 'cancel'); }}>Cancel</Button>
                      : download.state === 'failed' || download.state === 'cancelled'
                        ? <Button onClick={() => { void control(download.id, 'resume'); }}>Resume</Button> : null}
                  </div>
                </div>
              );
            })}</div>
          </div>
          <div>
            <h3 className="text-sm font-semibold text-text-primary">Store-managed models</h3>
            <div className="mt-2 space-y-2">{installed.map(model => (
              <div key={model} className="flex items-center justify-between gap-2 rounded-md border border-white/10 bg-[#07101d] p-3">
                <span className="truncate font-mono text-xs text-text-primary">{model}</span>
                <Button tone="danger" onClick={() => { void remove(model); }}>Remove</Button>
              </div>
            ))}</div>
          </div>
        </div>
      )}
    </Panel>
  );
};
