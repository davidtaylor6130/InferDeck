import { useCallback, useEffect, useRef } from 'react';
import { createPoller } from './poller';

export function usePolling<T>(
  load: (signal: AbortSignal) => Promise<T>,
  receive: (value: T) => void,
  failed: (error: unknown) => void,
  intervalMs: number,
  refreshKey: string | number = '',
) {
  const poller = useRef<ReturnType<typeof createPoller<T>> | null>(null);
  useEffect(() => {
    const current = createPoller(load, receive, failed, intervalMs);
    poller.current = current;
    const visibilityChanged = () => current.setPaused(document.hidden);
    document.addEventListener('visibilitychange', visibilityChanged);
    visibilityChanged();
    void current.refresh();
    return () => {
      document.removeEventListener('visibilitychange', visibilityChanged);
      current.stop();
      if (poller.current === current) poller.current = null;
    };
  }, [load, receive, failed, intervalMs, refreshKey]);
  return useCallback(() => poller.current?.refresh() ?? Promise.resolve(), []);
}
