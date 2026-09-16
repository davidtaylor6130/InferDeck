import React, { useCallback, useEffect, useState } from 'react';
import {
  createApiKey,
  getApiKeys,
  getApiSettings,
  revokeApiKey,
  saveApiSettings,
  updateApiKey,
  type ApiKeyRecord,
  type ApiSettingsDocument,
} from '../api';
import {
  Badge,
  Button,
  EmptyState,
  Panel,
  SectionTitle,
} from '../components/ui';
import { formatDate } from '../utils';

export const ApiSettingsPage: React.FC = () => {
  const [settings, setSettings] =
    useState<ApiSettingsDocument | null>(null);
  const [allowPublicTraffic, setAllowPublicTraffic] = useState(false);
  const [apiKeys, setApiKeys] = useState<ApiKeyRecord[]>([]);
  const [newName, setNewName] = useState('');
  const [newPriority, setNewPriority] = useState(0);
  const [createdKey, setCreatedKey] = useState('');
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState('');
  const [error, setError] = useState('');

  const load = useCallback(async () => {
    setError('');
    const [settingsResult, keysResult] = await Promise.allSettled([
      getApiSettings(),
      getApiKeys(),
    ]);
    const failures: string[] = [];
    if (settingsResult.status === 'fulfilled') {
      setSettings(settingsResult.value);
      setAllowPublicTraffic(settingsResult.value.allowPublicTraffic);
    } else {
      failures.push(
        settingsResult.reason instanceof Error
          ? settingsResult.reason.message
          : String(settingsResult.reason),
      );
    }
    if (keysResult.status === 'fulfilled') {
      setApiKeys(keysResult.value);
    } else {
      failures.push(
        keysResult.reason instanceof Error
          ? keysResult.reason.message
          : String(keysResult.reason),
      );
    }
    setError(failures.join(' '));
  }, []);

  useEffect(() => {
    void load();
  }, [load]);

  const savePublicAccess = async () => {
    if (!settings) return;
    if (
      allowPublicTraffic &&
      !settings.allowPublicTraffic &&
      !window.confirm(
        'Allow unauthenticated API requests at priority -999999?',
      )
    ) {
      return;
    }
    setBusy(true);
    setError('');
    setMessage('');
    try {
      const result = await saveApiSettings(
        allowPublicTraffic,
        settings.activeRevision,
      );
      setSettings(result);
      setMessage(
        result.applyScheduled
          ? 'Saved. InferDeck is applying the API access change.'
          : 'API access was already set to this value.',
      );
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  };

  const createKey = async () => {
    setBusy(true);
    setError('');
    setMessage('');
    setCreatedKey('');
    try {
      const created = await createApiKey(newName.trim(), newPriority);
      setCreatedKey(created.key);
      setNewName('');
      setNewPriority(0);
      setApiKeys(await getApiKeys());
      setMessage('API key created. Save it now; it cannot be shown again.');
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  };

  const editKey = (
    id: string,
    field: 'name' | 'priority',
    value: string | number,
  ) => {
    setApiKeys(current => current.map(key =>
      key.id === id ? { ...key, [field]: value } : key));
  };

  const saveKey = async (key: ApiKeyRecord) => {
    setBusy(true);
    setError('');
    setMessage('');
    try {
      const updated = await updateApiKey(
        key.id,
        key.name.trim(),
        key.priority,
      );
      setApiKeys(current =>
        current.map(item => item.id === updated.id ? updated : item));
      setMessage('API key settings saved.');
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  };

  const revoke = async (key: ApiKeyRecord) => {
    if (!window.confirm('Revoke API key ' + key.name + '?')) return;
    setBusy(true);
    setError('');
    setMessage('');
    try {
      await revokeApiKey(key.id);
      setApiKeys(await getApiKeys());
      setMessage('API key revoked.');
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  };

  const activeKeys = apiKeys.filter(key => key.revokedAtUnixMs == null);

  return (
    <div className="space-y-6">
      <div>
        <h2 className="text-xl font-semibold text-text-primary">
          API Settings
        </h2>
        <p className="mt-1 text-sm text-text-secondary">
          Control API access and client priorities.
        </p>
      </div>

      <Panel>
        <SectionTitle
          title="Public API access"
          action={settings ? (
            <Badge
              label={
                settings.runningAllowPublicTraffic
                  ? 'Public traffic enabled'
                  : 'Authentication required'
              }
              tone={
                settings.runningAllowPublicTraffic ? 'warn' : 'good'
              }
            />
          ) : undefined}
        />
        <label className="mt-4 flex min-h-11 max-w-3xl cursor-pointer items-start gap-3">
          <input
            type="checkbox"
            checked={allowPublicTraffic}
            disabled={busy || !settings}
            onChange={event =>
              setAllowPublicTraffic(event.target.checked)}
            className="mt-1 h-5 w-5 accent-queue-blue"
          />
          <span>
            <span className="block text-sm font-medium text-text-primary">
              Allow public API traffic
            </span>
            <span className="mt-1 block text-xs text-text-muted">
              Unauthenticated OpenAI API requests run at fixed priority
              {' '}-999999. Dashboard, configuration, model control, API
              keys, and background leases remain protected.
            </span>
          </span>
        </label>
        <div className="mt-3 flex flex-wrap items-center gap-3">
          <Button
            tone="blue"
            disabled={
              busy ||
              !settings ||
              allowPublicTraffic === settings.allowPublicTraffic
            }
            onClick={() => { void savePublicAccess(); }}
          >
            Save API access
          </Button>
          {settings?.restartRequired && (
            <span className="text-xs text-warning-amber">
              A saved configuration is waiting to restart.
            </span>
          )}
        </div>
      </Panel>

      <Panel>
        <SectionTitle title="Create API key" />
        <p className="mt-2 max-w-3xl text-sm text-text-secondary">
          Managed keys authenticate API clients. Their priority is owned by
          InferDeck and cannot be raised by a request.
        </p>
        <div className="mt-3 grid gap-2 sm:grid-cols-[minmax(180px,2fr)_minmax(120px,1fr)_auto]">
          <label className="text-xs text-text-muted">
            Name
            <input
              aria-label="New API key name"
              value={newName}
              maxLength={80}
              onChange={event => setNewName(event.target.value)}
              className="mt-1 min-h-11 w-full rounded border border-white/10 bg-[#07101d] px-3 text-sm text-text-primary sm:min-h-10"
              placeholder="Desktop client"
            />
          </label>
          <label className="text-xs text-text-muted">
            Priority
            <input
              aria-label="New API key priority"
              type="number"
              min="-100"
              max="100"
              value={newPriority}
              onChange={event => setNewPriority(Number(event.target.value))}
              className="mt-1 min-h-11 w-full rounded border border-white/10 bg-[#07101d] px-3 text-sm text-text-primary sm:min-h-10"
            />
          </label>
          <Button
            tone="green"
            className="self-end"
            disabled={
              busy ||
              !newName.trim() ||
              !Number.isInteger(newPriority) ||
              newPriority < -100 ||
              newPriority > 100
            }
            onClick={() => { void createKey(); }}
          >
            Create key
          </Button>
        </div>
        {createdKey && (
          <div className="mt-4 border-l-2 border-warning-amber bg-warning-amber/10 px-3 py-3">
            <p className="text-xs font-medium text-warning-amber">
              This key is shown once. Save it before leaving this page.
            </p>
            <code className="mt-2 block break-all select-all text-sm text-text-primary">
              {createdKey}
            </code>
          </div>
        )}
      </Panel>

      <Panel>
        <SectionTitle
          title="Managed API keys"
          aside={activeKeys.length + ' active'}
        />
        {activeKeys.length === 0 ? (
          <div className="mt-3">
            <EmptyState title="No active API keys" />
          </div>
        ) : (
          <div className="mt-3 divide-y divide-white/10">
            {activeKeys.map(key => (
              <div
                key={key.id}
                className="grid gap-3 py-3 md:grid-cols-[minmax(180px,2fr)_minmax(110px,0.7fr)_minmax(150px,1fr)_auto_auto] md:items-end"
              >
                <label className="text-xs text-text-muted">
                  Name
                  <input
                    aria-label={'Name for ' + key.prefix}
                    value={key.name}
                    maxLength={80}
                    disabled={busy}
                    onChange={event =>
                      editKey(key.id, 'name', event.target.value)}
                    className="mt-1 min-h-11 w-full rounded border border-white/10 bg-[#07101d] px-3 text-sm text-text-primary sm:min-h-10"
                  />
                </label>
                <label className="text-xs text-text-muted">
                  Priority
                  <input
                    aria-label={'Priority for ' + key.prefix}
                    type="number"
                    min="-100"
                    max="100"
                    value={key.priority}
                    disabled={busy}
                    onChange={event =>
                      editKey(
                        key.id,
                        'priority',
                        Number(event.target.value),
                      )}
                    className="mt-1 min-h-11 w-full rounded border border-white/10 bg-[#07101d] px-3 text-sm text-text-primary sm:min-h-10"
                  />
                </label>
                <div className="text-xs text-text-muted">
                  <span className="block font-mono text-text-secondary">
                    {key.prefix}
                  </span>
                  <span className="mt-1 block">
                    Created {formatDate(key.createdAtUnixMs)}
                  </span>
                </div>
                <Button
                  disabled={
                    busy ||
                    !key.name.trim() ||
                    !Number.isInteger(key.priority) ||
                    key.priority < -100 ||
                    key.priority > 100
                  }
                  onClick={() => { void saveKey(key); }}
                >
                  Save
                </Button>
                <Button
                  tone="danger"
                  disabled={busy}
                  onClick={() => { void revoke(key); }}
                >
                  Revoke
                </Button>
              </div>
            ))}
          </div>
        )}
      </Panel>

      {message && (
        <p className="text-sm text-success-green" role="status">
          {message}
        </p>
      )}
      {error && (
        <p className="text-sm text-danger-rose" role="alert">
          {error}
        </p>
      )}
    </div>
  );
};
