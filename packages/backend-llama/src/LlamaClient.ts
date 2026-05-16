import type {
  LlamaChatRequest,
  LlamaChatResponse,
  LlamaCompletionRequest,
  LlamaCompletionResponse,
  LlamaEmbedRequest,
  LlamaEmbedResponse,
  LlamaHealthResponse,
} from "./types.js";

export interface LlamaClientOptions {
  baseUrl: string;
  timeoutMs?: number;
  retries?: number;
}

export class LlamaClient {
  private baseUrl: string;
  private timeoutMs: number;
  private retries: number;

  constructor(options: LlamaClientOptions) {
    this.baseUrl = options.baseUrl.replace(/\/$/, "");
    this.timeoutMs = options.timeoutMs ?? 30000;
    this.retries = options.retries ?? 2;
  }

  private url(path: string): string {
    return `${this.baseUrl}${path}`;
  }

  private async fetchWithRetry<T>(
    path: string,
    options: RequestInit,
    retryCount: number = this.retries
  ): Promise<T> {
    try {
      const controller = new AbortController();
      const timeout = setTimeout(() => controller.abort(), this.timeoutMs);

      const res = await fetch(this.url(path), {
        ...options,
        signal: controller.signal,
        headers: {
          "Content-Type": "application/json",
          ...options.headers,
        },
      });

      clearTimeout(timeout);

      if (!res.ok) {
        const body = await res.text();
        let errorStr = body;
        try {
          const parsed = JSON.parse(body);
          errorStr = parsed.error?.message ?? parsed.error ?? body;
        } catch {
          // keep original
        }
        const err = new Error(`llama.cpp error ${res.status}: ${errorStr}`) as any;
        err.status = res.status;
        throw err;
      }

      return res.json() as Promise<T>;
    } catch (err: any) {
      if (err.name === "AbortError") {
        throw new Error(`llama.cpp request timeout(${this.timeoutMs}ms): ${path}`);
      }
      if (retryCount > 0 && !err.status) {
        await new Promise((r) => setTimeout(r, 1000));
        return this.fetchWithRetry(path, options, retryCount - 1);
      }
      throw err;
    }
  }

  private async stream(
    path: string,
    options: RequestInit
  ): Promise<ReadableStream<Uint8Array>> {
    const res = await fetch(this.url(path), {
      ...options,
      headers: {
        "Content-Type": "application/json",
        ...options.headers,
      },
    });

    if (!res.ok) {
      const body = await res.text();
      let errorStr = body;
      try {
        errorStr = JSON.parse(body).error?.message ?? JSON.parse(body).error ?? body;
      } catch {
        // keep original
      }
      throw new Error(`llama.cpp stream error ${res.status}: ${errorStr}`);
    }

    if (!res.body) {
      throw new Error("llama.cpp stream response has no body");
    }

    return res.body;
  }

  async health(): Promise<LlamaHealthResponse> {
    return this.fetchWithRetry<LlamaHealthResponse>("/health", { method: "GET" });
  }

  async ping(): Promise<boolean> {
    try {
      await this.fetchWithRetry<any>("/health", { method: "GET" });
      return true;
    } catch {
      return false;
    }
  }

  async chat(
    request: LlamaChatRequest,
    stream: boolean = false
  ): Promise<ReadableStream<Uint8Array> | LlamaChatResponse> {
    if (stream) {
      return this.stream("/v1/chat/completions", {
        method: "POST",
        body: JSON.stringify({ ...request, stream: true }),
      });
    }
    return this.fetchWithRetry<LlamaChatResponse>("/v1/chat/completions", {
      method: "POST",
      body: JSON.stringify({ ...request, stream: false }),
    });
  }

  async completions(
    request: LlamaCompletionRequest,
    stream: boolean = false
  ): Promise<ReadableStream<Uint8Array> | LlamaCompletionResponse> {
    if (stream) {
      return this.stream("/v1/completions", {
        method: "POST",
        body: JSON.stringify({ ...request, stream: true }),
      });
    }
    return this.fetchWithRetry<LlamaCompletionResponse>("/v1/completions", {
      method: "POST",
      body: JSON.stringify({ ...request, stream: false }),
    });
  }

  async embed(request: LlamaEmbedRequest): Promise<LlamaEmbedResponse> {
    return this.fetchWithRetry<LlamaEmbedResponse>("/v1/embeddings", {
      method: "POST",
      body: JSON.stringify(request),
    });
  }

  async rawFetch(path: string, options: RequestInit): Promise<Response> {
    const res = await fetch(this.url(path), {
      ...options,
      headers: {
        "Content-Type": "application/json",
        ...options.headers,
      },
    });
    if (!res.ok) {
      const body = await res.text();
      const err = new Error(`llama.cpp ${res.status}: ${body}`) as any;
      err.status = res.status;
      throw err;
    }
    return res;
  }
}
