/**
 * Scheduler unit tests
 */

import { Scheduler } from "../../packages/gateway-core/src/Scheduler";
import { QueueStore } from "../../packages/gateway-core/src/QueueStore";
import { describe, it, expect, beforeEach } from "vitest";

describe("Scheduler", () => {
  let scheduler: Scheduler;
  let queueStore: QueueStore;

  beforeEach(() => {
    queueStore = new QueueStore();
    scheduler = new Scheduler({
      config: {},
      mode: "ai",
      queueStore,
    });
  });

  it("should start in AI mode", () => {
    expect(scheduler.getMode()).toBe("ai");
  });

  it("should allow mode switching", () => {
    scheduler.setMode("gaming");
    expect(scheduler.getMode()).toBe("gaming");
    scheduler.setMode("ai");
    expect(scheduler.getMode()).toBe("ai");
  });

  it("should queue and dequeue jobs", () => {
    scheduler.enqueue({
      id: "job1",
      type: "llm_chat",
      payload: {},
      resourceClass: "gpu_llm",
      status: "queued",
      priority: 50,
    });
    const next = scheduler.getNextJob();
    expect(next?.id).toBe("job1");
  });

  it("should pause and resume queue", () => {
    scheduler.enqueue({
      id: "j1",
      type: "llm_chat",
      payload: {},
      resourceClass: "cpu",
      status: "queued",
      priority: 50,
    });
    scheduler.pauseQueue();
    expect(queueStore.length).toBe(0);
    scheduler.resumeQueue();
    expect(queueStore.length).toBe(1);
  });
});
