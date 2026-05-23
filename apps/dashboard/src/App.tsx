import React, { useCallback, useEffect, useMemo, useState } from 'react';
import { AppShell } from './components/AppShell';
import type { DashboardActions, DashboardState, JobRecord, ModelRecord, PageId, PageProps, ServiceRecord } from './types';
import { HardwarePage } from './pages/HardwarePage';
import { JobsPage } from './pages/JobsPage';
import { LogsPage } from './pages/LogsPage';
import { ModelsPage } from './pages/ModelsPage';
import { OverviewPage } from './pages/OverviewPage';
import { QueuePage } from './pages/QueuePage';
import { ServicesPage } from './pages/ServicesPage';
import { SettingsPage } from './pages/SettingsPage';
import { formatError } from './utils';

const API = '/api';
const pages: PageId[] = ['overview', 'queue', 'jobs', 'models', 'services', 'hardware', 'logs', 'settings'];

interface Toast {
  id: number;
  message: string;
  tone: 'success' | 'warning' | 'danger' | 'info';
}

const App: React.FC = () => {
  const [activePage, setActivePage] = useState<PageId>(() => getHashPage());
  const [healthData, setHealthData] = useState<Record<string, any> | null>(null);
  const [statusData, setStatusData] = useState<Record<string, any> | null>(null);
  const [jobsList, setJobsList] = useState<JobRecord[]>([]);
  const [modelsList, setModelsList] = useState<ModelRecord[]>([]);
  const [whisperModels, setWhisperModels] = useState<ModelRecord[]>([]);
  const [runningModels, setRunningModels] = useState<ModelRecord[]>([]);
  const [servicesList, setServicesList] = useState<ServiceRecord[]>([]);
  const [errors, setErrors] = useState<Record<string, string | null>>({});
  const [loading, setLoading] = useState(true);
  const [connected, setConnected] = useState(false);
  const [lastUpdatedAt, setLastUpdatedAt] = useState<Date | null>(null);
  const [toasts, setToasts] = useState<Toast[]>([]);

  const toast = useCallback((message: string, tone: Toast['tone'] = 'info') => {
    const id = Date.now() + Math.random();
    setToasts(current => [...current, { id, message, tone }].slice(-4));
    window.setTimeout(() => setToasts(current => current.filter(item => item.id !== id)), 2600);
  }, []);

  const requestJson = useCallback(async <T,>(path: string, options?: RequestInit): Promise<T> => {
    const res = await fetch(`${API}${path}`, options);
    if (!res.ok) {
      const body = await res.json().catch(() => ({}));
      throw new Error(formatError(body.error || `HTTP ${res.status}`));
    }
    return res.json() as Promise<T>;
  }, []);

  const fetchHealth = useCallback(async () => {
    try {
      const data = await requestJson<Record<string, any>>('/health');
      setHealthData(data);
      setErrors(prev => ({ ...prev, health: null }));
      setConnected(true);
      setLastUpdatedAt(new Date());
    } catch (err) {
      setHealthData(prev => prev || { status: 'degraded', version: '0.1.0' });
      setConnected(false);
      setErrors(prev => ({ ...prev, health: formatError(err) }));
    } finally {
      setLoading(false);
    }
  }, [requestJson]);

  const fetchStatus = useCallback(async () => {
    try {
      const data = await requestJson<Record<string, any>>('/status');
      setStatusData(data);
      setErrors(prev => ({ ...prev, status: null }));
      setLastUpdatedAt(new Date());
    } catch (err) {
      setErrors(prev => ({ ...prev, status: formatError(err) }));
    }
  }, [requestJson]);

  const fetchJobs = useCallback(async () => {
    try {
      const data = await requestJson<{ jobs?: JobRecord[] }>('/jobs');
      setJobsList(data.jobs || []);
      setErrors(prev => ({ ...prev, jobs: null }));
    } catch (err) {
      setErrors(prev => ({ ...prev, jobs: formatError(err) }));
    }
  }, [requestJson]);

  const fetchModels = useCallback(async () => {
    try {
      const data = await requestJson<{ models?: ModelRecord[]; whisperModels?: ModelRecord[]; backends?: { error?: string } }>('/models');
      setModelsList(data.models || []);
      setWhisperModels(data.whisperModels || []);
      const running = await requestJson<{ running?: ModelRecord[] }>('/models/running').catch(() => ({ running: [] }));
      setRunningModels(running.running || []);
      setErrors(prev => ({ ...prev, models: data.backends?.error ? formatError(data.backends.error) : null }));
    } catch (err) {
      setErrors(prev => ({ ...prev, models: formatError(err) }));
    }
  }, [requestJson]);

  const fetchServices = useCallback(async () => {
    try {
      const data = await requestJson<{ services?: ServiceRecord[] }>('/services');
      setServicesList(data.services || []);
      setErrors(prev => ({ ...prev, services: null }));
    } catch (err) {
      setErrors(prev => ({ ...prev, services: formatError(err) }));
    }
  }, [requestJson]);

  const refreshAll = useCallback(() => {
    void Promise.all([fetchHealth(), fetchStatus(), fetchJobs(), fetchModels(), fetchServices()]);
  }, [fetchHealth, fetchJobs, fetchModels, fetchServices, fetchStatus]);

  useEffect(() => {
    refreshAll();
  }, [refreshAll]);

  useEffect(() => {
    const onHashChange = () => setActivePage(getHashPage());
    window.addEventListener('hashchange', onHashChange);
    return () => window.removeEventListener('hashchange', onHashChange);
  }, []);

  useEffect(() => {
    let closed = false;
    let pollId: number | undefined;

    try {
      const source = new EventSource('/api/events/stream');
      const handleEvent = () => {
        if (!closed) refreshAll();
      };
      ['connected', 'heartbeat', 'job:created', 'job:updated', 'job:cancelled', 'queue:changed', 'mode:changed', 'model:changed', 'service:health', 'hardware:update', 'system:error', 'log:entry'].forEach(event => {
        source.addEventListener(event, handleEvent);
      });
      source.onerror = () => {
        source.close();
        if (!closed && !pollId) {
          pollId = window.setInterval(refreshAll, 4000);
        }
      };
      return () => {
        closed = true;
        source.close();
        if (pollId) window.clearInterval(pollId);
      };
    } catch {
      pollId = window.setInterval(refreshAll, 4000);
      return () => {
        closed = true;
        if (pollId) window.clearInterval(pollId);
      };
    }
  }, [refreshAll]);

  const actions = useMemo<DashboardActions>(() => ({
    refreshAll,
    changeMode: async (mode: string) => {
      try {
        await fetch(`${API}/modes/${mode}`, { method: 'POST' });
        toast(`Switched to ${mode === 'ai' ? 'AI Mode' : mode}`, 'success');
        refreshAll();
      } catch (err) {
        toast(err instanceof Error ? err.message : 'Mode switch failed', 'danger');
      }
    },
    pullModel: async (name: string) => {
      try {
        await requestJson('/models/pull', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ name }) });
        toast(`Pull requested for ${name}`, 'success');
        fetchModels();
      } catch (err) {
        toast(err instanceof Error ? err.message : 'Pull failed', 'danger');
      }
    },
    loadModel: async (model: string) => {
      try {
        await requestJson('/models/load', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ model }) });
        toast(`Loaded ${model}`, 'success');
        fetchModels();
      } catch (err) {
        toast(err instanceof Error ? err.message : 'Load failed', 'danger');
      }
    },
    unloadModel: async (model: string) => {
      try {
        await requestJson('/models/unload', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ model }) });
        toast(`Unloaded ${model}`, 'success');
        fetchModels();
      } catch (err) {
        toast(err instanceof Error ? err.message : 'Unload failed', 'danger');
      }
    },
    rescanModels: async () => {
      try {
        await requestJson('/models/rescan', { method: 'POST' });
        toast('Model inventory refreshed', 'success');
        fetchModels();
      } catch (err) {
        toast(err instanceof Error ? err.message : 'Rescan failed', 'danger');
      }
    },
    deleteModel: async (model: string) => {
      try {
        await fetch(`${API}/models/${encodeURIComponent(model)}`, { method: 'DELETE' });
        toast(`Deleted ${model}`, 'success');
        fetchModels();
      } catch (err) {
        toast(err instanceof Error ? err.message : 'Delete failed', 'danger');
      }
    },
    cancelJob: async (jobId: string) => {
      try {
        await requestJson(`/jobs/${encodeURIComponent(jobId)}/cancel`, { method: 'POST' });
        toast(`Cancelled ${jobId}`, 'warning');
        fetchJobs();
      } catch (err) {
        toast(formatError(err), 'danger');
      }
    },
    retryJob: async (jobId: string) => {
      try {
        await fetch(`${API}/jobs/${encodeURIComponent(jobId)}/retry`, { method: 'POST' });
        toast(`Retry queued for ${jobId}`, 'success');
        fetchJobs();
      } catch (err) {
        toast(err instanceof Error ? err.message : 'Retry failed', 'danger');
      }
    },
    reprioritizeJob: async (jobId: string, priority: number) => {
      try {
        await requestJson(`/jobs/${encodeURIComponent(jobId)}/reprioritize`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ priority }) });
        toast(`Priority updated for ${jobId}`, 'success');
        fetchJobs();
      } catch (err) {
        toast(formatError(err), 'danger');
      }
    },
    restartBackend: async () => {
      try {
        await requestJson('/services/llama-server/restart', { method: 'POST' });
        toast('llama.cpp restart requested', 'success');
        fetchServices();
      } catch (err) {
        toast(err instanceof Error ? err.message : 'Restart failed', 'danger');
      }
    },
    startService: async (id: string) => {
      try {
        await requestJson(`/services/${encodeURIComponent(id)}/start`, { method: 'POST' });
        toast(`Started ${id}`, 'success');
        fetchServices();
      } catch (err) {
        toast(formatError(err), 'danger');
      }
    },
    stopService: async (id: string) => {
      try {
        await requestJson(`/services/${encodeURIComponent(id)}/stop`, { method: 'POST' });
        toast(`Stopped ${id}`, 'warning');
        fetchServices();
      } catch (err) {
        toast(formatError(err), 'danger');
      }
    },
    restartService: async (id: string) => {
      try {
        await requestJson(`/services/${encodeURIComponent(id)}/restart`, { method: 'POST' });
        toast(`Restarted ${id}`, 'success');
        fetchServices();
      } catch (err) {
        toast(formatError(err), 'danger');
      }
    },
    pauseQueue: async () => {
      try {
        await requestJson('/queue/pause', { method: 'POST' });
        toast('Queue pause requested', 'warning');
        fetchStatus();
      } catch (err) {
        toast(formatError(err), 'danger');
      }
    },
    resumeQueue: async () => {
      try {
        await requestJson('/queue/resume', { method: 'POST' });
        toast('Queue resume requested', 'success');
        refreshAll();
      } catch (err) {
        toast(formatError(err), 'danger');
      }
    },
    clearFailedJobs: async () => {
      try {
        await requestJson('/queue/clear-failed', { method: 'POST' });
        toast('Failed jobs cleared', 'success');
        refreshAll();
      } catch (err) {
        toast(formatError(err), 'danger');
      }
    },
    unloadModels: async () => {
      try {
        await Promise.all(modelsList.map(model => requestJson('/models/unload', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ model: model.name }) })));
        toast('Unload requested for loaded models', 'success');
        fetchModels();
      } catch (err) {
        toast(formatError(err), 'danger');
      }
    },
    loadWhisperModel: async (model: string) => {
      try {
        await requestJson('/whisper/load', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ model }) });
        toast(`Loaded Whisper model ${model}`, 'success');
        refreshAll();
      } catch (err) {
        toast(formatError(err), 'danger');
      }
    },
    rescanWhisperModels: async () => {
      try {
        await requestJson('/whisper/rescan', { method: 'POST' });
        toast('Whisper models refreshed', 'success');
        refreshAll();
      } catch (err) {
        toast(formatError(err), 'danger');
      }
    },
    transcribeAudio: async (audio: Blob) => {
      const form = new FormData();
      form.append('file', audio, `dictation-${Date.now()}.wav`);
      form.append('model', 'whisper');
      form.append('response_format', 'json');
      const res = await fetch('/v1/audio/transcriptions', { method: 'POST', body: form });
      if (!res.ok) {
        const body = await res.json().catch(() => ({}));
        throw new Error(formatError(body.error || `HTTP ${res.status}`));
      }
      const body = await res.json() as { text?: string };
      refreshAll();
      return body.text || '';
    },
    toast,
  }), [fetchJobs, fetchModels, fetchServices, fetchStatus, modelsList, refreshAll, requestJson, toast]);

  const state: DashboardState = {
    healthData,
    statusData,
    jobsList,
    modelsList,
    whisperModels,
    runningModels,
    servicesList,
    errors,
    loading,
    connected,
    lastUpdatedAt,
  };

  const pageProps: PageProps = { state, actions };
  const Page = getPage(activePage);

  return (
    <AppShell
      activePage={activePage}
      onNavigate={(page) => {
        window.location.hash = page;
        setActivePage(page);
      }}
      version={healthData?.version || '0.1.0'}
      connected={connected}
      lastUpdatedAt={lastUpdatedAt}
      onMode={actions.changeMode}
      onPauseQueue={actions.pauseQueue}
      onRestartBackend={actions.restartBackend}
    >
      {loading ? <Skeleton /> : <Page {...pageProps} />}
      <ToastStack toasts={toasts} />
    </AppShell>
  );
};

function getHashPage(): PageId {
  const value = window.location.hash.replace('#', '') as PageId;
  return pages.includes(value) ? value : 'overview';
}

function getPage(page: PageId): React.FC<PageProps> {
  const map: Record<PageId, React.FC<PageProps>> = {
    overview: OverviewPage,
    queue: QueuePage,
    jobs: JobsPage,
    models: ModelsPage,
    services: ServicesPage,
    hardware: HardwarePage,
    logs: LogsPage,
    settings: SettingsPage,
  };
  return map[page];
}

const Skeleton: React.FC = () => (
  <div className="space-y-5">
    <div className="h-56 animate-pulse rounded-xl border border-border-slate bg-panel-slate" />
    <div className="grid gap-4 md:grid-cols-2 xl:grid-cols-3 2xl:grid-cols-6">
      {Array.from({ length: 6 }).map((_, index) => <div key={index} className="h-40 animate-pulse rounded-xl border border-border-slate bg-panel-slate" />)}
    </div>
    <div className="grid gap-4 xl:grid-cols-2">
      <div className="h-72 animate-pulse rounded-xl border border-border-slate bg-panel-slate" />
      <div className="h-72 animate-pulse rounded-xl border border-border-slate bg-panel-slate" />
    </div>
  </div>
);

const ToastStack: React.FC<{ toasts: Toast[] }> = ({ toasts }) => (
  <div className="fixed bottom-4 right-4 z-50 space-y-2">
    {toasts.map(toast => (
      <div key={toast.id} className={`max-w-sm rounded-lg border px-4 py-3 text-sm shadow-deck ${
        toast.tone === 'danger' ? 'border-danger-rose/50 bg-danger-rose/15 text-danger-rose' :
        toast.tone === 'warning' ? 'border-warning-amber/50 bg-warning-amber/15 text-warning-amber' :
        toast.tone === 'success' ? 'border-success-green/50 bg-success-green/15 text-success-green' :
        'border-border-slate bg-panel-slate text-text-primary'
      }`}>
        {toast.message}
      </div>
    ))}
  </div>
);

export default App;
