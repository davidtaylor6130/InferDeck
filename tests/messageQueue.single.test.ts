import { MockInferDeck } from "./fixtures/mockInferDeck";
import { describe, it, expect, beforeEach } from "vitest";

describe("message queue - single message", () => {
  let server: MockInferDeck;

  beforeEach(() => {
    server = new MockInferDeck();
  });

  it("should enqueue and process a single message", () => {
    server.enqueue("msg1", 50);
    expect(server.queueLength).toBe(1);
    const id = server.processNext();
    expect(id).toBe("msg1");
    expect(server.processed).toHaveLength(1);
    expect(server.processed[0].id).toBe("msg1");
  });
});
