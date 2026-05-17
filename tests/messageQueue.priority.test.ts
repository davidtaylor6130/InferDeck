import { MockInferDeck } from "./fixtures/mockInferDeck";
import { describe, it, expect, beforeEach } from "vitest";

describe("message queue - priority preemption", () => {
  let server: MockInferDeck;

  beforeEach(() => {
    server = new MockInferDeck();
  });

  it("should process high-priority message before low-priority even when low was enqueued first", () => {
    server.enqueue("low", 10);
    server.enqueue("high", 100);

    expect(server.processNext()).toBe("high");
    expect(server.processNext()).toBe("low");
  });

  it("should handle multiple priority levels with interleaved enqueue", () => {
    server.enqueue("batch1-low", 10);
    server.enqueue("batch1-high", 90);
    server.enqueue("batch1-mid", 50);
    server.enqueue("medium", 60);
    server.enqueue("urgent", 100);

    expect(server.processNext()).toBe("urgent");
    expect(server.processNext()).toBe("batch1-high");
    expect(server.processNext()).toBe("medium");
    expect(server.processNext()).toBe("batch1-mid");
    expect(server.processNext()).toBe("batch1-low");
  });

  it("should handle preemption with three priority levels", () => {
    server.enqueue("low", 1);
    server.enqueue("medium", 50);
    server.enqueue("high", 99);

    expect(server.processNext()).toBe("high");
    expect(server.processNext()).toBe("medium");
    expect(server.processNext()).toBe("low");
  });
});
