import { MockInferDeck } from "./fixtures/mockInferDeck";
import { describe, it, expect, beforeEach } from "vitest";

describe("message queue - multiple messages", () => {
  let server: MockInferDeck;

  beforeEach(() => {
    server = new MockInferDeck();
  });

  it("should process messages in priority order when enqueued together", () => {
    server.enqueue("lowest", 10);
    server.enqueue("medium", 50);
    server.enqueue("highest", 100);
    server.enqueue("low", 20);
    server.enqueue("high", 80);

    const order = server.processAll();

    expect(order).toEqual(["highest", "high", "medium", "low", "lowest"]);
    expect(server.processed).toHaveLength(5);
  });

  it("should preserve FIFO order for messages with equal priority", () => {
    server.enqueue("first", 50);
    server.enqueue("second", 50);
    server.enqueue("third", 50);

    const order = server.processAll();

    expect(order).toEqual(["first", "second", "third"]);
  });
});
