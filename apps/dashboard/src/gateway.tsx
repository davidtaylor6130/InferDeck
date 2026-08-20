import React, { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react';
import * as api from './api';
import type {
  ActivityItem,
  ConnectionState,
  MonthlyUsageRow,
  ModelEvent,
  ModelInfo,
  RequestEvent,
  StatsEvent,
  StatusPayload,
  SwapState,
} from './types';
import { compactModel, formatDuration } from './utils';

const HISTORY_LIMIT = 120;
const ACTIVITY_LIMIT = 30;
const FALLBACK_POLL_MS = 30_000;
const RECONNECT_DELAY_MS = 5_000;

export interface GatewayValue {
  connection: ConnectionState;
  lastUpdatedAt: number | null;
  stats: StatsEvent | null;
  statsHistory: StatsEvent[];
  status: StatusPayload | null;
  models: ModelInfo[];
  swap: SwapState;
  activity: ActivityItem[];
  refresh: () => Promise<void>;
  swapTo: (model: string) => Promise<string | null>;
  cancelSwap: () => Promise<string | null>;
  unload: (model?: string) => Promise<string | null>;
}

const idleSwap: SwapState = { swapping: false, target: '', from: '', startedUnixMs: 0, lastError: '' };

export function mergeDailyUsage(
  history: MonthlyUsageRow[], recent: MonthlyUsageRow[],
): MonthlyUsageRow[] {
  const rows = new Map<string, MonthlyUsageRow>();
  for (const row of history) rows.set(`${row.bucket}\0${row.model}`, row);
  for (const row of recent) rows.set(`${row.bucket}\0${row.model}`, row);
  return [...rows.values()].sort((left, right) =>
    left.bucket.localeCompare(right.bucket) || left.model.localeCompare(right.model));
}

export const GatewayContext = createContext<GatewayValue | null>(null);

export function useGateway(): GatewayValue {
  const value = useContext(GatewayContext);
  if (!value) throw new Error('useGateway must be used inside GatewayProvider');
  return value;
}

function requestActivity(event: RequestEvent): ActivityItem {
  const ok = event.status >= 200 && event.status < 300;
  return {
    id: `req-${event.timestampUnixMs}-${Math.random().toString(36).slice(2, 7)}`,
    kind: 'request',
    label: `${ok ? 'Completed' : `Failed (${event.status})`}: ${compactModel(event.model || 'request')}`,
    detail: ok
      ? `${event.promptTokens + event.completionTokens} tokens · ${event.tokensPerSecond.toFixed(1)} t/s · ${formatDuration(event.durationMs)}`
      : 'request did not complete',
    timestampUnixMs: event.timestampUnixMs,
    tone: ok ? 'good' : 'critical',
  };
}

function swapActivity(event: ModelEvent): ActivityItem {
  const labels: Record<ModelEvent['state'], string> = {
    swapping: `Swapping to ${compactModel(event.to)}`,
    waiting: `Waiting to load ${compactModel(event.to)}`,
    ready: `Loaded ${compactModel(event.to)}`,
    failed: `Swap to ${compactModel(event.to)} failed`,
    cancelled: `Swap to ${compactModel(event.to)} cancelled`,
    unloaded: `Unloaded ${compactModel(event.from)}`,
  };
  return {
    id: `swap-${event.timestampUnixMs}-${event.state}`,
    kind: 'swap',
    label: labels[event.state],
    detail: event.state === 'failed' || event.state === 'waiting'
      ? event.error
      : event.durationMs > 0 ? formatDuration(event.durationMs) : '',
    timestampUnixMs: event.timestampUnixMs || Date.now(),
    tone: event.state === 'failed' ? 'critical' : event.state === 'swapping' || event.state === 'waiting' ? 'info' : event.state === 'ready' ? 'good' : 'idle',
  };
}

function parseEvent<T>(event: Event, validate: (value: unknown) => value is T): T | null {
  try {
    const value: unknown = JSON.parse((event as MessageEvent<string>).data);
    return validate(value) ? value : null;
  } catch {
    return null;
  }
}

function isObject(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

function isFiniteNumber(value: unknown): value is number {
  return typeof value === 'number' && Number.isFinite(value);
}

function isStatsEvent(value: unknown): value is StatsEvent {
  if (!isObject(value) || !isObject(value.gpu)) return false;
  return isFiniteNumber(value.timestampUnixMs)
    && typeof value.loadedModel === 'string'
    && typeof value.swapping === 'boolean'
    && typeof value.swapTarget === 'string'
    && isFiniteNumber(value.activeRequests)
    && isFiniteNumber(value.totalRequests)
    && isFiniteNumber(value.totalSwaps)
    && isFiniteNumber(value.lifetimeTokensIn)
    && isFiniteNumber(value.lifetimeTokensOut)
    && isFiniteNumber(value.avgTokensPerSecond)
    && isFiniteNumber(value.uptimeSeconds)
    && typeof value.gpu.available === 'boolean'
    && typeof value.gpu.name === 'string'
    && isFiniteNumber(value.gpu.utilizationPct)
    && isFiniteNumber(value.gpu.vramUsedMb)
    && isFiniteNumber(value.gpu.temperatureC)
    && isFiniteNumber(value.gpu.powerW);
}

function isModelEvent(value: unknown): value is ModelEvent {
  if (!isObject(value)) return false;
  return ['swapping', 'waiting', 'ready', 'failed', 'cancelled', 'unloaded'].includes(String(value.state))
    && typeof value.from === 'string'
    && typeof value.to === 'string'
    && typeof value.error === 'string'
    && isFiniteNumber(value.durationMs)
    && isFiniteNumber(value.timestampUnixMs);
}

function isRequestEvent(value: unknown): value is RequestEvent {
  if (!isObject(value)) return false;
  return typeof value.model === 'string'
    && isFiniteNumber(value.timestampUnixMs)
    && isFiniteNumber(value.promptTokens)
    && isFiniteNumber(value.completionTokens)
    && isFiniteNumber(value.durationMs)
    && isFiniteNumber(value.tokensPerSecond)
    && isFiniteNumber(value.status);
}

export const GatewayProvider: React.FC<{ children: React.ReactNode }> = ({ children }) => {
  const [connection, setConnection] = useState<ConnectionState>('connecting');
  const [lastUpdatedAt, setLastUpdatedAt] = useState<number | null>(null);
  const [stats, setStats] = useState<StatsEvent | null>(null);
  const [statsHistory, setStatsHistory] = useState<StatsEvent[]>([]);
  const [status, setStatus] = useState<StatusPayload | null>(null);
  const [models, setModels] = useState<ModelInfo[]>([]);
  const [swap, setSwap] = useState<SwapState>(idleSwap);
  const [activity, setActivity] = useState<ActivityItem[]>([]);
  const sourceRef = useRef<EventSource | null>(null);
  const reconnectTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const refreshRef = useRef(0);
  const dailyUsageRef = useRef<MonthlyUsageRow[] | null>(null);

  const refresh = useCallback(async () => {
    const request = ++refreshRef.current;
    const [statusResult, modelsResult, dailyResult] = await Promise.allSettled([
      api.getStatus(),
      api.getModels(),
      dailyUsageRef.current === null ? api.getDailyUsage() : Promise.resolve(null),
    ]);
    if (request !== refreshRef.current) return;
    if (dailyResult.status === 'fulfilled' && dailyResult.value) {
      dailyUsageRef.current = dailyResult.value.dailyTokenUsage;
    }
    if (statusResult.status === 'fulfilled') {
      const next = statusResult.value;
      if (dailyUsageRef.current !== null) {
        const merged = mergeDailyUsage(
          dailyUsageRef.current, next.dailyTokenUsage ?? [],
        );
        dailyUsageRef.current = merged;
        setStatus({
          ...next,
          dailyTokenUsage: merged,
          dailyTokenUsageAllTime: true,
        });
      } else {
        setStatus(next);
      }
      setSwap(statusResult.value.swap ?? idleSwap);
      setLastUpdatedAt(Date.now());
    }
    if (modelsResult.status === 'fulfilled') setModels(modelsResult.value);
  }, []);

  useEffect(() => {
    let disposed = false;

    const connect = () => {
      if (disposed || typeof EventSource === 'undefined') return;
      sourceRef.current?.close();
      const source = new EventSource(api.eventStreamUrl());
      sourceRef.current = source;

      source.onopen = () => {
        setConnection('connected');
        void refresh();
      };

      source.onerror = () => {
        if (disposed) return;
        if (source.readyState === EventSource.CLOSED) {
          setConnection('offline');
          if (!reconnectTimer.current) {
            reconnectTimer.current = setTimeout(() => {
              reconnectTimer.current = null;
              connect();
            }, RECONNECT_DELAY_MS);
          }
        } else {
          setConnection('reconnecting');
        }
      };

      source.addEventListener('stats', event => {
        const next = parseEvent(event, isStatsEvent);
        if (!next) return;
        setConnection('connected');
        setStats(next);
        setStatsHistory(history => [...history, next].slice(-HISTORY_LIMIT));
        setLastUpdatedAt(Date.now());
      });

      source.addEventListener('model', event => {
        const next = parseEvent(event, isModelEvent);
        if (!next) return;
        if (next.state === 'swapping') {
          setSwap({ swapping: true, target: next.to, from: next.from, startedUnixMs: next.timestampUnixMs, lastError: '' });
        } else {
          setSwap({ swapping: false, target: next.to, from: next.from, startedUnixMs: 0, lastError: next.state === 'failed' ? next.error : '' });
          void refresh();
        }
        setActivity(items => [swapActivity(next), ...items].slice(0, ACTIVITY_LIMIT));
      });

      source.addEventListener('request', event => {
        const next = parseEvent(event, isRequestEvent);
        if (!next) return;
        setActivity(items => [requestActivity(next), ...items].slice(0, ACTIVITY_LIMIT));
      });
    };

    connect();
    void refresh();

    const fallback = setInterval(() => {
      void refresh();
    }, FALLBACK_POLL_MS);

    return () => {
      disposed = true;
      clearInterval(fallback);
      if (reconnectTimer.current) clearTimeout(reconnectTimer.current);
      sourceRef.current?.close();
    };
  }, [refresh]);

  const swapToAction = useCallback(async (model: string) => {
    try {
      await api.swapTo(model);
      return null;
    } catch (error) {
      return error instanceof Error ? error.message : String(error);
    }
  }, []);

  const cancelSwapAction = useCallback(async () => {
    try {
      await api.cancelSwap();
      return null;
    } catch (error) {
      return error instanceof Error ? error.message : String(error);
    }
  }, []);

  const unloadAction = useCallback(async (model?: string) => {
    try {
      await api.unloadModel(model);
      await refresh();
      return null;
    } catch (error) {
      return error instanceof Error ? error.message : String(error);
    }
  }, [refresh]);

  const value = useMemo<GatewayValue>(() => ({
    connection,
    lastUpdatedAt,
    stats,
    statsHistory,
    status,
    models,
    swap,
    activity,
    refresh,
    swapTo: swapToAction,
    cancelSwap: cancelSwapAction,
    unload: unloadAction,
  }), [connection, lastUpdatedAt, stats, statsHistory, status, models, swap, activity, refresh, swapToAction, cancelSwapAction, unloadAction]);

  return <GatewayContext.Provider value={value}>{children}</GatewayContext.Provider>;
};
