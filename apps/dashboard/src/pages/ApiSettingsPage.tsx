import React, { useCallback, useEffect, useState } from 'react';
import { useFeedback } from '../components/Feedback';
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
import { Badge, Button, EmptyState, GroupHeader, GroupList, Notice, PageHeader, Switch } from '../components/ui';
import { formatDate } from '../utils';
import { ThemeSwitch } from '../components/ThemeSwitch';

export const ApiSettingsPage: React.FC = () => {
  const { confirm, toast } = useFeedback();
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
      !(await confirm({
        title: 'Allow public API traffic?',
        detail: 'Anyone who can reach this server can run requests without a key. They run at the lowest priority, -999999.',
        confirmLabel: 'Allow public traffic',
        destructive: true,
      }))
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
      setMessage('');
      toast('API key saved', { detail: key.name });
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  };

  const revoke = async (key: ApiKeyRecord) => {
    if (!(await confirm({ title: `Revoke ${key.name}?`, detail: 'Clients using this key stop working immediately. This cannot be undone.', confirmLabel: 'Revoke key', destructive: true }))) return;
    setBusy(true);
    setError('');
    setMessage('');
    try {
      await revokeApiKey(key.id);
      setApiKeys(await getApiKeys());
      setMessage('');
      toast('API key revoked', { detail: key.name });
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      setBusy(false);
    }
  };

  const activeKeys = apiKeys.filter(key => key.revokedAtUnixMs == null);

  return (
    <div className="max-w-3xl space-y-8">
      <PageHeader
        title="API Settings"
        subtitle="Control API access and client priorities."
      />

      {message && <Notice tone="good" role="status">{message}</Notice>}
      {error && <Notice tone="critical" role="alert">{error}</Notice>}

      <section aria-label="Appearance">
        <GroupHeader title="Appearance" />
        <div className="max-w-sm"><ThemeSwitch showLabels /></div>
      </section>

      <section aria-label="Public API access">
        <GroupHeader
          title="Public API access"
          aside={settings ? (
            <Badge
              label={settings.runningAllowPublicTraffic ? 'Public traffic enabled' : 'Authentication required'}
              tone={settings.runningAllowPublicTraffic ? 'warn' : 'good'}
            />
          ) : undefined}
        />
        <GroupList>
          <div className="flex min-h-11 items-center justify-between gap-4 px-3 py-2.5">
            <span className="min-w-0">
              <span className="block text-base text-text-primary">Allow public API traffic</span>
              <span className="mt-0.5 block text-xs text-text-muted">
                Unauthenticated OpenAI API requests run at fixed priority
                {' '}-999999. Dashboard, configuration, model control, API
                keys, and background leases remain protected.
              </span>
            </span>
            <Switch
              label="Allow public API traffic"
              checked={allowPublicTraffic}
              disabled={busy || !settings}
              onChange={setAllowPublicTraffic}
            />
          </div>
          {settings && allowPublicTraffic !== settings.allowPublicTraffic && (
            <div className="flex items-center justify-between gap-3 px-3 py-2.5">
              <span className="text-xs text-text-muted">Unsaved change</span>
              <Button tone="blue" disabled={busy} onClick={() => { void savePublicAccess(); }}>Save API access</Button>
            </div>
          )}
        </GroupList>
        {settings?.restartRequired && (
          <p className="mt-1.5 text-xs text-warning-amber">A saved configuration is waiting to restart.</p>
        )}
      </section>

      <section aria-label="Create API key">
        <GroupHeader title="Create API key" />
        <GroupList>
          <label className="flex min-h-11 items-center justify-between gap-4 px-3 py-1.5">
            <span className="shrink-0 text-base">Name</span>
            <input
              aria-label="New API key name"
              value={newName}
              maxLength={80}
              onChange={event => setNewName(event.target.value)}
              className="h-8 w-56 min-w-0 px-2.5 text-sm"
              placeholder="Desktop client"
            />
          </label>
          <label className="flex min-h-11 items-center justify-between gap-4 px-3 py-1.5">
            <span className="shrink-0">
              <span className="block text-base">Priority</span>
              <span className="block text-xs text-text-muted">-100 to 100 · higher runs first</span>
            </span>
            <input
              aria-label="New API key priority"
              type="number"
              min="-100"
              max="100"
              value={newPriority}
              onChange={event => setNewPriority(Number(event.target.value))}
              className="tabular h-8 w-24 px-2.5 text-right text-base"
            />
          </label>
          <div className="flex justify-end px-3 py-2.5">
            <Button
              tone="blue"
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
        </GroupList>
        <p className="mt-1.5 text-xs text-text-muted">
          Managed keys authenticate API clients. Their priority is owned by InferDeck and cannot be raised by a request.
        </p>
        {createdKey && (
          <div className="mt-3 rounded-md border border-border-slate border-l-warning-amber bg-panel-slate p-3 [border-left-width:2px]">
            <p className="text-sm text-warning-amber">
              This key is shown once. Save it before leaving this page.
            </p>
            <div className="mt-2 flex items-center gap-2">
              <code className="min-w-0 flex-1 select-all break-all rounded border border-line-strong bg-void-black px-3 py-2 font-mono text-sm text-text-primary">
                {createdKey}
              </code>
              <Button onClick={() => { void navigator.clipboard?.writeText(createdKey); toast('API key copied'); }}>
                Copy key
              </Button>
            </div>
          </div>
        )}
      </section>

      <section aria-label="Managed API keys">
        <GroupHeader title="Managed API keys" aside={`${activeKeys.length} active`} />
        {activeKeys.length === 0 ? (
          <EmptyState title="No active API keys" detail="Create a key above to authenticate a client." />
        ) : (
          <GroupList>
            {activeKeys.map(key => (
              <div key={key.id} className="flex flex-wrap items-center gap-x-3 gap-y-2 px-3 py-2">
                <div className="min-w-0 flex-1 basis-40">
                  <input
                    aria-label={'Name for ' + key.prefix}
                    value={key.name}
                    maxLength={80}
                    disabled={busy}
                    onChange={event => editKey(key.id, 'name', event.target.value)}
                    className="h-7 w-full !border-transparent !bg-transparent px-0 text-sm font-medium text-text-primary hover:!border-line-strong focus:px-2"
                  />
                  <p className="text-xs text-text-muted"><span className="font-mono">{key.prefix}</span> · Created {formatDate(key.createdAtUnixMs)}</p>
                </div>
                <label className="flex items-center gap-2 text-xs text-text-muted">
                  Priority
                  <input
                    aria-label={'Priority for ' + key.prefix}
                    type="number"
                    min="-100"
                    max="100"
                    value={key.priority}
                    disabled={busy}
                    onChange={event => editKey(key.id, 'priority', Number(event.target.value))}
                    className="tabular h-8 w-20 px-2.5 text-right text-sm"
                  />
                </label>
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
                <Button tone="danger" disabled={busy} onClick={() => { void revoke(key); }}>
                  Revoke
                </Button>
              </div>
            ))}
          </GroupList>
        )}
      </section>
    </div>
  );
};
