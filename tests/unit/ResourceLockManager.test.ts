/**
 * ResourceLockManager unit tests
 */

import { ResourceLockManager } from "../../packages/gateway-core/src/ResourceLockManager";
import { describe, it, expect, beforeEach } from "vitest";

describe("ResourceLockManager", () => {
  let manager: ResourceLockManager;

  beforeEach(() => {
    manager = new ResourceLockManager();
  });

  it("should allow lock acquisition", () => {
    const result = manager.acquire("job1", "gpu_llm");
    expect(result.locked).toBe(true);
    expect(manager.lockCount).toBe(1);
  });

  it("should block conflicting locks", () => {
    manager.acquire("job1", "gpu_llm");
    const result2 = manager.acquire("job2", "gpu_image");
    expect(result2.locked).toBe(false);
  });

  it("should release locks", () => {
    manager.acquire("job1", "gpu_llm");
    manager.release("job1");
    expect(manager.lockCount).toBe(0);
  });

  it("should not block non-GPU jobs with GPU lock", () => {
    manager.acquire("gpu-job", "gpu_llm");
    const result = manager.acquire("cpu-job", "cpu");
    expect(result.locked).toBe(true);
  });

  it("should handle gaming mode", () => {
    manager.setGamingMode({
      active: true,
      rejectInteractiveLlm: true,
      pauseBackgroundJobs: true,
      unloadOllamaModels: true,
      stopComfyUi: true,
    });
    expect(manager.isGamingModeActive()).toBe(true);
    expect(manager.shouldRejectInteractiveLlm()).toBe(true);
  });
});
