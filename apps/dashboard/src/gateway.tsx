import React, { createContext, useCallback, useContext, useEffect, useMemo, useRef, useState } from 'react';
import * as api from './api';
import type {
  ActivityItem,
  ConnectionState,
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
  unload: () => Promise<string | null>;
}

const idleSwap: SwapState = { swapping: false, target: '', from: '', startedUnixMs: 0, lastError: '' };

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
    ready: `Loaded ${compactModel(event.to)}`,
    failed: `Swap to ${compactModel(event.to)} failed`,
    cancelled: `Swap to ${compactModel(event.to)} cancelled`,
    unloaded: `Unloaded ${compactModel(event.from)}`,
  };
  return {
    id: `swap-${event.timestampUnixMs}-${event.state}`,
    kind: 'swap',
    label: labels[event.state],
    detail: event.state === 'failed' ? event.error : event.durationMs > 0 ? formatDuration(event.durationMs) : '',
    timestampUnixMs: event.timestampUnixMs || Date.now(),
    tone: event.state === 'failed' ? 'critical' : event.state === 'swapping' ? 'info' : event.state === 'ready' ? 'good' : 'idle',
  };
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

  const refresh = useCallback(async () => {
    try {
      const [nextStatus, nextModels] = await Promise.all([api.getStatus(), api.getModels()]);
      setStatus(nextStatus);
      setModels(nextModels);
      setSwap(nextStatus.swap ?? idleSwap);
      setLastUpdatedAt(Date.now());
    } catch {
      // SSE connection state drives the offline banner; a failed refresh keeps stale data.
    }
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
        const next = JSON.parse((event as MessageEvent<string>).data) as StatsEvent;
        setConnection('connected');
        setStats(next);
        setStatsHistory(history => [...history, next].slice(-HISTORY_LIMIT));
        setLastUpdatedAt(Date.now());
      });

      source.addEventListener('model', event => {
        const next = JSON.parse((event as MessageEvent<string>).data) as ModelEvent;
        if (next.state === 'swapping') {
          setSwap({ swapping: true, target: next.to, from: next.from, startedUnixMs: next.timestampUnixMs, lastError: '' });
        } else {
          setSwap({ swapping: false, target: next.to, from: next.from, startedUnixMs: 0, lastError: next.error || '' });
          void refresh();
        }
        setActivity(items => [swapActivity(next), ...items].slice(0, ACTIVITY_LIMIT));
      });

      source.addEventListener('request', event => {
        const next = JSON.parse((event as MessageEvent<string>).data) as RequestEvent;
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

  const unloadAction = useCallback(async () => {
    try {
      await api.unloadModel();
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
