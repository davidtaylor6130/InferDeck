export type InferDeckEventType =
  | "job:created"
  | "job:updated"
  | "job:cancelled"
  | "queue:changed"
  | "mode:changed"
  | "model:changed"
  | "service:health"
  | "hardware:update"
  | "system:error"
  | "log:entry";

export interface InferDeckEvent {
  type: InferDeckEventType;
  data: Record<string, unknown>;
  timestamp: string;
}

type Listener = (event: InferDeckEvent) => void;

export class EventBus {
  private listeners = new Set<Listener>();

  emit(type: InferDeckEventType, data: Record<string, unknown> = {}): void {
    const event = { type, data, timestamp: new Date().toISOString() };
    for (const listener of this.listeners) listener(event);
  }

  subscribe(listener: Listener): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }
}
