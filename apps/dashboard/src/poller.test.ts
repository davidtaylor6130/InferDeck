import { afterEach, describe, expect, it, vi } from 'vitest';
import { createPoller } from './poller';

function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (error: unknown) => void;
  const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}

afterEach(() => vi.useRealTimers());

describe('sequential polling', () => {
  it('waits for a slow request before scheduling another', async () => {
    vi.useFakeTimers();
    const slow = deferred<number>();
    const load = vi.fn().mockReturnValueOnce(slow.promise).mockResolvedValue(2);
    const receive = vi.fn();
    const poller = createPoller(load, receive, vi.fn(), 1000);
    const initial = poller.refresh();
    await vi.advanceTimersByTimeAsync(5000);
    expect(load).toHaveBeenCalledTimes(1);
    slow.resolve(1);
    await initial;
    expect(receive).toHaveBeenLastCalledWith(1);
    await vi.advanceTimersByTimeAsync(999);
    expect(load).toHaveBeenCalledTimes(1);
    await vi.advanceTimersByTimeAsync(1);
    expect(load).toHaveBeenCalledTimes(2);
    poller.stop();
  });

  it('coalesces explicit refreshes and ignores the superseded response', async () => {
    const slow = deferred<number>();
    const load = vi.fn().mockReturnValueOnce(slow.promise).mockResolvedValue(2);
    const receive = vi.fn();
    const poller = createPoller(load, receive, vi.fn(), 1000);
    const initial = poller.refresh();
    const signal = load.mock.calls[0][0] as AbortSignal;
    const refreshed = poller.refresh();
    void poller.refresh();
    expect(signal.aborted).toBe(true);
    expect(load).toHaveBeenCalledTimes(1);
    slow.resolve(1);
    await Promise.all([initial, refreshed]);
    expect(load).toHaveBeenCalledTimes(2);
    expect(receive.mock.calls).toEqual([[2]]);
    poller.stop();
  });

  it('aborts on cleanup and ignores late results and errors', async () => {
    for (const rejects of [false, true]) {
      const slow = deferred<number>();
      const receive = vi.fn();
      const failed = vi.fn();
      let signal!: AbortSignal;
      const poller = createPoller(value => { signal = value; return slow.promise; }, receive, failed, 1000);
      const initial = poller.refresh();
      poller.stop();
      expect(signal.aborted).toBe(true);
      if (rejects) slow.reject(new Error('late error'));
      else slow.resolve(1);
      await initial;
      expect(receive).not.toHaveBeenCalled();
      expect(failed).not.toHaveBeenCalled();
    }
  });

  it('pauses while hidden and refreshes on visibility without stale writes', async () => {
    vi.useFakeTimers();
    const slow = deferred<number>();
    const receive = vi.fn();
    const load = vi.fn().mockReturnValueOnce(slow.promise).mockResolvedValue(2);
    const poller = createPoller(load, receive, vi.fn(), 1000);
    const initial = poller.refresh();
    poller.setPaused(true);
    await vi.advanceTimersByTimeAsync(5000);
    expect(load).toHaveBeenCalledTimes(1);
    slow.resolve(1);
    await initial;
    expect(receive).not.toHaveBeenCalled();
    poller.setPaused(false);
    await poller.refresh();
    expect(receive).toHaveBeenLastCalledWith(2);
    poller.stop();
  });

  it('retries after a request fails', async () => {
    vi.useFakeTimers();
    const load = vi.fn().mockRejectedValueOnce(new Error('offline')).mockResolvedValue(2);
    const failed = vi.fn();
    const receive = vi.fn();
    const poller = createPoller(load, receive, failed, 1000);
    await poller.refresh();
    expect(failed).toHaveBeenCalledOnce();
    await vi.advanceTimersByTimeAsync(1000);
    expect(receive).toHaveBeenLastCalledWith(2);
    poller.stop();
  });
});
