import { QueueStore } from "../../packages/gateway-core/src/QueueStore";

export interface ProcessedMessage {
  id: string;
  priority: number;
  processedAt: number;
}

export class MockInferDeck {
  private store: QueueStore;
  public processed: ProcessedMessage[] = [];

  constructor() {
    this.store = new QueueStore();
  }

  enqueue(id: string, priority: number): void {
    this.store.enqueue({ id, priority });
  }

  processNext(): string | null {
    const item = this.store.dequeue();
    if (!item) return null;
    this.store.lease(item.id);
    this.processed.push({
      id: item.id,
      priority: item.priority,
      processedAt: Date.now(),
    });
    return item.id;
  }

  processAll(): string[] {
    const ids: string[] = [];
    let id;
    while ((id = this.processNext()) !== null) {
      ids.push(id);
    }
    return ids;
  }

  get queueLength(): number {
    return this.store.length;
  }

  reset(): void {
    this.processed = [];
    this.store = new QueueStore();
  }
}
