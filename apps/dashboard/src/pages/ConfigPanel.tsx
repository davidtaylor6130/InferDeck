import React, { useCallback, useEffect, useState } from 'react';
import { getConfig, resetActiveConfig, waitForStableConfig, type ConfigDocument } from '../api';
import { Badge, Button, Panel } from '../components/ui';

export const ConfigPanel: React.FC = () => {
  const [config, setConfig] = useState<ConfigDocument | null>(null);
  const [busy, setBusy] = useState(true);
  const [message, setMessage] = useState('');

  const load = useCallback(async () => {
    setBusy(true);
    setMessage('');
    try {
      setConfig(await getConfig());
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
    } finally {
      setBusy(false);
    }
  }, []);

  useEffect(() => { void load(); }, [load]);

  const reset = async () => {
    if (!window.confirm('Discard the complete active profile and apply the stable base configuration now?')) return;
    setBusy(true);
    setMessage('');
    try {
      const result = await resetActiveConfig();
      if (result.removed) {
        setMessage('Active profile removed. InferDeck is applying the stable baseline now.');
        setConfig(await waitForStableConfig());
        setMessage('Stable baseline applied. InferDeck is back online.');
        setBusy(false);
      } else {
        await load();
        setMessage('No active profile existed; the stable baseline is already the source of truth.');
      }
    } catch (error) {
      setMessage(error instanceof Error ? error.message : String(error));
      setBusy(false);
    }
  };

  return (
    <Panel>
      <details>
        <summary className="flex cursor-pointer list-none items-center justify-between gap-3 text-base font-semibold text-text-primary">
          <span>
            Configuration recovery
            <span className="ml-1 text-xs font-normal text-text-muted">(advanced · collapsed for safety)</span>
          </span>
          <span onClick={event => event.preventDefault()}>
            {
          config?.hasActiveProfile
            ? <Badge label={config.usingActiveProfile ? 'Active profile running' : 'Active profile saved'} tone={config.usingActiveProfile ? 'good' : 'warn'} />
            : <Badge label="Stable baseline" tone="good" />
            }
          </span>
        </summary>
        <div className="mt-3 border-l border-border-slate pl-4">
          <p className="max-w-3xl text-xs text-text-muted">
            The complete baseline is preserved below. Model Details writes a separate validated active profile; this file remains the recovery point if that profile cannot load.
          </p>
          {config?.fallbackReason && (
            <div className="mt-3 border-l-2 border-warning-amber bg-warning-amber/10 px-3 py-2 text-xs text-warning-amber">
              The saved active profile was rejected at startup and InferDeck fell back safely: {config.fallbackReason}
            </div>
          )}

          <label className="mt-4 block text-xs text-text-muted">
            Complete stable YAML
            <textarea
              aria-label="Complete stable YAML"
              spellCheck={false}
              readOnly
              className="mt-1 h-[420px] w-full resize-y rounded border border-border-slate bg-[#05080f] p-3 font-mono text-xs leading-5 text-text-secondary"
              value={config?.yaml ?? ''}
            />
          </label>

          {config?.hasActiveProfile && (
            <details className="mt-3 border-t border-border-slate pt-3">
              <summary className="cursor-pointer text-sm font-medium text-text-secondary">Compare saved active profile</summary>
              <textarea
                aria-label="Complete active YAML"
                spellCheck={false}
                readOnly
                className="mt-2 h-[320px] w-full resize-y rounded border border-border-slate bg-[#05080f] p-3 font-mono text-xs leading-5 text-text-secondary"
                value={config.activeYaml}
              />
            </details>
          )}

          <div className="mt-3 flex flex-wrap items-center gap-2">
            <Button disabled={busy} onClick={() => { void load(); }}>{busy ? 'Loading...' : 'Reload from disk'}</Button>
            <Button tone="danger" disabled={busy || !config?.hasActiveProfile} onClick={() => { void reset(); }}>Reset all active changes</Button>
            {message && <span className="text-xs text-text-secondary" role="status">{message}</span>}
          </div>
        </div>
      </details>
    </Panel>
  );
};
