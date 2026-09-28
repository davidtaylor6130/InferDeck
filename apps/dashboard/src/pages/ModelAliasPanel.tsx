import React, { useEffect, useMemo, useState } from 'react';
import { useFeedback } from '../components/Feedback';
import { deleteModelAlias, getModelAliases, getModels, saveModelAlias, type ModelAliasRecord } from '../api';
import { Button, GroupHeader, GroupList } from '../components/ui';
import { modelBelongsToSection, type DashboardSection } from '../dashboardSections';

export const ModelAliasPanel: React.FC<{ section: DashboardSection }> = ({ section }) => {
  const { confirm, toast } = useFeedback();
  const [aliases, setAliases] = useState<ModelAliasRecord[]>([]);
  const [revision, setRevision] = useState('');
  const [models, setModels] = useState<Awaited<ReturnType<typeof getModels>>>([]);
  const [name, setName] = useState('');
  const [target, setTarget] = useState('');
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);
  const concrete = useMemo(
    () => models.filter(model => !model.alias && modelBelongsToSection(model, section)),
    [models, section],
  );

  const refresh = async () => {
    const [aliasDocument, nextModels] = await Promise.all([getModelAliases(), getModels()]);
    setAliases(aliasDocument.aliases);
    setRevision(aliasDocument.revision);
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
      await saveModelAlias(aliasName, aliasTarget, revision);
      setName('');
      await refresh();
      toast(`Alias ${aliasName} now points to ${aliasTarget}`);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  };

  const remove = async (aliasName: string) => {
    if (!(await confirm({ title: `Delete alias ${aliasName}?`, detail: "Clients using this name stop resolving. The models it pointed to are not affected.", confirmLabel: "Delete alias", destructive: true }))) return;
    setBusy(true);
    setError('');
    try {
      await deleteModelAlias(aliasName, revision);
      await refresh();
      toast(`Alias ${aliasName} deleted`);
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  };

  const scopedAliases = aliases.filter(alias => concrete.some(model => model.id === alias.target));
  return (
    <section aria-label="Stable API aliases">
      <GroupHeader title="Stable API aliases" aside={`${scopedAliases.length} configured`} />
      <GroupList>
        {scopedAliases.map(alias => (
          <div key={alias.name} className="flex flex-wrap items-center gap-x-4 gap-y-2 px-3 py-2">
            <div className="min-w-0 flex-1 basis-40">
              <p className="truncate font-mono text-sm text-text-primary">{alias.name}</p>
              <p className="tabular text-xs text-text-muted">Requires {alias.requiredContextSize.toLocaleString()} context{alias.requiredCapabilities.length ? `, ${alias.requiredCapabilities.join(', ')}` : ''}</p>
            </div>
            <span className="text-sm text-text-muted" aria-hidden="true">→</span>
            <select aria-label={`Target for ${alias.name}`} className="h-8 min-w-0 flex-1 basis-52 px-2.5 font-mono text-sm" value={alias.target} onChange={event => { void save(alias.name, event.target.value); }} disabled={busy}>
              {concrete.map(model => <option key={model.id} value={model.id}>{model.id}</option>)}
            </select>
            <Button tone="danger" disabled={busy} onClick={() => { void remove(alias.name); }} title={`Delete alias ${alias.name}`}>Delete</Button>
          </div>
        ))}
        <div className="flex flex-wrap items-center gap-x-3 gap-y-2 bg-panel-slate px-3 py-2">
          <input aria-label="New alias name" className="h-8 min-w-0 flex-1 basis-32 px-2.5 font-mono text-sm" value={name} onChange={event => setName(event.target.value)} placeholder="New alias, e.g. production-chat" />
          <select aria-label="Alias target" className="h-8 min-w-0 flex-1 basis-52 px-2.5 font-mono text-sm" value={target} onChange={event => setTarget(event.target.value)}>
            {concrete.map(model => <option key={model.id} value={model.id}>{model.id}</option>)}
          </select>
          <Button tone="blue" disabled={busy || !name.trim() || !target} onClick={() => { void save(name.trim(), target); }}>Create alias</Button>
        </div>
      </GroupList>
      {scopedAliases.length === 0 && <p className="mt-1.5 text-xs text-text-muted">No stable aliases for this service.</p>}
      {error && <p className="mt-1.5 text-xs text-danger-rose" role="alert">{error}</p>}
      <p className="mt-1.5 text-xs text-text-muted">
        Give clients a durable model ID. Retargeting is refused unless the replacement preserves the alias context and capability contract.
      </p>
    </section>
  );
};
