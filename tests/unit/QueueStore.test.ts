/**
 * QueueStore unit tests
 */

import { QueueStore } from "../../packages/gateway-core/src/QueueStore";
import { describe, it, expect, beforeEach } from "vitest";

describe("QueueStore", () => {
  let store: QueueStore;

  beforeEach(() => {
    store = new QueueStore();
  });

  it("should enqueue items", () => {
    store.enqueue({ id: "1", priority: 50 });
    store.enqueue({ id: "2", priority: 30 });
    expect(store.length).toBe(2);
  });

  it("should prioritize high priority items", () => {
    store.enqueue({ id: "low", priority: 10 });
    store.enqueue({ id: "high", priority: 100 });
    const next = store.dequeue();
    expect(next?.id).toBe("high");
  });

  it("should return undefined when empty", () => {
    expect(store.dequeue()).toBeUndefined();
  });

  it("should track all states", () => {
    store.enqueue({ id: "q", priority: 1, status: "queued" });
    store.enqueue({ id: "l", priority: 2, status: "leased" });
    store.enqueue({ id: "p", priority: 3, status: "paused" });

    expect(store.length).toBe(1); // only queued
    expect(store.getLeased().length).toBe(1);
    expect(store.getPaused().length).toBe(1);
  });
});
