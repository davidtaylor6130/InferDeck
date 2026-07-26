import React, { useEffect, useMemo, useState } from 'react';
import { parseDocument } from 'yaml';
import {
  getConfig, optimizeProfile, saveActiveConfig, waitForActiveConfig,
  type ConfigDocument, type ProfileOptimizationResult,
} from '../api';
import { Badge, Button, EmptyState, Panel, SectionTitle, Stat } from '../components/ui';
import {
  modalityLabel,
  modelsForSection,
  sectionLabel,
  usageForSection,
  type DashboardSection,
} from '../dashboardSections';
import { useGateway } from '../gateway';
import type { ModelInfo } from '../types';
import { compactModel, formatDuration, formatMb, formatTokenCount, timeAgo } from '../utils';
import { MediaJobsPanel } from './MediaJobsPanel';

const inputClass = 'h-9 w-full rounded border border-white/10 bg-[#07101d] px-2 text-sm text-text-primary';
type ConfigValue = string | number | boolean | null;

export const OperatePage: React.FC<{ section: DashboardSection }> = ({ section }) => {
  const { models, status, stats, swap, swapTo, unload } = useGateway();
  const scopedModels = useMemo(() => modelsForSection(models, section), [models, section]);
  const usage = useMemo(
    () => usageForSection(status?.tokenUsage ?? [], models, section),
    [status?.tokenUsage, models, section],
  );
  const loaded = scopedModels.filter(model => model.loaded);
  const requests = usage.reduce((sum, row) => sum + row.requests, 0);
  const successful = usage.reduce((sum, row) => sum + row.successfulRequests, 0);
  const promptTokens = usage.reduce((sum, row) => sum + row.promptTokens, 0);
  const completionTokens = usage.reduce((sum, row) => sum + row.completionTokens, 0);
  const lastUsed = Math.max(0, ...usage.map(row => row.lastTimestampUnixMs));
  const [pending, setPending] = useState('');
  const [error, setError] = useState('');
  const [editing, setEditing] = useState<ModelInfo | null>(null);

  const load = async (model: string) => {
    setPending(`load:${model}`);
    setError('');
    const failure = await swapTo(model);
    if (failure) setError(failure);
    setPending('');
  };

  const unloadModel = async (model: string) => {
    setPending(`unload:${model}`);
    setError('');
    const failure = await unload(model);
    if (failure) setError(failure);
    setPending('');
  };

  return (
    <div className="space-y-4">
      {error && (
        <div className="border-l-2 border-danger-rose bg-danger-rose/10 px-4 py-2 text-sm text-danger-rose" role="alert">
          {error}
        </div>
      )}

      <Panel>
        <SectionTitle
          title={`${sectionLabel(section)} operation`}
          aside={section === 'dictation' ? 'runtime control' : `${loaded.length} loaded`}
        />
        <p className="mt-2 max-w-3xl text-sm text-text-secondary">
          {section === 'dictation'
            ? 'Control backend speech services and tune their runtime profiles. Recording and playback stay in clients such as Open WebUI.'
            : 'Load, unload, and tune the models the gateway actually runs. Saving applies the active profile automatically.'}
        </p>
        <div className="mt-4 grid grid-cols-2 gap-4 sm:grid-cols-4">
          <Stat label="Configured" value={String(scopedModels.length)} />
          <Stat label="Loaded" value={String(loaded.length)} tone={loaded.length ? 'good' : 'idle'} />
          <Stat label="Lifetime requests" value={requests.toLocaleString()} />
          <Stat
            label={section === 'dictation' ? 'Successful' : 'Lifetime tokens'}
            value={section === 'dictation' ? successful.toLocaleString() : formatTokenCount(promptTokens + completionTokens)}
            sub={lastUsed ? `last used ${timeAgo(lastUsed)}` : 'no persisted use'}
          />
        </div>
      </Panel>

      <Panel>
        <SectionTitle title="Runtime models" aside="compact control plane" />
        {scopedModels.length === 0 ? (
          <div className="mt-3">
            <EmptyState
              title={`No ${sectionLabel(section)} models configured`}
              detail={`Use ${sectionLabel(section)} Models to acquire an artifact, then add it to the active profile.`}
            />
          </div>
        ) : (
          <div className="mt-3 overflow-x-auto">
            <table className="w-full min-w-[820px] text-left text-sm">
              <thead>
                <tr className="border-b border-white/10 text-xs uppercase tracking-wide text-text-muted">
                  <th className="py-2 pr-4 font-medium">Model</th>
                  <th className="py-2 pr-4 font-medium">Service</th>
                  <th className="py-2 pr-4 font-medium">State</th>
                  <th className="py-2 pr-4 font-medium">Slots</th>
                  <th className="py-2 pr-4 font-medium">{section === 'llm' ? 'Context' : 'Memory'}</th>
                  <th className="py-2 pr-4 font-medium">Requests</th>
                  <th className="py-2 font-medium">Actions</th>
                </tr>
              </thead>
              <tbody className="divide-y divide-white/5">
                {scopedModels.map(model => {
                  const modelUsage = usage.find(row => row.model === model.id);
                  const isTarget = swap.swapping && swap.target === model.id;
                  return (
                    <tr key={model.id}>
                      <td className="py-2.5 pr-4">
                        <p className="font-mono text-text-primary">{compactModel(model.id)}</p>
                        <p className="text-xs text-text-muted">{model.family || 'unknown family'}</p>
                      </td>
                      <td className="py-2.5 pr-4">
                        <Badge label={modalityLabel(model.modality)} tone={section === 'dictation' ? 'violet' : 'info'} />
                        <p className="mt-1 text-xs text-text-muted">{model.runtime || 'llama_cpp'}</p>
                      </td>
                      <td className="py-2.5 pr-4">
                        {model.runtime_available === false
                          ? <Badge label="Unavailable" tone="critical" />
                          : model.loaded
                            ? <Badge label={model.primary ? 'Primary' : 'Loaded'} tone="good" />
                            : isTarget
                              ? <Badge label="Loading" tone="info" />
                              : <Badge label="Standby" tone="idle" />}
                      </td>
                      <td className="py-2.5 pr-4 text-text-secondary">
                        {model.loaded && model.free_slots != null ? `${model.free_slots}/${model.n_slots} free` : model.n_slots}
                      </td>
                      <td className="py-2.5 pr-4 text-text-secondary">
                        {section === 'llm' ? formatTokenCount(model.context_size) : formatMb(model.vram_required_mb)}
                      </td>
                      <td className="py-2.5 pr-4 text-text-secondary">{(modelUsage?.requests ?? 0).toLocaleString()}</td>
                      <td className="py-2.5">
                        <div className="flex flex-wrap gap-2">
                          {model.runtime_available === false ? (
                            <Button disabled>Unavailable</Button>
                          ) : model.loaded ? (
                            <Button disabled={pending !== ''} onClick={() => { void unloadModel(model.id); }}>
                              {pending === `unload:${model.id}` ? 'Unloading...' : 'Unload'}
                            </Button>
                          ) : (
                            <Button tone="blue" disabled={swap.swapping || pending !== ''} onClick={() => { void load(model.id); }}>
                              {pending === `load:${model.id}` ? 'Starting...' : 'Load'}
                            </Button>
                          )}
                          <Button onClick={() => setEditing(model)}>Model details</Button>
                        </div>
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        )}
      </Panel>

      {section === 'llm' && (
        <Panel>
          <SectionTitle title="Current workload" aside={status?.queue.resourceDecision || 'shared gateway'} />
          <div className="mt-3 grid grid-cols-2 gap-4 sm:grid-cols-4">
            <Stat label="Running" value={String(status?.queue.running ?? stats?.activeRequests ?? 0)} />
            <Stat label="Queued" value={String(status?.queue.queued ?? 0)} />
            <Stat label="Request average t/s" value={(stats?.avgTokensPerSecond ?? 0).toFixed(1)} />
            <Stat label="p95 latency" value={formatDuration(status?.summary.p95LatencyMs)} />
          </div>
        </Panel>
      )}

      {section === 'dictation' && <MediaJobsPanel showEmpty />}
      {editing && <ModelConfigDialog model={editing} section={section} onClose={() => setEditing(null)} />}
    </div>
  );
};

const ModelConfigDialog: React.FC<{
  model: ModelInfo;
  section: DashboardSection;
  onClose: () => void;
}> = ({ model, section, onClose }) => {
  const { status } = useGateway();
  const [config, setConfig] = useState<ConfigDocument | null>(null);
  const [yaml, setYaml] = useState('');
  const [busy, setBusy] = useState(true);
  const [optimizing, setOptimizing] = useState(false);
  const [optimization, setOptimization] = useState<ProfileOptimizationResult | null>(null);
  const [dirty, setDirty] = useState(false);
  const [message, setMessage] = useState('');

  useEffect(() => {
    let active = true;
    getConfig().then(document => {
      if (!active) return;
      setConfig(document);
      setYaml(document.activeYaml || document.yaml);
      setBusy(false);
    }).catch(error => {
      if (!active) return;
      setMessage(error instanceof Error ? error.message : String(error));
      setBusy(false);
    });
    return () => { active = false; };
  }, []);

  useEffect(() => {
    const close = (event: KeyboardEvent) => { if (event.key === 'Escape') onClose(); };
    window.addEventListener('keydown', close);
    return () => window.removeEventListener('keydown', close);
  }, [onClose]);

  const modelIndex = (text: string) => {
    try {
      const registry = (parseDocument(text).toJS() as { model_registry?: unknown[] }).model_registry;
      return Array.isArray(registry)
        ? registry.findIndex(entry => entry && typeof entry === 'object' && (entry as { name?: string }).name === model.id)
        : -1;
    } catch {
      return -1;
    }
  };
  const index = modelIndex(yaml);

  const read = (path: Array<string | number>): ConfigValue | undefined => {
    if (index < 0) return undefined;
    try {
      const value = parseDocument(yaml).getIn(['model_registry', index, ...path]);
      return typeof value === 'string' || typeof value === 'number' || typeof value === 'boolean' || value === null
        ? value : undefined;
    } catch {
      return undefined;
    }
  };

  const readRoot = (path: Array<string | number>): ConfigValue | undefined => {
    try {
      const value = parseDocument(yaml).getIn(path);
      return typeof value === 'string' || typeof value === 'number' || typeof value === 'boolean' || value === null
        ? value : undefined;
    } catch {
      return undefined;
    }
  };

  const update = (path: Array<string | number>, value: ConfigValue) => {
    if (index < 0) return;
    try {
      const document = parseDocument(yaml);
      document.setIn(['model_registry', index, ...path], value);
      setYaml(document.toString());
      setDirty(true);
      setMessage('');
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    }
  };

  const updateRoot = (path: Array<string | number>, value: ConfigValue) => {
    try {
      const document = parseDocument(yaml);
      document.setIn(path, value);
      setYaml(document.toString());
      setDirty(true);
      setMessage('');
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    }
  };

  const resetModel = () => {
    if (!config || index < 0) return;
    try {
      const base = parseDocument(config.yaml);
      const active = parseDocument(yaml);
      const baseRegistry = (base.toJS() as { model_registry?: unknown[] }).model_registry;
      const baseEntry = Array.isArray(baseRegistry)
        ? baseRegistry.find(entry => entry && typeof entry === 'object' && (entry as { name?: string }).name === model.id)
        : undefined;
      if (!baseEntry) {
        setMessage('This model does not exist in the stable baseline.');
        return;
      }
      active.setIn(['model_registry', index], baseEntry);
      setYaml(active.toString());
      setDirty(true);
      setMessage('Model settings restored from the stable baseline. Save to apply the reset.');
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    }
  };

  const analyzeProfile = async () => {
    setOptimizing(true);
    setOptimization(null);
    setMessage('');
    try {
      const result = await optimizeProfile({
        model: model.id,
        contextPerSlot: Number(read(['context_size']) ?? model.context_size),
        slots: Number(read(['n_slots']) ?? model.n_slots),
        minSlots: Number(read(['min_slots']) ?? 1),
        nBatch: Number(readRoot(['gateway', 'n_batch']) ?? 512),
        nUbatch: Number(readRoot(['gateway', 'n_ubatch']) ?? 512),
        cacheTypeK: String(readRoot(['gateway', 'cache_type_k']) ?? 'q8_0'),
        cacheTypeV: String(readRoot(['gateway', 'cache_type_v']) ?? 'q8_0'),
      });
      setOptimization(result);
      setMessage('Profile analysis complete. Review the estimate before applying it to this draft.');
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    } finally {
      setOptimizing(false);
    }
  };

  const applyOptimization = () => {
    if (!optimization || index < 0) return;
    try {
      const candidate = optimization.recommended;
      const document = parseDocument(yaml);
      document.setIn(['model_registry', index, 'context_size'], candidate.contextPerSlot);
      document.setIn(['model_registry', index, 'n_slots'], candidate.slots);
      document.setIn(['gateway', 'cache_type_k'], candidate.cacheTypeK);
      document.setIn(['gateway', 'cache_type_v'], candidate.cacheTypeV);
      document.setIn(['gateway', 'n_batch'], candidate.nBatch);
      document.setIn(['gateway', 'n_ubatch'], candidate.nUbatch);
      document.setIn(['gateway', 'flash_attn'], candidate.flashAttention);
      setYaml(document.toString());
      setDirty(true);
      setMessage('Recommendation staged in this draft. Save active profile to validate and hot apply it.');
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    }
  };

  const save = async () => {
    if (!config) return;
    setBusy(true);
    setMessage('');
    try {
      const result = await saveActiveConfig(yaml, config.activeRevision || config.revision);
      setDirty(false);
      setMessage('Profile saved. InferDeck is applying it now; the dashboard will reconnect automatically.');
      const applied = await waitForActiveConfig(result.activeRevision);
      setConfig(applied);
      setMessage('Active profile applied. InferDeck is back online with these settings.');
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="fixed inset-0 z-50 flex items-start justify-center overflow-y-auto bg-black/75 p-4 sm:p-8" role="dialog" aria-modal="true" aria-label={`${model.id} model details`}>
      <div className="w-full max-w-5xl border border-border-slate bg-panel-slate shadow-2xl">
        <header className="flex items-start justify-between gap-4 border-b border-border-slate p-4">
          <div>
            <p className="text-xs uppercase tracking-wide text-text-muted">{sectionLabel(section)} active profile</p>
            <h2 className="mt-1 font-mono text-base font-semibold text-text-primary">{model.id}</h2>
            <p className="mt-1 text-xs text-text-muted">Changes are validated and saved separately from the stable gateway.yml baseline.</p>
          </div>
          <Button onClick={onClose}>Close</Button>
        </header>

        <div className="p-4">
          {busy && !config ? (
            <p className="py-8 text-center text-sm text-text-muted">Loading configuration...</p>
          ) : index < 0 ? (
            <EmptyState title="Model not found in the active configuration" detail="Reload the gateway configuration and try again." />
          ) : (
            <>
              <div className="grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
                <ConfigField label="Slots">
                  <input className={inputClass} type="number" min="1" value={Number(read(['n_slots']) ?? model.n_slots)} onChange={event => update(['n_slots'], Number(event.target.value))} />
                </ConfigField>
                <ConfigField label="Minimum slots">
                  <input className={inputClass} type="number" min="1" value={Number(read(['min_slots']) ?? 1)} onChange={event => update(['min_slots'], Number(event.target.value))} />
                </ConfigField>
                <ConfigField label="Context tokens">
                  <input className={inputClass} type="number" min="1" disabled={section === 'dictation'} value={Number(read(['context_size']) ?? model.context_size)} onChange={event => update(['context_size'], Number(event.target.value))} />
                </ConfigField>
                <ConfigField label="VRAM budget (MB)">
                  <input className={inputClass} type="number" min="0" value={Number(read(['vram_required_mb']) ?? model.vram_required_mb)} onChange={event => update(['vram_required_mb'], Number(event.target.value))} />
                </ConfigField>
                <ConfigField label="GPU layers (-1 = all)">
                  <input className={inputClass} type="number" min="-1" disabled={section === 'dictation'} value={Number(read(['n_gpu_layers']) ?? -1)} onChange={event => update(['n_gpu_layers'], Number(event.target.value))} />
                </ConfigField>
                <ConfigField label="Temperature">
                  <input className={inputClass} type="number" min="0" max="2" step="0.05" disabled={section === 'dictation'} value={Number(read(['sampling', 'temperature']) ?? 0.7)} onChange={event => update(['sampling', 'temperature'], Number(event.target.value))} />
                </ConfigField>
                <ConfigField label="Top P">
                  <input className={inputClass} type="number" min="0" max="1" step="0.01" disabled={section === 'dictation'} value={Number(read(['sampling', 'top_p']) ?? 0.95)} onChange={event => update(['sampling', 'top_p'], Number(event.target.value))} />
                </ConfigField>
                <ConfigField label="Repeat penalty">
                  <input className={inputClass} type="number" min="0.01" step="0.01" disabled={section === 'dictation'} value={Number(read(['sampling', 'repeat_penalty']) ?? 1)} onChange={event => update(['sampling', 'repeat_penalty'], Number(event.target.value))} />
                </ConfigField>
              </div>

              {section === 'llm' && (
                <div className="mt-4 border-t border-border-slate pt-3">
                  <h3 className="text-sm font-medium text-text-secondary">Shared runtime memory controls</h3>
                  <p className="mt-1 text-xs text-text-muted">
                    KV-cache precision controls context memory, not the model file quantization. Q8 is the quality-first default; Q4 saves VRAM for longer context or more parallel slots.
                  </p>
                  <div className="mt-3 grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
                    <ConfigField label="KV cache keys">
                      <select
                        className={inputClass}
                        value={String(readRoot(['gateway', 'cache_type_k']) ?? 'q8_0')}
                        onChange={event => updateRoot(['gateway', 'cache_type_k'], event.target.value)}
                      >
                        <option value="q4_0">Q4 · maximum headroom</option>
                        <option value="q8_0">Q8 · quality-first</option>
                        <option value="f16">F16 · maximum precision</option>
                      </select>
                    </ConfigField>
                    <ConfigField label="KV cache values">
                      <select
                        className={inputClass}
                        value={String(readRoot(['gateway', 'cache_type_v']) ?? 'q8_0')}
                        onChange={event => updateRoot(['gateway', 'cache_type_v'], event.target.value)}
                      >
                        <option value="q4_0">Q4 · maximum headroom</option>
                        <option value="q8_0">Q8 · quality-first</option>
                        <option value="f16">F16 · maximum precision</option>
                      </select>
                    </ConfigField>
                    <ConfigField label="Prompt batch">
                      <input className={inputClass} type="number" min="1" value={Number(readRoot(['gateway', 'n_batch']) ?? 512)} onChange={event => updateRoot(['gateway', 'n_batch'], Number(event.target.value))} />
                    </ConfigField>
                    <ConfigField label="Physical batch">
                      <input className={inputClass} type="number" min="1" value={Number(readRoot(['gateway', 'n_ubatch']) ?? 512)} onChange={event => updateRoot(['gateway', 'n_ubatch'], Number(event.target.value))} />
                    </ConfigField>
                    <ConfigField label="Flash attention">
                      <select
                        className={inputClass}
                        value={String(readRoot(['gateway', 'flash_attn']) ?? 'auto')}
                        onChange={event => updateRoot(['gateway', 'flash_attn'], event.target.value)}
                      >
                        <option value="auto">Auto</option>
                        <option value="on">On</option>
                        <option value="off">Off</option>
                      </select>
                    </ConfigField>
                  </div>
                  <p className="mt-2 text-xs text-text-muted">
                    Seed stays a per-request control and defaults to random. A future measured quality benchmark will use a fixed seed internally without making normal responses deterministic.
                  </p>

                  <div className="mt-4 border border-white/10 bg-[#07101d] p-3">
                    <div className="flex flex-wrap items-start justify-between gap-3">
                      <div className="max-w-2xl">
                        <h3 className="text-sm font-medium text-text-primary">Quality-first profile analysis</h3>
                        <p className="mt-1 text-xs text-text-muted">
                          Scores safe candidate profiles from this model's real artifact size, per-slot context, slots, GPU capacity, and persisted throughput. It does not run inference, reload the model, change the GGUF quantization, or save anything automatically.
                        </p>
                      </div>
                      <Button
                        tone="blue"
                        disabled={optimizing || busy || index < 0}
                        onClick={() => { void analyzeProfile(); }}
                      >
                        {optimizing ? 'Analysing...' : 'Analyse profile'}
                      </Button>
                    </div>
                    {(status?.queue.running ?? 0) > 0 || (status?.queue.queued ?? 0) > 0 ? (
                      <p className="mt-2 text-xs text-warning-amber">
                        Safety gate: analysis will wait until active and queued requests are zero.
                      </p>
                    ) : null}
                    {optimization && (
                      <div className="mt-3 border-t border-white/10 pt-3">
                        <div className="grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
                          <Stat label="Quality score" value={`${Math.round(optimization.recommended.qualityScore * 100)}%`} tone="good" />
                          <Stat label="Overall score" value={`${Math.round(optimization.recommended.overallScore * 100)}%`} />
                          <Stat label="Estimated VRAM" value={formatMb(optimization.recommended.estimatedVramMb)} />
                          <Stat label="Estimated reserve" value={formatMb(optimization.recommended.reserveVramMb)} tone={optimization.recommended.fits ? 'good' : 'critical'} />
                        </div>
                        <p className="mt-3 text-sm text-text-secondary">
                          Recommend {formatTokenCount(optimization.recommended.contextPerSlot)} context per slot,
                          {' '}{optimization.recommended.slots} slot(s),
                          {' '}{optimization.recommended.cacheTypeK}/{optimization.recommended.cacheTypeV} KV,
                          {' '}batch {optimization.recommended.nBatch}/{optimization.recommended.nUbatch}.
                        </p>
                        <p className="mt-1 text-xs text-text-muted">
                          Estimate only, not a measured quality benchmark. Quality carries {Math.round(optimization.weights.quality * 100)}% of the score.
                          {optimization.observedTokensPerSecond > 0
                            ? ` Persisted baseline: ${optimization.observedTokensPerSecond.toFixed(1)} output t/s.`
                            : ' No persisted throughput baseline exists for this model yet.'}
                        </p>
                        <div className="mt-3 flex flex-wrap items-center gap-2">
                          <Button onClick={applyOptimization}>Stage recommendation</Button>
                          <Badge label={`${optimization.candidates.length} safe candidates compared`} tone="info" />
                        </div>
                      </div>
                    )}
                  </div>
                </div>
              )}

              <details className="mt-4 border-t border-border-slate pt-3">
                <summary className="cursor-pointer text-sm font-medium text-text-secondary">Advanced active YAML</summary>
                <p className="mt-2 text-xs text-text-muted">Full control is available here for runtime artifacts, sampling, memory, and any setting not exposed above.</p>
                <textarea
                  aria-label="Advanced active YAML"
                  spellCheck={false}
                  className="mt-2 h-[320px] w-full resize-y rounded border border-border-slate bg-[#05080f] p-3 font-mono text-xs leading-5 text-text-secondary"
                  value={yaml}
                  onChange={event => { setYaml(event.target.value); setDirty(true); setMessage(''); }}
                />
              </details>
            </>
          )}

          <div className="mt-4 flex flex-wrap items-center gap-2 border-t border-border-slate pt-4">
            <Button tone="blue" disabled={busy || !dirty || index < 0} onClick={() => { void save(); }}>Save active profile</Button>
            <Button disabled={busy || index < 0} onClick={resetModel}>Restore model baseline</Button>
            {config?.hasActiveProfile && <Badge label={config.usingActiveProfile ? 'Active profile running' : 'Active profile saved'} tone={config.usingActiveProfile ? 'good' : 'warn'} />}
            {message && <span className="text-xs text-text-secondary" role="status">{message}</span>}
          </div>
        </div>
      </div>
    </div>
  );
};

const ConfigField: React.FC<{ label: string; children: React.ReactNode }> = ({ label, children }) => (
  <label className="text-xs text-text-muted">{label}<span className="mt-1 block">{children}</span></label>
);
