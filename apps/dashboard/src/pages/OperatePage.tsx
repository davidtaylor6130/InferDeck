import React, { useEffect, useMemo, useState } from 'react';
import { Cog6ToothIcon, PlayIcon, StopIcon, XMarkIcon } from '@heroicons/react/24/outline';
import { parseDocument } from 'yaml';
import {
  cancelProfileBenchmark, getConfig, getOptimizationSchedule, getProfileBenchmark, saveActiveConfig,
  startProfileBenchmark, waitForActiveConfig,
  type ConfigDocument, type ProfileBenchmarkSnapshot, type ProfileOptimizationCandidate,
  type ScheduledOptimizationRecord,
} from '../api';
import { Badge, Button, EmptyState, IconButton, Panel, SectionTitle, Stat } from '../components/ui';
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
import { ModelAliasPanel } from './ModelAliasPanel';

const inputClass = 'h-9 w-full rounded border border-white/10 bg-[#07101d] px-2 text-sm text-text-primary';
type ConfigValue = string | number | boolean | null;
type EditingModel = { model: ModelInfo; autoOptimize: boolean };

export function stageProfileOptimization(
  yaml: string,
  modelId: string,
  candidate: ProfileOptimizationCandidate,
): string {
  const document = parseDocument(yaml);
  const registry = (document.toJS() as { model_registry?: unknown[] }).model_registry;
  const index = Array.isArray(registry)
    ? registry.findIndex(entry =>
        entry && typeof entry === 'object' &&
        (entry as { name?: string }).name === modelId)
    : -1;
  if (index < 0) throw new Error(`Model ${modelId} is not present in the active profile.`);
  document.setIn(['model_registry', index, 'context_size'], candidate.contextPerSlot);
  document.setIn(['model_registry', index, 'n_slots'], candidate.slots);
  document.setIn(['model_registry', index, 'cache_type_k'], candidate.cacheTypeK);
  document.setIn(['model_registry', index, 'cache_type_v'], candidate.cacheTypeV);
  document.setIn(['model_registry', index, 'n_batch'], candidate.nBatch);
  document.setIn(['model_registry', index, 'n_ubatch'], candidate.nUbatch);
  document.setIn(['model_registry', index, 'speculative', 'max_active_requests'], candidate.mtpMaxActiveRequests);
  document.setIn(['gateway', 'flash_attn'], candidate.flashAttention);
  return document.toString();
}

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
  const [editing, setEditing] = useState<EditingModel | null>(null);

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
          title={`${sectionLabel(section)} Model Settings`}
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

      <ModelAliasPanel section={section} />

      <Panel>
        <SectionTitle title="Runtime models" aside="compact control plane" />
        {scopedModels.length === 0 ? (
          <div className="mt-3">
            <EmptyState
              title={`No ${sectionLabel(section)} models configured`}
              detail={`Use the ${sectionLabel(section)} Model Store to acquire an artifact, then add it to the active profile.`}
            />
          </div>
        ) : (
          <div className="mt-3 overflow-x-auto" role="region" aria-label="Runtime models" tabIndex={0}>
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
                        {model.optimization?.status === 'measured' && (
                          <div className="mt-1">
                            <Badge label="Measured optimized" tone="good" />
                          </div>
                        )}
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
                            <IconButton label={pending === `unload:${model.id}` ? `Unloading ${model.id}` : `Unload ${model.id}`} disabled={pending !== ''} onClick={() => { void unloadModel(model.id); }}>
                              <StopIcon className="h-4 w-4" aria-hidden="true" />
                            </IconButton>
                          ) : (
                            <IconButton label={pending === `load:${model.id}` ? `Loading ${model.id}` : `Load ${model.id}`} tone="blue" disabled={swap.swapping || pending !== ''} onClick={() => { void load(model.id); }}>
                              <PlayIcon className="h-4 w-4" aria-hidden="true" />
                            </IconButton>
                          )}
                          {section === 'llm' && (
                            <Button
                              tone={model.optimization?.status === 'measured' ? 'green' : 'blue'}
                              onClick={() => setEditing({ model, autoOptimize: true })}
                            >
                              Auto-optimize
                            </Button>
                          )}
                          <IconButton label={`Model settings for ${model.id}`} onClick={() => setEditing({ model, autoOptimize: false })}>
                            <Cog6ToothIcon className="h-4 w-4" aria-hidden="true" />
                          </IconButton>
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
      {editing && (
        <ModelConfigDialog
          model={editing.model}
          section={section}
          autoOptimize={editing.autoOptimize}
          onClose={() => setEditing(null)}
        />
      )}
    </div>
  );
};

const ModelConfigDialog: React.FC<{
  model: ModelInfo;
  section: DashboardSection;
  autoOptimize: boolean;
  onClose: () => void;
}> = ({ model, section, autoOptimize, onClose }) => {
  const { status } = useGateway();
  const [config, setConfig] = useState<ConfigDocument | null>(null);
  const [yaml, setYaml] = useState('');
  const [busy, setBusy] = useState(true);
  const [optimizing, setOptimizing] = useState(false);
  const [benchmark, setBenchmark] = useState<ProfileBenchmarkSnapshot | null>(null);
  const [dirty, setDirty] = useState(false);
  const [message, setMessage] = useState('');
  const [autoStarted, setAutoStarted] = useState(false);
  const [scheduleStatus, setScheduleStatus] = useState<ScheduledOptimizationRecord | null>(null);
  const [scheduleTimezone, setScheduleTimezone] = useState('server local time');

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

  useEffect(() => {
    let active = true;
    getProfileBenchmark().then(current => {
      if (active && current.model === model.id && current.state !== 'idle') {
        setBenchmark(current);
      }
    }).catch(() => {});
    return () => { active = false; };
  }, [model.id]);

  useEffect(() => {
    let active = true;
    getOptimizationSchedule().then(result => {
      if (!active) return;
      setScheduleTimezone(result.timezone || 'server local time');
      setScheduleStatus(result.schedules.find(schedule => schedule.model === model.id) ?? null);
    }).catch(() => {});
    return () => { active = false; };
  }, [model.id]);

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

  const stageCandidate = (candidate: ProfileOptimizationCandidate) => {
    setYaml(current => stageProfileOptimization(current, model.id, candidate));
    setDirty(true);
  };

  const analyzeProfile = async () => {
    setOptimizing(true);
    setBenchmark(null);
    setMessage('');
    try {
      const result = await startProfileBenchmark({
        model: model.id,
        contextPerSlot: Number(read(['context_size']) ?? model.context_size),
        slots: Number(read(['n_slots']) ?? model.n_slots),
        minSlots: Number(read(['min_slots']) ?? 1),
        nBatch: Number(read(['n_batch']) ?? readRoot(['gateway', 'n_batch']) ?? 512),
        nUbatch: Number(read(['n_ubatch']) ?? readRoot(['gateway', 'n_ubatch']) ?? 512),
        cacheTypeK: String(read(['cache_type_k']) ?? readRoot(['gateway', 'cache_type_k']) ?? 'q8_0'),
        cacheTypeV: String(read(['cache_type_v']) ?? readRoot(['gateway', 'cache_type_v']) ?? 'q8_0'),
        flashAttention: String(readRoot(['gateway', 'flash_attn']) ?? 'auto'),
        candidateLimit: 3,
      });
      setBenchmark(result);
      setMessage('Measured benchmark started. GPU model requests are paused while the accelerator is measured; CPU dictation remains available.');
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    } finally {
      setOptimizing(false);
    }
  };

  const applyOptimization = () => {
    if (!benchmark?.recommended) return;
    try {
      stageCandidate(benchmark.recommended);
      setMessage('Recommendation staged in this draft. Save active profile to validate and hot apply it.');
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    }
  };

  const discardOptimization = () => {
    setBenchmark(null);
    setMessage('Measured recommendation discarded. The active profile is unchanged.');
  };

  const cancelBenchmark = async () => {
    try {
      setBenchmark(await cancelProfileBenchmark());
      setMessage('Cancellation requested. InferDeck will restore the previous model residency.');
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    }
  };

  useEffect(() => {
    if (!benchmark || (benchmark.state !== 'running' && benchmark.state !== 'cancelling')) return;
    let active = true;
    const timer = globalThis.setTimeout(() => {
      getProfileBenchmark().then(current => {
        if (active) setBenchmark(current);
      }).catch(error => {
        if (active) setMessage(error instanceof Error ? error.message : String(error));
      });
    }, 750);
    return () => {
      active = false;
      globalThis.clearTimeout(timer);
    };
  }, [benchmark]);

  useEffect(() => {
    if (!autoOptimize || autoStarted || !config || busy || index < 0) return;
    setAutoStarted(true);
    void analyzeProfile();
  // The direct action is deliberately one-shot for each opened dialog.
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [autoOptimize, autoStarted, config, busy, index]);

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

  const benchmarkRunning =
    benchmark?.state === 'running' || benchmark?.state === 'cancelling';
  const winnerTrial = benchmark?.recommended
    ? benchmark.candidates.find(candidate =>
        candidate.contextPerSlot === benchmark.recommended?.contextPerSlot &&
        candidate.slots === benchmark.recommended?.slots &&
        candidate.nBatch === benchmark.recommended?.nBatch &&
        candidate.cacheTypeK === benchmark.recommended?.cacheTypeK &&
        candidate.cacheTypeV === benchmark.recommended?.cacheTypeV &&
        candidate.mtpMaxActiveRequests === benchmark.recommended?.mtpMaxActiveRequests)
    : undefined;
  const baselineTrial = benchmark?.baseline?.completed ? benchmark.baseline : undefined;
  const relativeChange = (before: number | undefined, after: number | undefined) => {
    if (before == null || after == null || before === 0) return 'n/a';
    const change = (after - before) / before * 100;
    return `${change >= 0 ? '+' : ''}${change.toFixed(1)}%`;
  };
  const changeTone = (before: number | undefined, after: number | undefined, lowerIsBetter = false) => {
    if (before == null || after == null || before === after) return 'text-text-secondary';
    const improved = lowerIsBetter ? after < before : after > before;
    return improved ? 'text-success-green' : 'text-danger-rose';
  };
  const performanceIndex = winnerTrial?.performanceIndex ?? 100;
  const performanceTone = performanceIndex > 100 ? 'good' : performanceIndex < 100 ? 'critical' : 'idle';
  const correctnessChange = baselineTrial && winnerTrial
    ? winnerTrial.qualityScore >= baselineTrial.qualityScore ? 'Preserved' : 'Regressed'
    : 'n/a';

  return (
    <div className="fixed inset-0 z-50 flex items-start justify-center overflow-y-auto bg-black/75 p-4 sm:p-8" role="dialog" aria-modal="true" aria-label={`${model.id} model details`}>
      <div className="w-full max-w-5xl border border-border-slate bg-panel-slate shadow-2xl">
        <header className="flex items-start justify-between gap-4 border-b border-border-slate p-4">
          <div>
            <p className="text-xs uppercase tracking-wide text-text-muted">{sectionLabel(section)} active profile</p>
            <h2 className="mt-1 font-mono text-base font-semibold text-text-primary">{model.id}</h2>
            <p className="mt-1 text-xs text-text-muted">Changes are validated and saved separately from the stable gateway.yml baseline.</p>
          </div>
          <IconButton label="Close model settings" onClick={onClose}>
            <XMarkIcon className="h-5 w-5" aria-hidden="true" />
          </IconButton>
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
                {section === 'llm' && (
                  <>
                    <ConfigField label="Input / 1M tokens (USD)">
                      <input className={inputClass} type="number" min="0" step="0.001" value={Number(read(['prompt_price_per_million']) ?? 0)} onChange={event => update(['prompt_price_per_million'], Number(event.target.value))} />
                    </ConfigField>
                    <ConfigField label="Cached input / 1M tokens (USD)">
                      <input className={inputClass} type="number" min="0" step="0.001" value={Number(read(['cached_prompt_price_per_million']) ?? read(['prompt_price_per_million']) ?? 0)} onChange={event => update(['cached_prompt_price_per_million'], Number(event.target.value))} />
                    </ConfigField>
                    <ConfigField label="Output / 1M tokens (USD)">
                      <input className={inputClass} type="number" min="0" step="0.001" value={Number(read(['completion_price_per_million']) ?? 0)} onChange={event => update(['completion_price_per_million'], Number(event.target.value))} />
                    </ConfigField>
                  </>
                )}
              </div>

              {section === 'llm' && (
                <div className="mt-4 border-t border-border-slate pt-3">
                  <h3 className="text-sm font-medium text-text-secondary">Model runtime and adaptive MTP</h3>
                  <p className="mt-1 text-xs text-text-muted">
                    These values apply to this model. Adaptive MTP accelerates a single request and automatically returns to ordinary continuous batching when concurrency exceeds its configured window.
                  </p>
                  <div className="mt-3 grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
                    <ConfigField label="KV cache keys">
                      <select
                        className={inputClass}
                        value={String(read(['cache_type_k']) ?? readRoot(['gateway', 'cache_type_k']) ?? 'q8_0')}
                        onChange={event => update(['cache_type_k'], event.target.value)}
                      >
                        <option value="q4_0">Q4 · maximum headroom</option>
                        <option value="q8_0">Q8 · quality-first</option>
                        <option value="f16">F16 · maximum precision</option>
                      </select>
                    </ConfigField>
                    <ConfigField label="KV cache values">
                      <select
                        className={inputClass}
                        value={String(read(['cache_type_v']) ?? readRoot(['gateway', 'cache_type_v']) ?? 'q8_0')}
                        onChange={event => update(['cache_type_v'], event.target.value)}
                      >
                        <option value="q4_0">Q4 · maximum headroom</option>
                        <option value="q8_0">Q8 · quality-first</option>
                        <option value="f16">F16 · maximum precision</option>
                      </select>
                    </ConfigField>
                    <ConfigField label="Prompt batch">
                      <input className={inputClass} type="number" min="1" value={Number(read(['n_batch']) ?? readRoot(['gateway', 'n_batch']) ?? 512)} onChange={event => update(['n_batch'], Number(event.target.value))} />
                    </ConfigField>
                    <ConfigField label="Physical batch">
                      <input className={inputClass} type="number" min="1" value={Number(read(['n_ubatch']) ?? readRoot(['gateway', 'n_ubatch']) ?? 512)} onChange={event => update(['n_ubatch'], Number(event.target.value))} />
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
                    <ConfigField label="Speculative mode">
                      <select
                        className={inputClass}
                        value={String(read(['speculative', 'type']) ?? 'none')}
                        onChange={event => update(['speculative', 'type'], event.target.value)}
                      >
                        <option value="none">Disabled</option>
                        <option value="mtp">Adaptive MTP</option>
                      </select>
                    </ConfigField>
                    <ConfigField label="MTP draft tokens">
                      <input className={inputClass} type="number" min="1" max="4" value={Number(read(['speculative', 'draft_tokens']) ?? 2)} onChange={event => update(['speculative', 'draft_tokens'], Number(event.target.value))} />
                    </ConfigField>
                    <ConfigField label="MTP probability floor">
                      <input className={inputClass} type="number" min="0" max="1" step="0.05" value={Number(read(['speculative', 'p_min']) ?? 0)} onChange={event => update(['speculative', 'p_min'], Number(event.target.value))} />
                    </ConfigField>
                    <ConfigField label="MTP active-request limit">
                      <input className={inputClass} type="number" min="1" max={Number(read(['n_slots']) ?? model.n_slots)} value={Number(read(['speculative', 'max_active_requests']) ?? 1)} onChange={event => update(['speculative', 'max_active_requests'], Number(event.target.value))} />
                    </ConfigField>
                  </div>
                  <p className="mt-2 text-xs text-text-muted">
                    Normal request seeds stay random. The benchmark uses fixed seeds internally so every candidate receives the same quality probes.
                  </p>

                  <div className="mt-4 border border-white/10 bg-[#07101d] p-3">
                    <div className="flex flex-wrap items-start justify-between gap-3">
                      <div className="max-w-2xl">
                        <h3 className="text-sm font-medium text-text-primary">Measured mini-benchmark</h3>
                        <p className="mt-1 text-xs text-text-muted">
                          Loads up to three safe profiles in-process, checks one-, two-, and four-request throughput, verifies MTP drafting and acceptance per request, runs fixed-seed correctness probes, and restores the previous model.
                        </p>
                      </div>
                      <div className="flex flex-wrap gap-2">
                        <Button
                          tone="blue"
                          disabled={optimizing || benchmarkRunning || busy || index < 0}
                          onClick={() => { void analyzeProfile(); }}
                        >
                          {benchmarkRunning ? 'Benchmarking model...' : 'Auto-optimize'}
                        </Button>
                        {benchmarkRunning && (
                          <Button onClick={() => { void cancelBenchmark(); }}>
                            Cancel benchmark
                          </Button>
                        )}
                      </div>
                    </div>
                    <div className="mt-3 grid gap-3 border-t border-white/10 pt-3 sm:grid-cols-[auto_1fr_1fr] sm:items-end">
                      <label className="inline-flex min-h-9 items-center gap-2 text-xs text-text-secondary">
                        <input type="checkbox" checked={Boolean(read(['optimization', 'schedule', 'enabled']) ?? model.optimization?.schedule_enabled ?? false)} onChange={event => update(['optimization', 'schedule', 'enabled'], event.target.checked)} />
                        Run on schedule
                      </label>
                      <ConfigField label={`Window start (${scheduleTimezone})`}>
                        <input className={inputClass} type="time" value={String(read(['optimization', 'schedule', 'window_start']) ?? model.optimization?.schedule_window_start ?? '03:00')} onChange={event => update(['optimization', 'schedule', 'window_start'], event.target.value)} />
                      </ConfigField>
                      <ConfigField label={`Window end (${scheduleTimezone})`}>
                        <input className={inputClass} type="time" value={String(read(['optimization', 'schedule', 'window_end']) ?? model.optimization?.schedule_window_end ?? '04:00')} onChange={event => update(['optimization', 'schedule', 'window_end'], event.target.value)} />
                      </ConfigField>
                    </div>
                    <p className="mt-2 text-xs text-text-muted">
                      {scheduleStatus?.enabled && scheduleStatus.nextRunUnixMs
                        ? `Next scheduled window: ${new Date(scheduleStatus.nextRunUnixMs).toLocaleString()}.`
                        : 'Scheduling is disabled. The default maintenance window is 03:00-04:00 server local time.'}
                      {' '}Last scheduled outcome: {scheduleStatus?.lastOutcome ?? 'never'}{scheduleStatus?.lastMessage ? ` — ${scheduleStatus.lastMessage}` : ''}.
                    </p>
                    {(status?.queue.running ?? 0) > 0 || (status?.queue.queued ?? 0) > 0 ? (
                      <p className="mt-2 text-xs text-warning-amber">
                        Safety gate: the benchmark waits for active and queued work using the same compute resource.
                      </p>
                    ) : null}
                    {benchmarkRunning && benchmark && (
                      <div className="mt-3">
                        <div className="h-2 overflow-hidden bg-white/5">
                          <div className="h-full bg-accent-blue transition-all" style={{ width: `${Math.max(2, benchmark.progressPct)}%` }} />
                        </div>
                        <div className="mt-2 flex flex-wrap justify-between gap-2 text-xs text-text-muted">
                          <span>{benchmark.message}</span>
                          <span>{benchmark.completedCandidates}/{benchmark.totalCandidates || 3} profiles</span>
                        </div>
                      </div>
                    )}
                    {benchmark?.state === 'failed' && (
                      <p className="mt-3 text-xs text-danger-rose">{benchmark.message}</p>
                    )}
                    {benchmark?.state === 'cancelled' && (
                      <p className="mt-3 text-xs text-warning-amber">{benchmark.message}</p>
                    )}
                    {benchmark?.recommended && (
                      <div className="mt-3 border-t border-white/10 pt-3">
                        <div className="grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
                          <Stat label="Performance vs current" value={`${performanceIndex.toFixed(1)}%`} sub="Current profile = 100%" tone={performanceTone} />
                          <Stat label="Prompt processing" value={`${winnerTrial?.promptTokensPerSecond.toFixed(1) ?? '0.0'} t/s`} />
                          <Stat label="Single generation speed" value={`${winnerTrial?.averageTokensPerSecond.toFixed(1) ?? '0.0'} t/s`} />
                          <Stat label="Peak VRAM" value={formatMb(winnerTrial?.peakVramMb ?? benchmark.recommended.estimatedVramMb)} tone={benchmark.recommended.fits ? 'good' : 'critical'} />
                        </div>
                        <p className="mt-3 text-sm text-text-secondary">
                          Recommend {formatTokenCount(benchmark.recommended.contextPerSlot)} context per slot,
                          {' '}{benchmark.recommended.slots} slot(s),
                          {' '}{benchmark.recommended.cacheTypeK}/{benchmark.recommended.cacheTypeV} KV,
                          {' '}batch {benchmark.recommended.nBatch}/{benchmark.recommended.nUbatch}.
                        </p>
                        <div className="mt-3 overflow-x-auto" role="region" aria-label="Benchmark comparison" tabIndex={0}>
                          <table className="w-full min-w-[760px] text-left text-xs">
                            <thead>
                              <tr className="border-b border-white/10 uppercase tracking-wide text-text-muted">
                                <th className="py-2 pr-3 font-medium">Measured outcome</th>
                                <th className="pr-3 font-medium">Current profile</th>
                                <th className="pr-3 font-medium">Recommendation</th>
                                <th className="font-medium">Change</th>
                              </tr>
                            </thead>
                            <tbody className="divide-y divide-white/5 text-text-secondary">
                              <tr>
                                <td className="py-2 pr-3 font-medium text-text-primary">Performance index</td>
                                <td className="pr-3">100.0%</td>
                                <td className="pr-3">{winnerTrial ? `${winnerTrial.performanceIndex.toFixed(1)}%` : 'n/a'}</td>
                                <td className={changeTone(100, winnerTrial?.performanceIndex)}>{winnerTrial ? `${winnerTrial.performanceIndex - 100 >= 0 ? '+' : ''}${(winnerTrial.performanceIndex - 100).toFixed(1)}%` : 'n/a'}</td>
                              </tr>
                              <tr>
                                <td className="py-2 pr-3">Prompt processing</td>
                                <td className="pr-3">{baselineTrial ? `${baselineTrial.promptTokensPerSecond.toFixed(1)} t/s` : 'n/a'}</td>
                                <td className="pr-3">{winnerTrial ? `${winnerTrial.promptTokensPerSecond.toFixed(1)} t/s` : 'n/a'}</td>
                                <td className={changeTone(baselineTrial?.promptTokensPerSecond, winnerTrial?.promptTokensPerSecond)}>{relativeChange(baselineTrial?.promptTokensPerSecond, winnerTrial?.promptTokensPerSecond)}</td>
                              </tr>
                              <tr>
                                <td className="py-2 pr-3">Single generation speed</td>
                                <td className="pr-3">{baselineTrial ? `${baselineTrial.averageTokensPerSecond.toFixed(1)} t/s` : 'n/a'}</td>
                                <td className="pr-3">{winnerTrial ? `${winnerTrial.averageTokensPerSecond.toFixed(1)} t/s` : 'n/a'}</td>
                                <td className={changeTone(baselineTrial?.averageTokensPerSecond, winnerTrial?.averageTokensPerSecond)}>{relativeChange(baselineTrial?.averageTokensPerSecond, winnerTrial?.averageTokensPerSecond)}</td>
                              </tr>
                              <tr>
                                <td className="py-2 pr-3">Parallel throughput</td>
                                <td className="pr-3">{baselineTrial ? `${baselineTrial.parallelTokensPerSecond.toFixed(1)} t/s` : 'n/a'}</td>
                                <td className="pr-3">{winnerTrial ? `${winnerTrial.parallelTokensPerSecond.toFixed(1)} t/s` : 'n/a'}</td>
                                <td className={changeTone(baselineTrial?.parallelTokensPerSecond, winnerTrial?.parallelTokensPerSecond)}>{relativeChange(baselineTrial?.parallelTokensPerSecond, winnerTrial?.parallelTokensPerSecond)}</td>
                              </tr>
                              {[2, 4].map(requests => {
                                const before = baselineTrial?.concurrency.find(value => value.requests === requests);
                                const after = winnerTrial?.concurrency.find(value => value.requests === requests);
                                if (!before && !after) return null;
                                return (
                                  <React.Fragment key={requests}>
                                    <tr>
                                      <td className="py-2 pr-3">{requests}-request aggregate TPS</td>
                                      <td className="pr-3">{before ? `${before.aggregateTokensPerSecond.toFixed(1)} t/s` : 'n/a'}</td>
                                      <td className="pr-3">{after ? `${after.aggregateTokensPerSecond.toFixed(1)} t/s` : 'n/a'}</td>
                                      <td className={changeTone(before?.aggregateTokensPerSecond, after?.aggregateTokensPerSecond)}>{relativeChange(before?.aggregateTokensPerSecond, after?.aggregateTokensPerSecond)}</td>
                                    </tr>
                                    <tr>
                                      <td className="py-2 pr-3">{requests}-request per-request TPS</td>
                                      <td className="pr-3">{before ? `${before.averageRequestTokensPerSecond.toFixed(1)} t/s` : 'n/a'}</td>
                                      <td className="pr-3">{after ? `${after.averageRequestTokensPerSecond.toFixed(1)} t/s` : 'n/a'}</td>
                                      <td className={changeTone(before?.averageRequestTokensPerSecond, after?.averageRequestTokensPerSecond)}>{relativeChange(before?.averageRequestTokensPerSecond, after?.averageRequestTokensPerSecond)}</td>
                                    </tr>
                                    <tr>
                                      <td className="py-2 pr-3">{requests}-request MTP proof</td>
                                      <td className="pr-3">{before ? `${before.mtpRequests}/${requests} drafted` : 'n/a'}</td>
                                      <td className="pr-3">{after ? `${after.mtpRequests}/${requests} drafted` : 'n/a'}</td>
                                      <td className={after?.mtpRequests === requests ? 'text-success-green' : 'text-danger-rose'}>
                                        {after && after.mtpDraftedTokens > 0
                                          ? `${(after.mtpAcceptedTokens / after.mtpDraftedTokens * 100).toFixed(1)}% accepted`
                                          : 'MTP inactive'}
                                      </td>
                                    </tr>
                                  </React.Fragment>
                                );
                              })}
                              <tr>
                                <td className="py-2 pr-3">Average first token</td>
                                <td className="pr-3">{baselineTrial ? formatDuration(baselineTrial.averageTimeToFirstTokenMs) : 'n/a'}</td>
                                <td className="pr-3">{winnerTrial ? formatDuration(winnerTrial.averageTimeToFirstTokenMs) : 'n/a'}</td>
                                <td className={changeTone(baselineTrial?.averageTimeToFirstTokenMs, winnerTrial?.averageTimeToFirstTokenMs, true)}>{relativeChange(baselineTrial?.averageTimeToFirstTokenMs, winnerTrial?.averageTimeToFirstTokenMs)}</td>
                              </tr>
                              <tr>
                                <td className="py-2 pr-3">Peak VRAM</td>
                                <td className="pr-3">{baselineTrial ? formatMb(baselineTrial.peakVramMb) : 'n/a'}</td>
                                <td className="pr-3">{winnerTrial ? formatMb(winnerTrial.peakVramMb) : 'n/a'}</td>
                                <td className={changeTone(baselineTrial?.peakVramMb, winnerTrial?.peakVramMb, true)}>{relativeChange(baselineTrial?.peakVramMb, winnerTrial?.peakVramMb)}</td>
                              </tr>
                              <tr>
                                <td className="py-2 pr-3">Correctness guard</td>
                                <td className="pr-3">{baselineTrial ? `${baselineTrial.qualityPasses}/${baselineTrial.qualityTotal} probes` : 'n/a'}</td>
                                <td className="pr-3">{winnerTrial ? `${winnerTrial.qualityPasses}/${winnerTrial.qualityTotal} probes` : 'n/a'}</td>
                                <td className={correctnessChange === 'Regressed' ? 'text-danger-rose' : correctnessChange === 'Preserved' ? 'text-success-green' : 'text-text-secondary'}>{correctnessChange}</td>
                              </tr>
                            </tbody>
                          </table>
                        </div>
                        <p className="mt-2 text-xs text-text-muted">
                          Performance is indexed to the current profile at 100%, weighting prompt-processing throughput and generation TPS equally. Positive changes are green; regressions are red. Fixed prompts are used only to reject a candidate that loses correctness.
                        </p>
                        <div className="mt-3 grid gap-2 text-xs text-text-secondary sm:grid-cols-2">
                          <div className="border-t border-white/10 pt-2">
                            <span className="block font-medium text-text-primary">Current active values</span>
                            <span>{formatTokenCount(Number(read(['context_size']) ?? model.context_size))} context · {Number(read(['n_slots']) ?? model.n_slots)} slot(s) · {String(read(['cache_type_k']) ?? readRoot(['gateway', 'cache_type_k']) ?? 'q8_0')}/{String(read(['cache_type_v']) ?? readRoot(['gateway', 'cache_type_v']) ?? 'q8_0')} KV</span>
                          </div>
                          <div className="border-t border-white/10 pt-2">
                            <span className="block font-medium text-text-primary">Measured recommendation</span>
                            <span>{formatTokenCount(benchmark.recommended.contextPerSlot)} context · {benchmark.recommended.slots} slot(s) · {benchmark.recommended.cacheTypeK}/{benchmark.recommended.cacheTypeV} KV</span>
                          </div>
                        </div>
                        <p className="mt-1 text-xs text-text-muted">
                          Prompt processing and generation each carry {Math.round(benchmark.weights.promptProcessing * 100)}% of the performance index.
                          {' '}Winner load time: {winnerTrial ? formatDuration(winnerTrial.loadMs) : 'n/a'}.
                          {' '}Average first token: {winnerTrial ? formatDuration(winnerTrial.averageTimeToFirstTokenMs) : 'n/a'}.
                          {' '}Previous residency restored: {benchmark.restored ? 'yes' : 'no'}.
                        </p>
                        <div className="mt-3 grid gap-2">
                          {benchmark.candidates.map((candidate, candidateIndex) => (
                            <div key={`${candidate.contextPerSlot}-${candidate.slots}-${candidate.cacheTypeK}-${candidate.mtpMaxActiveRequests}-${candidateIndex}`} className="grid gap-1 border border-white/10 px-3 py-2 text-xs text-text-secondary sm:grid-cols-6">
                              <span>{formatTokenCount(candidate.contextPerSlot)} × {candidate.slots} slots</span>
                              <span>{candidate.cacheTypeK}/{candidate.cacheTypeV} KV</span>
                              <span>MTP up to {candidate.mtpMaxActiveRequests} request(s)</span>
                              <span>{candidate.averageTokensPerSecond.toFixed(1)} generation t/s</span>
                              <span>{candidate.parallelTokensPerSecond.toFixed(1)} parallel generation t/s</span>
                              <span className={changeTone(100, candidate.performanceIndex)}>{candidate.performanceIndex.toFixed(1)}% vs current · {candidate.qualityPasses}/{candidate.qualityTotal} correctness probes</span>
                            </div>
                          ))}
                        </div>
                        <div className="mt-3 flex flex-wrap items-center gap-2">
                          <Button tone="green" onClick={applyOptimization}>Use these values</Button>
                          <Button onClick={discardOptimization}>Discard results</Button>
                          <Button onClick={() => { void analyzeProfile(); }}>Rerun</Button>
                          {dirty && <Badge label="Values staged" tone="good" />}
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
            <Button tone="blue" disabled={busy || benchmarkRunning || !dirty || index < 0} onClick={() => { void save(); }}>Save active profile</Button>
            <Button disabled={busy || benchmarkRunning || index < 0} onClick={resetModel}>Restore model baseline</Button>
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
