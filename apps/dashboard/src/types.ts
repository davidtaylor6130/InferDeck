export type PageId = 'overview' | 'queue' | 'jobs' | 'models' | 'services' | 'hardware' | 'logs' | 'settings';

export type ServiceStatus = 'unknown' | 'not_configured' | 'starting' | 'ready' | 'running' | 'healthy' | 'stopped' | 'error' | 'unhealthy' | 'offline';

export interface ServiceRecord {
  id: string;
  name?: string;
  kind: string;
  status: ServiceStatus;
  pid?: number | null;
  baseUrl?: string | null;
  version?: string | null;
  managed?: boolean;
  lastError?: string | null;
  lastHealthcheckAt?: string | null;
}

export interface JobRecord {
  id: string;
  type: string;
  status: string;
  priority: number;
  client?: string;
  model?: string;
  resourceClass?: string;
  resource_class?: string;
  created_at?: string;
  createdAt?: string;
  started_at?: string;
  startedAt?: string;
  completed_at?: string;
  completedAt?: string;
  payload?: unknown;
  result?: unknown;
  error?: string | null;
  promptTokens?: number;
  completionTokens?: number;
  totalTokens?: number;
  durationMs?: number;
  httpStatus?: number;
  events?: Array<{ eventType?: string; message?: string; createdAt?: string; data?: unknown }>;
}

export interface ModelRecord {
  id?: string;
  name: string;
  path?: string;
  size?: number;
  digest?: string;
  loaded?: boolean;
  aliases?: string[];
  modified_at?: string;
  details?: {
    parameter_size?: string;
    parent_model?: string;
    format?: string;
    quantization_level?: string;
  };
}

export interface DashboardState {
  healthData: Record<string, any> | null;
  statusData: Record<string, any> | null;
  jobsList: JobRecord[];
  modelsList: ModelRecord[];
  runningModels: ModelRecord[];
  servicesList: ServiceRecord[];
  errors: Record<string, string | null>;
  loading: boolean;
  connected: boolean;
  lastUpdatedAt: Date | null;
}

export interface DashboardActions {
  refreshAll: () => void;
  changeMode: (mode: string) => Promise<void>;
  pullModel: (name: string) => Promise<void>;
  loadModel: (model: string) => Promise<void>;
  unloadModel: (model: string) => Promise<void>;
  rescanModels: () => Promise<void>;
  deleteModel: (model: string) => Promise<void>;
  cancelJob: (jobId: string) => Promise<void>;
  retryJob: (jobId: string) => Promise<void>;
  reprioritizeJob: (jobId: string, priority: number) => Promise<void>;
  restartBackend: () => Promise<void>;
  startService: (id: string) => Promise<void>;
  stopService: (id: string) => Promise<void>;
  restartService: (id: string) => Promise<void>;
  pauseQueue: () => Promise<void>;
  resumeQueue: () => Promise<void>;
  clearFailedJobs: () => Promise<void>;
  unloadModels: () => Promise<void>;
  toast: (message: string, tone?: 'success' | 'warning' | 'danger' | 'info') => void;
}

export interface PageProps {
  state: DashboardState;
  actions: DashboardActions;
}
