import React, { useCallback, useEffect, useState } from 'react';
import { parseDocument } from 'yaml';
import {
  getConfig,
  saveActiveConfig,
  waitForActiveConfig,
  type ConfigDocument,
} from '../api';
import { Button, Panel, SectionTitle } from './ui';

const DEFAULT_RESERVE_MIB = 1024;
const MAX_CONFIG_INTEGER = 2_147_483_647;

export function readVramReserve(yaml: string): number {
  const document = parseDocument(yaml);
  if (document.errors.length > 0) throw document.errors[0];
  const value = document.getIn(['gateway', 'vram_safety_margin_mb']);
  return typeof value === 'number' && Number.isInteger(value) && value >= 0
    ? value
    : DEFAULT_RESERVE_MIB;
}

export function updateVramReserve(yaml: string, value: number): string {
  const document = parseDocument(yaml);
  if (document.errors.length > 0) throw document.errors[0];
  document.setIn(['gateway', 'vram_safety_margin_mb'], value);
  return document.toString();
}

export interface VramReserveSaveResult {
  config: ConfigDocument;
  conflict: boolean;
}

export async function persistVramReserve(
  config: ConfigDocument,
  value: number,
  timeoutMs = 180_000,
  pollMs = 750,
  onSaved?: () => void,
): Promise<VramReserveSaveResult> {
  const yaml = updateVramReserve(config.activeYaml || config.yaml, value);
  try {
    const saved = await saveActiveConfig(yaml, config.activeRevision);
    onSaved?.();
    return {
      config: await waitForActiveConfig(saved.activeRevision, timeoutMs, pollMs),
      conflict: false,
    };
  } catch (reason) {
    if (reason instanceof Error &&
        reason.message === 'active configuration revision conflict') {
      return { config: await getConfig(), conflict: true };
    }
    throw reason;
  }
}

export const VramReserveSettings: React.FC = () => {
  const [config, setConfig] = useState<ConfigDocument | null>(null);
  const [reserve, setReserve] = useState(String(DEFAULT_RESERVE_MIB));
  const [busy, setBusy] = useState(true);
  const [message, setMessage] = useState('');
  const [error, setError] = useState('');

  const load = useCallback(async () => {
    setBusy(true);
    setError('');
    try {
      const loaded = await getConfig();
      setConfig(loaded);
      setReserve(String(readVramReserve(loaded.activeYaml || loaded.yaml)));
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  }, []);

  useEffect(() => { void load(); }, [load]);

  const value = Number(reserve);
  const valid = reserve.trim() !== '' && Number.isInteger(value) &&
    value >= 0 && value <= MAX_CONFIG_INTEGER;

  const save = async () => {
    if (!config || !valid) return;
    setBusy(true);
    setMessage('');
    setError('');
    try {
      const result = await persistVramReserve(
        config,
        value,
        180_000,
        750,
        () => setMessage('Saved. InferDeck is reloading the configuration; reconnecting...'),
      );
      setConfig(result.config);
      setReserve(String(readVramReserve(
        result.config.activeYaml || result.config.yaml,
      )));
      if (result.conflict) {
        setError('Configuration changed elsewhere. The latest value was loaded; review it and save again.');
      } else {
        setMessage('Saved. InferDeck reloaded the configuration with the new VRAM reserve.');
      }
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  };

  return (
    <Panel>
      <SectionTitle title="VRAM reserve" />
      <p className="mt-2 max-w-3xl text-sm text-text-secondary">
        VRAM kept free for model loading and automatic context pools. Set 0 for no additional reserve. Changes apply after configuration reload.
      </p>
      <div className="mt-3 flex max-w-xl flex-wrap items-end gap-3">
        <label className="min-w-48 flex-1 text-xs text-text-muted">
          Safety margin (MiB)
          <input
            aria-label="VRAM safety margin in MiB"
            type="number"
            min="0"
            max={MAX_CONFIG_INTEGER}
            step="1"
            value={reserve}
            disabled={busy}
            onChange={event => setReserve(event.target.value)}
            className="mt-1 min-h-11 w-full rounded border border-white/10 bg-[#07101d] px-3 text-sm text-text-primary sm:min-h-10"
          />
        </label>
        <Button tone="blue" disabled={busy || !config || !valid} onClick={() => { void save(); }}>
          {busy ? 'Loading...' : 'Save reserve'}
        </Button>
      </div>
      {!valid && (
        <p className="mt-2 text-xs text-danger-rose" role="alert">
          Enter a whole number from 0 to {MAX_CONFIG_INTEGER.toLocaleString()} MiB.
        </p>
      )}
      {message && <p className="mt-2 text-xs text-success-green" role="status">{message}</p>}
      {error && <p className="mt-2 text-xs text-danger-rose" role="alert">{error}</p>}
    </Panel>
  );
};
