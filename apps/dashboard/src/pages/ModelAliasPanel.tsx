import React, { useEffect, useMemo, useState } from 'react';
import { deleteModelAlias, getModelAliases, getModels, saveModelAlias, type ModelAliasRecord } from '../api';
import { Badge, Button, EmptyState, Panel, SectionTitle } from '../components/ui';
import { type DashboardSection } from '../dashboardSections';

export const ModelAliasPanel: React.FC<{ section: DashboardSection }> = ({ section }) => {
  const [aliases, setAliases] = useState<ModelAliasRecord[]>([]);
  const [models, setModels] = useState<Awaited<ReturnType<typeof getModels>>>([]);
  const [name, setName] = useState('');
  const [target, setTarget] = useState('');
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);
  const concrete = useMemo(() => models.filter(model => !model.alias && (
    section === 'dictation'
      ? model.modality === 'audio_transcription' || model.modality === 'audio_speech'
      : model.modality !== 'audio_transcription' && model.modality !== 'audio_speech'
  )), [models, section]);

  const refresh = async () => {
    const [nextAliases, nextModels] = await Promise.all([getModelAliases(), getModels()]);
    setAliases(nextAliases);
    setModels(nextModels);
  };

  useEffect(() => { void refresh().catch(reason => setError(reason instanceof Error ? reason.message : String(reason))); }, []);
  useEffect(() => {
    if (!concrete.some(model => model.id === target)) setTarget(concrete[0]?.id ?? '');
  }, [concrete, target]);

  const save = async (aliasName: string, aliasTarget: string) => {
    setBusy(true);
    setError('');
    try {
      await saveModelAlias(aliasName, aliasTarget);
      setName('');
      await refresh();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  };

  const remove = async (aliasName: string) => {
    if (!window.confirm(`Delete model alias ${aliasName}? Concrete models and active requests are unaffected.`)) return;
    setBusy(true);
    setError('');
    try {
      await deleteModelAlias(aliasName);
      await refresh();
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  };

  const scopedAliases = aliases.filter(alias => concrete.some(model => model.id === alias.target));
  return (
    <Panel>
      <SectionTitle title="Stable API aliases" aside={`${scopedAliases.length} configured`} />
      <p className="mt-2 max-w-3xl text-sm text-text-secondary">
        Give clients a durable model ID. Retargeting is refused unless the replacement preserves the alias context and capability contract.
      </p>
      <div className="mt-3 grid gap-2 sm:grid-cols-[minmax(160px,1fr)_minmax(220px,2fr)_auto]">
        <input aria-label="New alias name" className="h-9 rounded border border-white/10 bg-[#07101d] px-2 text-sm text-text-primary" value={name} onChange={event => setName(event.target.value)} placeholder="production-chat" />
        <select aria-label="Alias target" className="h-9 rounded border border-white/10 bg-[#07101d] px-2 text-sm text-text-primary" value={target} onChange={event => setTarget(event.target.value)}>
          {concrete.map(model => <option key={model.id} value={model.id}>{model.id}</option>)}
        </select>
        <Button tone="blue" disabled={busy || !name.trim() || !target} onClick={() => { void save(name.trim(), target); }}>Create alias</Button>
      </div>
      {error && <p className="mt-2 text-xs text-danger-rose" role="alert">{error}</p>}
      {scopedAliases.length === 0 ? <div className="mt-3"><EmptyState title="No stable aliases for this service" /></div> : (
        <div className="mt-3 divide-y divide-white/5">
          {scopedAliases.map(alias => (
            <div key={alias.name} className="grid gap-2 py-3 text-sm md:grid-cols-[1fr_2fr_auto] md:items-center">
              <div><p className="font-mono text-text-primary">{alias.name}</p><p className="text-xs text-text-muted">contract: {alias.requiredContextSize.toLocaleString()} context</p></div>
              <div>
                <select aria-label={`Target for ${alias.name}`} className="h-9 w-full rounded border border-white/10 bg-[#07101d] px-2 text-sm text-text-primary" value={alias.target} onChange={event => { void save(alias.name, event.target.value); }} disabled={busy}>
                  {concrete.map(model => <option key={model.id} value={model.id}>{model.id}</option>)}
                </select>
                <span className="mt-1 flex flex-wrap gap-1">{alias.requiredCapabilities.map(capability => <Badge key={capability} label={capability} tone="idle" />)}</span>
              </div>
              <Button tone="danger" disabled={busy} onClick={() => { void remove(alias.name); }}>Delete alias</Button>
            </div>
          ))}
        </div>
      )}
    </Panel>
  );
};
