export function createPoller<T>(
  load: (signal: AbortSignal) => Promise<T>,
  receive: (value: T) => void,
  failed: (error: unknown) => void,
  intervalMs: number,
) {
  let stopped = false;
  let paused = false;
  let pending = false;
  let current: Promise<void> | undefined;
  let controller: AbortController | undefined;
  let timer: ReturnType<typeof setTimeout> | undefined;

  const clearTimer = () => {
    clearTimeout(timer);
    timer = undefined;
  };
  const run = async () => {
    while (pending && !stopped && !paused) {
      pending = false;
      const request = new AbortController();
      controller = request;
      try {
        const value = await load(request.signal);
        if (!stopped && !paused && !request.signal.aborted) receive(value);
      } catch (error) {
        if (!stopped && !paused && !request.signal.aborted) failed(error);
      } finally {
        if (controller === request) controller = undefined;
      }
    }
  };
  const refresh = (): Promise<void> => {
    if (stopped || paused) return Promise.resolve();
    clearTimer();
    pending = true;
    controller?.abort();
    if (!current) {
      current = run().finally(() => {
        current = undefined;
        if (!stopped && !paused) {
          if (pending) void refresh();
          else timer = setTimeout(() => { void refresh(); }, intervalMs);
        }
      });
    }
    return current;
  };
  return {
    refresh,
    setPaused(value: boolean) {
      if (paused === value || stopped) return;
      paused = value;
      clearTimer();
      if (paused) controller?.abort();
      else void refresh();
    },
    stop() {
      stopped = true;
      pending = false;
      clearTimer();
      controller?.abort();
    },
  };
}
