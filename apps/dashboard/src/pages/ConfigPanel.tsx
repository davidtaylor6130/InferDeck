import React, { useCallback, useEffect, useState } from 'react';
import { parseDocument } from 'yaml';
import { getConfig, saveConfig } from '../api';
import { Badge, Button, Panel, SectionTitle } from '../components/ui';

type Value = string | number | boolean;

const inputClass = 'h-9 w-full rounded-md border border-white/10 bg-[#07101d] px-2 text-sm text-text-primary';

export const ConfigPanel: React.FC = () => {
  const [yaml, setYaml] = useState('');
  const [revision, setRevision] = useState('');
  const [busy, setBusy] = useState(true);
  const [dirty, setDirty] = useState(false);
  const [restartRequired, setRestartRequired] = useState(false);
  const [message, setMessage] = useState('');

  const load = useCallback(async () => {
    setBusy(true);
    setMessage('');
    try {
      const config = await getConfig();
      setYaml(config.yaml);
      setRevision(config.revision);
      setDirty(false);
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    } finally {
      setBusy(false);
    }
  }, []);

  useEffect(() => { void load(); }, [load]);

  const read = (path: Array<string>): Value | undefined => {
    try {
      const value = parseDocument(yaml).getIn(path);
      return typeof value === 'string' || typeof value === 'number' || typeof value === 'boolean'
        ? value : undefined;
    } catch {
      return undefined;
    }
  };

  const update = (path: Array<string>, value: Value) => {
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

  const save = async () => {
    setBusy(true);
    setMessage('');
    try {
      const result = await saveConfig(yaml, revision);
      setRevision(result.revision);
      setRestartRequired(result.restartRequired);
      setDirty(false);
      setMessage('Configuration saved. Restart InferDeck to apply it.');
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Panel>
      <SectionTitle
        title="Gateway configuration"
        aside="gateway.yml"
        action={restartRequired ? <Badge label="Restart required" tone="warn" /> : undefined}
      />
      <p className="mt-2 text-xs text-text-muted">Common settings are editable below. The YAML editor preserves every supported and future setting; saved changes apply after restart.</p>
      <div className="mt-4 grid gap-3 md:grid-cols-2 xl:grid-cols-4">
        <Field label="Listen host"><input className={inputClass} value={String(read(['server', 'host']) ?? '')} onChange={event => update(['server', 'host'], event.target.value)} /></Field>
        <Field label="Port"><input className={inputClass} type="number" min="1" max="65535" value={Number(read(['server', 'port']) ?? 11434)} onChange={event => update(['server', 'port'], Number(event.target.value))} /></Field>
        <Field label="Maximum queue"><input className={inputClass} type="number" min="1" value={Number(read(['gateway', 'max_queue_size']) ?? 128)} onChange={event => update(['gateway', 'max_queue_size'], Number(event.target.value))} /></Field>
        <Field label="Telemetry interval (ms)"><input className={inputClass} type="number" min="10" value={Number(read(['observability', 'telemetry_poll_ms']) ?? 100)} onChange={event => update(['observability', 'telemetry_poll_ms'], Number(event.target.value))} /></Field>
        <Field label="VRAM budget (MB)"><input className={inputClass} type="number" min="0" value={Number(read(['gateway', 'vram_budget_mb']) ?? 0)} onChange={event => update(['gateway', 'vram_budget_mb'], Number(event.target.value))} /></Field>
        <Field label="VRAM safety margin (MB)"><input className={inputClass} type="number" min="0" value={Number(read(['gateway', 'vram_safety_margin_mb']) ?? 1024)} onChange={event => update(['gateway', 'vram_safety_margin_mb'], Number(event.target.value))} /></Field>
        <Toggle label="Automatic model switching" checked={Boolean(read(['gateway', 'auto_swap']) ?? true)} onChange={value => update(['gateway', 'auto_swap'], value)} />
        <Toggle label="Require API token" checked={Boolean(read(['auth', 'required']) ?? false)} onChange={value => update(['auth', 'required'], value)} />
      </div>
      <label className="mt-4 block text-xs text-text-muted">
        Complete YAML
        <textarea
          aria-label="Complete YAML"
          spellCheck={false}
          className="mt-1 h-[360px] w-full resize-y rounded-lg border border-white/10 bg-[#05080f] p-3 font-mono text-xs leading-5 text-text-secondary"
          value={yaml}
          onChange={event => { setYaml(event.target.value); setDirty(true); setMessage(''); }}
        />
      </label>
      <div className="mt-3 flex flex-wrap items-center gap-2">
        <Button tone="blue" disabled={busy || !dirty || !yaml} onClick={() => { void save(); }}>Save configuration</Button>
        <Button disabled={busy} onClick={() => { void load(); }}>Reload from disk</Button>
        {busy && <span className="text-xs text-text-muted">Working…</span>}
        {message && <span className="text-xs text-text-secondary">{message}</span>}
      </div>
    </Panel>
  );
};

const Field: React.FC<{ label: string; children: React.ReactNode }> = ({ label, children }) => (
  <label className="text-xs text-text-muted">{label}<span className="mt-1 block">{children}</span></label>
);

const Toggle: React.FC<{ label: string; checked: boolean; onChange: (value: boolean) => void }> = ({ label, checked, onChange }) => (
  <label className="flex h-9 items-center gap-2 self-end rounded-md border border-white/10 bg-[#07101d] px-3 text-xs text-text-secondary">
    <input type="checkbox" checked={checked} onChange={event => onChange(event.target.checked)} />
    {label}
  </label>
);
