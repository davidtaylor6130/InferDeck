import React, { useEffect, useMemo, useState } from 'react';
import { useFeedback } from '../components/Feedback';
import { ChevronRightIcon } from '@heroicons/react/20/solid';
import { parseDocument } from 'yaml';
import {
  cancelProfileBenchmark, getConfig, getOptimizationSchedule, getProfileBenchmark, saveActiveConfig,
  startProfileBenchmark, waitForActiveConfig,
  type ConfigDocument, type ProfileBenchmarkSnapshot, type ProfileOptimizationCandidate,
  type ScheduledOptimizationRecord,
} from '../api';
import {
  Badge, Button, EmptyState, GroupHeader, GroupList, Notice, PageHeader, ProgressBar,
  Readout, Segmented, Spinner, StatTile, Switch,
} from '../components/ui';
import {
  modalityLabel,
  modelsForSection,
  sectionLabel,
  usageForSection,
  type DashboardSection,
} from '../dashboardSections';
import { useGateway } from '../gateway';
import { modelHref, type ModelTab } from '../routes';
import type { ModelInfo } from '../types';
import { formatDuration, formatMb, formatTokenCount } from '../utils';
import { MediaJobsPanel } from './MediaJobsPanel';
import { ModelAliasPanel } from './ModelAliasPanel';

const fieldClass = 'tabular h-8 w-full px-2.5 text-right text-sm sm:w-56';
type ConfigValue = string | number | boolean | null;
type DialogTab = ModelTab;

const SETTINGS_DESCRIPTION: Record<DashboardSection, string> = {
  llm: 'Load, unload, and tune the models the gateway actually runs. Saving applies the active profile automatically.',
  dictation: 'Control backend speech services and tune their runtime profiles. Recording and playback stay in clients such as Open WebUI.',
  image: 'Control image generation runtimes, residency, and model profiles used by the Image API and dashboard generator.',
  music: 'Control music generation runtimes, residency, and model profiles used by the audio generation API and dashboard generator.',
  video: 'Control video generation runtimes, residency, and model profiles used by the video API and dashboard generator.',
};

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
  const { toast } = useFeedback();
  const scopedModels = useMemo(() => modelsForSection(models, section), [models, section]);
  const usage = useMemo(
    () => usageForSection(status?.tokenUsage ?? [], models, section),
    [status?.tokenUsage, models, section],
  );
  const [pending, setPending] = useState('');
  const [error, setError] = useState('');
  const concrete = useMemo(
    () => scopedModels.filter(model => !model.alias).sort((left, right) => left.id.localeCompare(right.id)),
    [scopedModels],
  );
  const loaded = concrete.filter(model => model.loaded || (swap.swapping && swap.target === model.id));
  const available = concrete.filter(model => !loaded.includes(model));

  const load = async (model: string) => {
    setPending(`load:${model}`);
    setError('');
    const failure = await swapTo(model);
    if (failure) toast(`Couldn't load ${model}`, { tone: 'critical', detail: failure });
    else toast(`Loading ${model}`, { tone: 'info', detail: 'You will get a message when it is ready.' });
    setPending('');
  };

  const unloadModel = async (model: string) => {
    setPending(`unload:${model}`);
    setError('');
    const failure = await unload(model);
    if (failure) toast(`Couldn't unload ${model}`, { tone: 'critical', detail: failure });
    else toast(`${model} unloaded`);
    setPending('');
  };

  const renderRuntimeState = (model: ModelInfo, isTarget: boolean) => (
    model.runtime_available === false
      ? <Badge label="Unavailable" tone="critical" />
      : model.loaded
        ? <Badge label={model.primary ? 'Primary' : 'Loaded'} tone="good" />
        : isTarget
          ? <Badge label="Loading" tone="info" />
          : <Badge label="Standby" tone="idle" />
  );

  return (
    <div className="space-y-6">
      <PageHeader
        title={`${sectionLabel(section)} models`}
        subtitle={SETTINGS_DESCRIPTION[section]}
        actions={<a href={`#store/${section}`} className="inline-flex min-h-10 items-center gap-1.5 rounded-md border border-line-strong bg-panel-slate shadow-card px-3 text-sm font-medium text-text-primary hover:bg-elevated-slate sm:min-h-8">Get more models</a>}
      />

      {error && <Notice tone="critical" role="alert">{error}</Notice>}

      {concrete.length === 0 ? (
        <EmptyState
          title={`No ${sectionLabel(section)} models yet`}
          detail="Install one from the Model Store and it appears here, ready to load."
          action={<a href={`#store/${section}`} className="inline-flex min-h-8 items-center rounded-md bg-queue-blue px-3 text-sm font-semibold text-on-accent">Get a model</a>}
        />
      ) : (
        <div className="space-y-6" aria-label="Runtime model cards">
          {[{ title: 'Loaded', rows: loaded }, { title: 'Ready to load', rows: available }].filter(group => group.rows.length).map(group => (
            <section key={group.title}>
              <GroupHeader title={group.title} aside={`${group.rows.length}`} />
              <div className="divide-y divide-border-slate rounded-lg border border-border-slate bg-panel-slate shadow-card">
                {group.rows.map(model => (
                  <ModelRow
                    key={model.id}
                    model={model}
                    section={section}
                    requests={usage.find(row => row.model === model.id)?.requests ?? 0}
                    loading={swap.swapping && swap.target === model.id}
                    busy={pending !== '' || (swap.swapping && !model.loaded)}
                    pending={pending}
                    onLoad={() => { void load(model.id); }}
                    onUnload={() => { void unloadModel(model.id); }}
                  />
                ))}
              </div>
            </section>
          ))}
        </div>
      )}

      <ModelAliasPanel section={section} />

      {section === 'dictation' && <MediaJobsPanel showEmpty />}
      {section === 'image' && (
        <MediaJobsPanel
          modalities={['image']}
          title="Image generation jobs"
          emptyTitle="No image generation jobs yet"
          emptyDetail="Image requests appear here while the gateway processes them."
          showEmpty
        />
      )}
      {section === 'music' && (
        <MediaJobsPanel
          modalities={['audio_generation']}
          title="Music generation jobs"
          emptyTitle="No music generation jobs yet"
          emptyDetail="Music requests appear here while the gateway processes them."
          showEmpty
        />
      )}
    </div>
  );
};

const FormRow: React.FC<{ label: string; hint?: string; children: React.ReactNode }> = ({ label, hint, children }) => (
  <label className="flex min-h-11 flex-wrap items-center justify-between gap-x-4 gap-y-1.5 px-3 py-2">
    <span className="min-w-0">
      <span className="block text-sm text-text-primary">{label}</span>
      {hint && <span className="block text-xs text-text-muted">{hint}</span>}
    </span>
    {children}
  </label>
);

export const ModelSettingsPanel: React.FC<{
  model: ModelInfo;
  section: DashboardSection;
  initialTab?: DialogTab;
}> = ({ model, section, initialTab }) => {
  const autoOptimize = initialTab === 'optimize';
  const { status } = useGateway();
  const { toast } = useFeedback();
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
  const [tab, setTab] = useState<DialogTab>(section === 'llm' && initialTab ? initialTab : 'resources');
  const tabs: Array<{ id: DialogTab; label: string }> = section === 'llm'
    ? [
        { id: 'resources', label: 'Capacity' },
        { id: 'optimize', label: 'Auto-optimize' },
        { id: 'runtime', label: 'Speed' },
        { id: 'sampling', label: 'Sampling' },
        { id: 'pricing', label: 'Pricing' },
        { id: 'yaml', label: 'Advanced' },
      ]
    : [
        { id: 'resources', label: 'Capacity' },
        { id: 'yaml', label: 'Advanced' },
      ];

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
    let active = true;
    getProfileBenchmark().then(current => {
      if (active && current.model === model.id && current.state !== 'idle') {
        setBenchmark(current);
      }
    }).catch(() => {});
    return () => { active = false; };
  }, [model.id, section]);

  useEffect(() => {
    if (section !== 'llm') return;
    let active = true;
    getOptimizationSchedule().then(result => {
      if (!active) return;
      setScheduleTimezone(result.timezone || 'server local time');
      setScheduleStatus(result.schedules.find(schedule => schedule.model === model.id) ?? null);
    }).catch(() => {});
    return () => { active = false; };
  }, [model.id, section]);

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
    if (!autoOptimize || autoStarted || !config || busy || index < 0 || section !== 'llm') return;
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
      setMessage('');
      toast('Settings saved', { detail: 'InferDeck is running with the new profile.' });
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
  const correctnessChange = baselineTrial && winnerTrial
    ? winnerTrial.qualityScore >= baselineTrial.qualityScore ? 'Preserved' : 'Regressed'
    : 'n/a';

  const numberField = (label: string, path: Array<string | number>, fallback: number, props: React.InputHTMLAttributes<HTMLInputElement> = {}, hint?: string) => (
    <FormRow label={label} hint={hint}>
      <input className={fieldClass} type="number" value={Number(read(path) ?? fallback)} onChange={event => update(path, Number(event.target.value))} {...props} />
    </FormRow>
  );
  const cacheOptions = (
    <>
      <option value="q4_0">Q4 · maximum headroom</option>
      <option value="q8_0">Q8 · quality-first</option>
      <option value="f16">F16 · maximum precision</option>
    </>
  );
  const comparisonRows: Array<{ label: string; before: string; after: string; change: string; tone: string; strong?: boolean }> = [];
  if (benchmark?.recommended) {
    comparisonRows.push({ label: 'Performance index', before: '100.0%', after: winnerTrial ? `${winnerTrial.performanceIndex.toFixed(1)}%` : 'n/a', change: winnerTrial ? `${winnerTrial.performanceIndex - 100 >= 0 ? '+' : ''}${(winnerTrial.performanceIndex - 100).toFixed(1)}%` : 'n/a', tone: changeTone(100, winnerTrial?.performanceIndex), strong: true });
    comparisonRows.push({ label: 'Prompt processing', before: baselineTrial ? `${baselineTrial.promptTokensPerSecond.toFixed(1)} t/s` : 'n/a', after: winnerTrial ? `${winnerTrial.promptTokensPerSecond.toFixed(1)} t/s` : 'n/a', change: relativeChange(baselineTrial?.promptTokensPerSecond, winnerTrial?.promptTokensPerSecond), tone: changeTone(baselineTrial?.promptTokensPerSecond, winnerTrial?.promptTokensPerSecond) });
    comparisonRows.push({ label: 'Single generation speed', before: baselineTrial ? `${baselineTrial.averageTokensPerSecond.toFixed(1)} t/s` : 'n/a', after: winnerTrial ? `${winnerTrial.averageTokensPerSecond.toFixed(1)} t/s` : 'n/a', change: relativeChange(baselineTrial?.averageTokensPerSecond, winnerTrial?.averageTokensPerSecond), tone: changeTone(baselineTrial?.averageTokensPerSecond, winnerTrial?.averageTokensPerSecond) });
    comparisonRows.push({ label: 'Parallel throughput', before: baselineTrial ? `${baselineTrial.parallelTokensPerSecond.toFixed(1)} t/s` : 'n/a', after: winnerTrial ? `${winnerTrial.parallelTokensPerSecond.toFixed(1)} t/s` : 'n/a', change: relativeChange(baselineTrial?.parallelTokensPerSecond, winnerTrial?.parallelTokensPerSecond), tone: changeTone(baselineTrial?.parallelTokensPerSecond, winnerTrial?.parallelTokensPerSecond) });
    for (const count of [2, 4]) {
      const before = baselineTrial?.concurrency.find(value => value.requests === count);
      const after = winnerTrial?.concurrency.find(value => value.requests === count);
      if (!before && !after) continue;
      comparisonRows.push({ label: `${count}-request aggregate TPS`, before: before ? `${before.aggregateTokensPerSecond.toFixed(1)} t/s` : 'n/a', after: after ? `${after.aggregateTokensPerSecond.toFixed(1)} t/s` : 'n/a', change: relativeChange(before?.aggregateTokensPerSecond, after?.aggregateTokensPerSecond), tone: changeTone(before?.aggregateTokensPerSecond, after?.aggregateTokensPerSecond) });
      comparisonRows.push({ label: `${count}-request per-request TPS`, before: before ? `${before.averageRequestTokensPerSecond.toFixed(1)} t/s` : 'n/a', after: after ? `${after.averageRequestTokensPerSecond.toFixed(1)} t/s` : 'n/a', change: relativeChange(before?.averageRequestTokensPerSecond, after?.averageRequestTokensPerSecond), tone: changeTone(before?.averageRequestTokensPerSecond, after?.averageRequestTokensPerSecond) });
      comparisonRows.push({ label: `${count}-request MTP proof`, before: before ? `${before.mtpRequests}/${count} drafted` : 'n/a', after: after ? `${after.mtpRequests}/${count} drafted` : 'n/a', change: after && after.mtpDraftedTokens > 0 ? `${(after.mtpAcceptedTokens / after.mtpDraftedTokens * 100).toFixed(1)}% accepted` : 'MTP inactive', tone: after?.mtpRequests === count ? 'text-success-green' : 'text-danger-rose' });
    }
    comparisonRows.push({ label: 'Average first token', before: baselineTrial ? formatDuration(baselineTrial.averageTimeToFirstTokenMs) : 'n/a', after: winnerTrial ? formatDuration(winnerTrial.averageTimeToFirstTokenMs) : 'n/a', change: relativeChange(baselineTrial?.averageTimeToFirstTokenMs, winnerTrial?.averageTimeToFirstTokenMs), tone: changeTone(baselineTrial?.averageTimeToFirstTokenMs, winnerTrial?.averageTimeToFirstTokenMs, true) });
    comparisonRows.push({ label: 'Peak VRAM', before: baselineTrial ? formatMb(baselineTrial.peakVramMb) : 'n/a', after: winnerTrial ? formatMb(winnerTrial.peakVramMb) : 'n/a', change: relativeChange(baselineTrial?.peakVramMb, winnerTrial?.peakVramMb), tone: changeTone(baselineTrial?.peakVramMb, winnerTrial?.peakVramMb, true) });
    comparisonRows.push({ label: 'Correctness guard', before: baselineTrial ? `${baselineTrial.qualityPasses}/${baselineTrial.qualityTotal} probes` : 'n/a', after: winnerTrial ? `${winnerTrial.qualityPasses}/${winnerTrial.qualityTotal} probes` : 'n/a', change: correctnessChange, tone: correctnessChange === 'Regressed' ? 'text-danger-rose' : correctnessChange === 'Preserved' ? 'text-success-green' : 'text-text-secondary' });
  }

  const slotsNow = Math.max(1, Number(read(['n_slots']) ?? model.n_slots) || 1);
  const contextNow = Number(read(['context_size']) ?? model.context_size) || 0;
  const pool = slotsNow * contextNow;
  const shapes = [
    { slots: 1, label: 'Long conversations', detail: 'One request at a time with the whole context' },
    { slots: 2, label: 'Balanced', detail: 'Two requests at once' },
    { slots: 4, label: 'Many clients', detail: 'Four requests at once with shorter context' },
  ];
  const applyShape = (slots: number) => {
    if (index < 0 || !pool) return;
    try {
      const document = parseDocument(yaml);
      document.setIn(['model_registry', index, 'n_slots'], slots);
      document.setIn(['model_registry', index, 'context_size'], Math.floor(pool / slots / 1024) * 1024);
      setYaml(document.toString());
      setDirty(true);
      setMessage('');
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    }
  };

  return (
    <section aria-label={`${model.id} settings`}>
      <div className="flex flex-wrap items-end justify-between gap-3 pb-3">
        <div>
          <h2 className="text-base font-semibold">Settings</h2>
          <p className="mt-0.5 text-xs text-text-muted">Saved to the active profile. The stable gateway.yml baseline is never changed.</p>
        </div>
        <div className="flex items-center gap-2">
          {dirty && <span className="text-xs text-warning-amber">Unsaved changes</span>}
          <Button tone="blue" disabled={busy || benchmarkRunning || !dirty || index < 0} onClick={() => { void save(); }}>
            {busy && config ? 'Saving...' : 'Save changes'}
          </Button>
        </div>
      </div>

      {busy && !config ? (
        <div className="space-y-1" role="status">
          <span className="sr-only">Loading configuration...</span>
          {[0, 1, 2, 3].map(key => <div key={key} className="h-11 rounded bg-panel-slate" />)}
        </div>
      ) : index < 0 ? (
        <EmptyState title="Model not found in the active configuration" detail="Reload the gateway configuration and try again." />
      ) : (
        <>
          <Segmented aria-label="Model settings sections" value={tab} onChange={id => setTab(id as DialogTab)} items={tabs} className="mb-1" />

          {tab === 'resources' && (
            <>
              {section === 'llm' && pool > 0 && (
                <>
                  <GroupHeader title="How should this model share its memory?" aside={`${formatTokenCount(pool)} tokens of context in total`} />
                  <div className="grid gap-2 sm:grid-cols-3" role="radiogroup" aria-label="Capacity shape">
                    {shapes.map(shape => {
                      const active = slotsNow === shape.slots;
                      return (
                        <button
                          key={shape.slots}
                          type="button"
                          role="radio"
                          aria-checked={active}
                          onClick={() => applyShape(shape.slots)}
                          className={`rounded-md border px-3 py-2.5 text-left transition-colors ${active ? 'border-queue-blue bg-elevated-slate' : 'border-line-strong hover:bg-panel-slate'}`}
                        >
                          <span className="block text-sm font-medium text-text-primary">{shape.label}</span>
                          <span className="mt-0.5 block text-xs text-text-muted">{shape.detail}</span>
                          <span className="tabular mt-1.5 block text-xs text-text-secondary">{shape.slots} × {formatTokenCount(Math.floor(pool / shape.slots / 1024) * 1024)}</span>
                        </button>
                      );
                    })}
                  </div>
                </>
              )}
              <details className="group mt-6" open={!(section === 'llm' && pool > 0)}>
                <summary className="cursor-pointer list-none pb-2 text-sm text-text-secondary hover:text-text-primary">
                  <span className="inline-block transition-transform group-open:rotate-90" aria-hidden="true">›</span> Exact values
                </summary>
                <GroupList>
                  {numberField('Slots', ['n_slots'], model.n_slots, { min: 1 }, 'Concurrent requests this model serves')}
                  {numberField('Minimum slots', ['min_slots'], 1, { min: 1 })}
                  {numberField('VRAM budget (MB)', ['vram_required_mb'], model.vram_required_mb, { min: 0 })}
                  {section === 'llm' && numberField('Context tokens', ['context_size'], model.context_size, { min: 1 }, 'Per slot')}
                  {section === 'llm' && numberField('GPU layers', ['n_gpu_layers'], -1, { min: -1 }, '-1 offloads every layer')}
                </GroupList>
              </details>
            </>
          )}

          {tab === 'sampling' && section === 'llm' && (
            <>
              <GroupHeader title="Sampling defaults" />
              <GroupList>
                {numberField('Temperature', ['sampling', 'temperature'], 0.7, { min: 0, max: 2, step: 0.05 })}
                {numberField('Top P', ['sampling', 'top_p'], 0.95, { min: 0, max: 1, step: 0.01 })}
                {numberField('Repeat penalty', ['sampling', 'repeat_penalty'], 1, { min: 0.01, step: 0.01 })}
              </GroupList>
              <p className="mt-1.5 text-xs text-text-muted">Clients can still override these per request.</p>
            </>
          )}

          {tab === 'pricing' && section === 'llm' && (
            <>
              <GroupHeader title="API-equivalent price per 1M tokens (USD)" />
              <GroupList>
                {numberField('Input', ['prompt_price_per_million'], 0, { min: 0, step: 0.001 })}
                <FormRow label="Cached input">
                  <input className={fieldClass} type="number" min="0" step="0.001" value={Number(read(['cached_prompt_price_per_million']) ?? read(['prompt_price_per_million']) ?? 0)} onChange={event => update(['cached_prompt_price_per_million'], Number(event.target.value))} />
                </FormRow>
                {numberField('Output', ['completion_price_per_million'], 0, { min: 0, step: 0.001 })}
              </GroupList>
              <p className="mt-1.5 text-xs text-text-muted">Used for Usage cost estimates and the Home break-even tracker.</p>
            </>
          )}

          {tab === 'runtime' && section === 'llm' && (
            <>
              <GroupHeader title="KV cache and batching" />
              <GroupList>
                <FormRow label="KV cache keys">
                  <select className={fieldClass} value={String(read(['cache_type_k']) ?? readRoot(['gateway', 'cache_type_k']) ?? 'q8_0')} onChange={event => update(['cache_type_k'], event.target.value)}>{cacheOptions}</select>
                </FormRow>
                <FormRow label="KV cache values">
                  <select className={fieldClass} value={String(read(['cache_type_v']) ?? readRoot(['gateway', 'cache_type_v']) ?? 'q8_0')} onChange={event => update(['cache_type_v'], event.target.value)}>{cacheOptions}</select>
                </FormRow>
                <FormRow label="Prompt batch">
                  <input className={fieldClass} type="number" min="1" value={Number(read(['n_batch']) ?? readRoot(['gateway', 'n_batch']) ?? 512)} onChange={event => update(['n_batch'], Number(event.target.value))} />
                </FormRow>
                <FormRow label="Physical batch">
                  <input className={fieldClass} type="number" min="1" value={Number(read(['n_ubatch']) ?? readRoot(['gateway', 'n_ubatch']) ?? 512)} onChange={event => update(['n_ubatch'], Number(event.target.value))} />
                </FormRow>
                <FormRow label="Flash attention" hint="Gateway-wide">
                  <select className={fieldClass} value={String(readRoot(['gateway', 'flash_attn']) ?? 'auto')} onChange={event => updateRoot(['gateway', 'flash_attn'], event.target.value)}>
                    <option value="auto">Auto</option>
                    <option value="on">On</option>
                    <option value="off">Off</option>
                  </select>
                </FormRow>
              </GroupList>

              <GroupHeader title="Model runtime and adaptive MTP" />
              <GroupList>
                <FormRow label="Speculative mode">
                  <select className={fieldClass} value={String(read(['speculative', 'type']) ?? 'none')} onChange={event => update(['speculative', 'type'], event.target.value)}>
                    <option value="none">Disabled</option>
                    <option value="mtp">Adaptive MTP</option>
                  </select>
                </FormRow>
                {numberField('MTP draft tokens', ['speculative', 'draft_tokens'], 2, { min: 1, max: 4 })}
                {numberField('MTP probability floor', ['speculative', 'p_min'], 0, { min: 0, max: 1, step: 0.05 })}
                <FormRow label="MTP active-request limit">
                  <input className={fieldClass} type="number" min="1" max={Number(read(['n_slots']) ?? model.n_slots)} value={Number(read(['speculative', 'max_active_requests']) ?? 1)} onChange={event => update(['speculative', 'max_active_requests'], Number(event.target.value))} />
                </FormRow>
              </GroupList>
              <p className="mt-1.5 text-xs text-text-muted">
                Adaptive MTP accelerates a single request and automatically returns to ordinary continuous batching when concurrency exceeds its configured window. Normal request seeds stay random.
              </p>
            </>
          )}

          {tab === 'optimize' && section === 'llm' && (
            <>
              <div className="mt-4 rounded-lg border border-border-slate bg-panel-slate shadow-card p-4">
                <div className="flex flex-wrap items-start justify-between gap-4">
                  <div className="max-w-md">
                    <h3 className="text-sm font-semibold">Measured mini-benchmark</h3>
                    <p className="mt-1 text-xs text-text-muted">
                      Loads up to three safe profiles in-process, checks one-, two-, and four-request throughput, verifies MTP drafting and acceptance per request, runs fixed-seed correctness probes, and restores the previous model.
                    </p>
                  </div>
                  <div className="flex gap-2">
                    <Button tone="blue" disabled={optimizing || benchmarkRunning || busy || index < 0} onClick={() => { void analyzeProfile(); }}>
                      {benchmarkRunning ? 'Benchmarking model...' : 'Auto-optimize'}
                    </Button>
                    {benchmarkRunning && <Button onClick={() => { void cancelBenchmark(); }}>Cancel benchmark</Button>}
                  </div>
                </div>
                {benchmarkRunning && benchmark && (
                  <div className="relative mt-5">
                    <ProgressBar percent={Math.max(2, benchmark.progressPct)} tone="info" />
                    <div className="mt-2 flex flex-wrap justify-between gap-2 text-xs text-text-muted">
                      <span>{benchmark.message}</span>
                      <span className="tabular">{benchmark.completedCandidates}/{benchmark.totalCandidates || 3} profiles</span>
                    </div>
                  </div>
                )}
                {(status?.queue.running ?? 0) > 0 || (status?.queue.queued ?? 0) > 0 ? (
                  <p className="relative mt-3 text-xs text-warning-amber">Safety gate: the benchmark waits for active and queued work using the same compute resource.</p>
                ) : null}
                {benchmark?.state === 'failed' && <p className="relative mt-3 text-xs text-danger-rose">{benchmark.message}</p>}
                {benchmark?.state === 'cancelled' && <p className="relative mt-3 text-xs text-warning-amber">{benchmark.message}</p>}
              </div>

              {benchmark?.recommended && (
                <>
                  <Readout className="mt-4">
                    <StatTile label="Performance vs current" value={`${performanceIndex.toFixed(1)}%`} sub="Current profile = 100%" tone={performanceIndex > 100 ? 'good' : performanceIndex < 100 ? 'critical' : 'idle'} />
                    <StatTile label="Prompt processing" value={`${winnerTrial?.promptTokensPerSecond.toFixed(1) ?? '0.0'} t/s`} />
                    <StatTile label="Single generation speed" value={`${winnerTrial?.averageTokensPerSecond.toFixed(1) ?? '0.0'} t/s`} />
                    <StatTile label="Peak VRAM" value={formatMb(winnerTrial?.peakVramMb ?? benchmark.recommended.estimatedVramMb)} tone={benchmark.recommended.fits ? 'good' : 'critical'} />
                  </Readout>
                  <p className="mt-4 px-1 text-sm text-text-secondary">
                    Recommend <span className="font-semibold text-text-primary">{formatTokenCount(benchmark.recommended.contextPerSlot)}</span> context per slot,
                    {' '}<span className="font-semibold text-text-primary">{benchmark.recommended.slots}</span> slot(s),
                    {' '}{benchmark.recommended.cacheTypeK}/{benchmark.recommended.cacheTypeV} KV,
                    {' '}batch {benchmark.recommended.nBatch}/{benchmark.recommended.nUbatch}.
                  </p>
                  <div className="mt-3 overflow-x-auto rounded-lg border border-border-slate bg-panel-slate shadow-card" role="region" aria-label="Benchmark comparison" tabIndex={0}>
                    <table className="w-full min-w-[560px] text-left text-sm">
                      <thead>
                        <tr className="text-xs text-text-muted">
                          <th className="px-3 py-2.5 font-medium">Measured outcome</th>
                          <th className="px-2 font-medium">Current profile</th>
                          <th className="px-2 font-medium">Recommendation</th>
                          <th className="px-4 text-right font-medium">Change</th>
                        </tr>
                      </thead>
                      <tbody className="tabular text-text-secondary">
                        {comparisonRows.map(row => (
                          <tr key={row.label} className="border-t border-border-slate">
                            <td className={`px-4 py-2 ${row.strong ? 'font-semibold text-text-primary' : ''}`}>{row.label}</td>
                            <td className="px-2">{row.before}</td>
                            <td className="px-2 text-text-primary">{row.after}</td>
                            <td className={`px-4 text-right font-semibold ${row.tone}`}>{row.change}</td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  </div>
                  <p className="mt-2 px-1 text-xs text-text-muted">
                    Performance is indexed to the current profile at 100%, weighting prompt-processing throughput and generation TPS equally ({Math.round(benchmark.weights.promptProcessing * 100)}% each). Winner load time: {winnerTrial ? formatDuration(winnerTrial.loadMs) : 'n/a'}. Previous residency restored: {benchmark.restored ? 'yes' : 'no'}.
                  </p>
                  <GroupHeader title="Candidates" />
                  <GroupList>
                    {benchmark.candidates.map((candidate, candidateIndex) => (
                      <div key={`${candidate.contextPerSlot}-${candidate.slots}-${candidate.cacheTypeK}-${candidate.mtpMaxActiveRequests}-${candidateIndex}`} className="flex flex-wrap items-center justify-between gap-x-4 gap-y-1 px-3 py-2.5 text-xs">
                        <span className="text-text-primary">{formatTokenCount(candidate.contextPerSlot)} × {candidate.slots} slots · {candidate.cacheTypeK}/{candidate.cacheTypeV} KV · MTP up to {candidate.mtpMaxActiveRequests}</span>
                        <span className="tabular text-text-muted">{candidate.averageTokensPerSecond.toFixed(1)} / {candidate.parallelTokensPerSecond.toFixed(1)} t/s · <span className={changeTone(100, candidate.performanceIndex)}>{candidate.performanceIndex.toFixed(1)}%</span> · {candidate.qualityPasses}/{candidate.qualityTotal} probes</span>
                      </div>
                    ))}
                  </GroupList>
                  <div className="mt-4 flex flex-wrap items-center gap-2">
                    <Button tone="green" onClick={applyOptimization}>Use these values</Button>
                    <Button onClick={discardOptimization}>Discard results</Button>
                    <Button onClick={() => { void analyzeProfile(); }}>Rerun</Button>
                    {dirty && <Badge label="Values staged" tone="good" />}
                  </div>
                </>
              )}

              <GroupHeader title="Schedule" />
              <GroupList>
                <div className="flex min-h-11 items-center justify-between gap-4 px-3 py-2">
                  <span className="text-base">Run on schedule</span>
                  <Switch
                    label="Run on schedule"
                    checked={Boolean(read(['optimization', 'schedule', 'enabled']) ?? model.optimization?.schedule_enabled ?? false)}
                    onChange={checked => update(['optimization', 'schedule', 'enabled'], checked)}
                  />
                </div>
                <FormRow label="Window start" hint={scheduleTimezone}>
                  <input className={fieldClass} type="time" value={String(read(['optimization', 'schedule', 'window_start']) ?? model.optimization?.schedule_window_start ?? '03:00')} onChange={event => update(['optimization', 'schedule', 'window_start'], event.target.value)} />
                </FormRow>
                <FormRow label="Window end" hint={scheduleTimezone}>
                  <input className={fieldClass} type="time" value={String(read(['optimization', 'schedule', 'window_end']) ?? model.optimization?.schedule_window_end ?? '04:00')} onChange={event => update(['optimization', 'schedule', 'window_end'], event.target.value)} />
                </FormRow>
              </GroupList>
              <p className="mt-1.5 text-xs text-text-muted">
                {scheduleStatus?.enabled && scheduleStatus.nextRunUnixMs
                  ? `Next scheduled window: ${new Date(scheduleStatus.nextRunUnixMs).toLocaleString()}.`
                  : 'Scheduling is disabled. The default maintenance window is 03:00-04:00 server local time.'}
                {' '}Last scheduled outcome: {scheduleStatus?.lastOutcome ?? 'never'}{scheduleStatus?.lastMessage ? ` — ${scheduleStatus.lastMessage}` : ''}.
              </p>
            </>
          )}

          {tab === 'yaml' && (
            <>
              <GroupHeader title="Advanced active YAML" />
              <textarea
                aria-label="Advanced active YAML"
                spellCheck={false}
                className="h-[420px] w-full resize-y p-3 font-mono text-xs leading-5 text-text-secondary"
                value={yaml}
                onChange={event => { setYaml(event.target.value); setDirty(true); setMessage(''); }}
              />
              <p className="mt-1.5 text-xs text-text-muted">Full control is available here for runtime artifacts, sampling, memory, and any setting not exposed above.</p>
            </>
          )}
        </>
      )}

      {message && <Notice tone="info" role="status" className="mt-4">{message}</Notice>}
      <div className="mt-6 flex flex-wrap items-center gap-3 border-t border-border-slate pt-4 text-xs text-text-muted">
        <Button disabled={busy || benchmarkRunning || index < 0} onClick={resetModel}>Restore model baseline</Button>
        {config?.hasActiveProfile && <span>{config.usingActiveProfile ? 'Active profile is running.' : 'Active profile saved, not running yet.'}</span>}
      </div>
    </section>
  );
};

const ModelRow: React.FC<{
  model: ModelInfo;
  section: DashboardSection;
  requests: number;
  loading: boolean;
  busy: boolean;
  pending: string;
  onLoad: () => void;
  onUnload: () => void;
}> = ({ model, section, requests, loading, busy, pending, onLoad, onUnload }) => {
  const active = model.active_requests ?? (model.free_slots != null ? model.n_slots - model.free_slots : 0);
  const facts = [
    modalityLabel(model.modality),
    model.loaded ? `${active} of ${model.n_slots} slot${model.n_slots === 1 ? '' : 's'} busy` : `${model.n_slots} slot${model.n_slots === 1 ? '' : 's'}`,
    section === 'llm' && model.context_size ? `${formatTokenCount(model.context_size)} context` : '',
    model.vram_required_mb ? formatMb(model.vram_required_mb) : '',
    requests ? `${requests.toLocaleString()} request${requests === 1 ? '' : 's'}` : 'Not used yet',
  ].filter(Boolean);
  return (
    <div className="flex flex-wrap items-center gap-x-4 gap-y-2 px-3 py-3 transition-colors hover:bg-elevated-slate/60 sm:flex-nowrap">
      <span className={`h-2 w-2 shrink-0 rounded-full ${model.runtime_available === false ? 'bg-danger-rose' : model.loaded ? 'bg-success-green' : loading ? 'bg-queue-blue' : 'bg-line-strong'}`} aria-hidden="true" />
      <a href={modelHref(model.id)} className="group min-w-0 flex-1">
        <span className="flex min-w-0 flex-wrap items-center gap-2">
          <span className="truncate font-mono text-sm text-text-primary group-hover:underline">{model.id}</span>
          {model.primary && <Badge label="Primary" tone="info" />}
          {model.runtime_available === false && <Badge label="Unavailable" tone="critical" />}
          {loading && <Badge label="Loading" tone="info" />}
          {model.optimization?.status === 'measured' && <Badge label="Measured optimized" tone="good" />}
        </span>
        <span className="mt-0.5 block truncate text-xs text-text-muted">{facts.join(' · ')}</span>
      </a>
      <div className="flex shrink-0 items-center gap-1.5">
        {model.runtime_available === false ? null : model.loaded ? (
          <button
            type="button"
            aria-label={pending === `unload:${model.id}` ? `Unloading ${model.id}` : `Unload ${model.id}`}
            disabled={pending !== ''}
            onClick={onUnload}
            className="inline-flex min-h-10 items-center gap-1.5 rounded-md border border-line-strong bg-panel-slate shadow-card px-3 text-sm font-medium text-text-primary hover:bg-elevated-slate disabled:opacity-40 sm:min-h-8"
          >
            {pending === `unload:${model.id}` && <Spinner />}
            Unload
          </button>
        ) : (
          <button
            type="button"
            aria-label={pending === `load:${model.id}` ? `Loading ${model.id}` : `Load ${model.id}`}
            disabled={busy}
            onClick={onLoad}
            className="inline-flex min-h-10 items-center gap-1.5 rounded-md border border-line-strong bg-panel-slate shadow-card px-3 text-sm font-medium text-queue-blue hover:bg-elevated-slate disabled:opacity-40 sm:min-h-8"
          >
            {(loading || pending === `load:${model.id}`) && <Spinner />}
            {loading ? 'Loading' : 'Load'}
          </button>
        )}
        <a
          href={modelHref(model.id)}
          aria-label={`Model settings for ${model.id}`}
          title={`Model settings for ${model.id}`}
          className="inline-flex h-10 w-10 items-center justify-center rounded-md text-text-muted hover:bg-elevated-slate hover:text-text-primary sm:h-8 sm:w-8"
        >
          <ChevronRightIcon className="h-4 w-4" aria-hidden="true" />
        </a>
      </div>
    </div>
  );
};
