/**
 * Backend client for Ollama API.
 */

import type {
  OllamaChatRequest,
  OllamaGenerateRequest,
  OllamaEmbedRequest,
  OllamaChatResponse,
  OllamaGenerateResponse,
  OllamaEmbedResponse,
  OllamaModelInfo,
  OllamaModelDetails,
  OllamaRunningModel,
} from "./types.js";

export interface OllamaClientOptions {
  baseUrl: string;
  timeoutMs?: number;
  retries?: number;
}

interface OllamaHttpError {
  error: string;
  status?: number;
}

export class OllamaClient {
  private baseUrl: string;
  private timeoutMs: number;
  private retries: number;

  constructor(options: OllamaClientOptions) {
    this.baseUrl = options.baseUrl.replace(/\/$/, "");
    this.timeoutMs = options.timeoutMs ?? 30000;
    this.retries = options.retries ?? 2;
  }

  private url(path: string): string {
    return `${this.baseUrl}${path}`;
  }

  private async fetchWithRetry<T>(path: string, options: RequestInit, retryCount: number = this.retries): Promise<T> {
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
          errorStr = parsed.error ?? body;
        } catch {
          // keep original
        }
        const err = new Error(`Ollama error ${res.status}: ${errorStr}`) as any;
        err.status = res.status;
        throw err;
      }

      return res.json() as Promise<T>;
    } catch (err: any) {
      if (err.name === "AbortError") {
        throw new Error(`Ollama request timeout(${this.timeoutMs}ms): ${path}`);
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
        errorStr = JSON.parse(body).error ?? body;
      } catch {
        // keep original
      }
      throw new Error(`Ollama stream error ${res.status}: ${errorStr}`);
    }

    if (!res.body) {
      throw new Error("Ollama stream response has no body");
    }

    return res.body;
  }

  // ─── Health & Version ─────

  async health(): Promise<{ status: string; gpu: string[] }> {
    const data = await this.fetchWithRetry<any>("/api/tags", { method: "GET" });
    return {
      status: "healthy",
      gpu: data.gpu ?? [],
    };
  }

  async ping(): Promise<boolean> {
    try {
      await this.fetchWithRetry<any>("/api/tags", { method: "GET" });
      return true;
    } catch {
      return false;
    }
  }

  async getVersion(): Promise<{ version: string }> {
    return this.fetchWithRetry("/api/version", { method: "GET" });
  }

  // ─── Models ─────

  async listModels(): Promise<OllamaModelInfo[]> {
    const res = await this.fetchWithRetry<{ models: OllamaModelInfo[] }>("/api/tags", { method: "GET" });
    return res.models;
  }

  async getModelInfo(name: string): Promise<OllamaModelInfo | undefined> {
    const models = await this.listModels();
    return models.find((m) => m.name === name || m.name.startsWith(`${name}:`));
  }

  async listRunningModels(): Promise<OllamaRunningModel[]> {
    const res = await this.fetchWithRetry<{ models: OllamaRunningModel[] }>("/api/ps", { method: "GET" });
    return res.models ?? [];
  }

  async showModel(name: string): Promise<OllamaModelInfo> {
    return this.fetchWithRetry("/api/show", {
      method: "POST",
      body: JSON.stringify({ name }),
    });
  }

  async pullModel(name: string, stream: boolean = false): Promise<ReadableStream<Uint8Array> | any> {
    const path = `/api/pull?stream=${stream ? "true" : "false"}`;
    if (stream) {
      const res = await fetch(this.url(path), {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ name, stream: true }),
      });
      if (!res.body) throw new Error("No stream body");
      return res.body;
    }
    return this.fetchWithRetry(path.replace("?stream=false", ""), {
      method: "POST",
      body: JSON.stringify({ name, stream: false }),
    });
  }

  async loadModel(name: string, keepAlive: string = "5m"): Promise<any> {
    return this.fetchWithRetry("/api/pull", {
      method: "POST",
      body: JSON.stringify({ name, keep_alive: keepAlive }),
    });
  }

  async unloadModel(name: string): Promise<any> {
    return this.fetchWithRetry("/api/generate", {
      method: "POST",
      body: JSON.stringify({ model: name, prompt: "", stream: false, keep_alive: 0 }),
    });
  }

  async deleteModel(name: string): Promise<any> {
    return this.fetchWithRetry(`/api/delete?model=${encodeURIComponent(name)}`, {
      method: "DELETE",
    });
  }

  // ─── Chat ─────

  async chat(request: OllamaChatRequest, stream: boolean = false): Promise<ReadableStream<Uint8Array> | any> {
    const path = `/api/chat?stream=${stream ? "true" : "false"}`;
    if (stream) {
      return this.stream(path, {
        method: "POST",
        body: JSON.stringify(request),
      });
    }
    return this.fetchWithRetry(path.replace("?stream=false", ""), {
      method: "POST",
      body: JSON.stringify({ ...request, stream: false }),
    });
  }

  async generateChatStream(request: OllamaChatRequest): Promise<ReadableStream<Uint8Array>> {
    const data: OllamaChatRequest = {
      model: request.model,
      messages: request.messages ?? [],
      stream: true,
      options: request.options,
      keep_alive: request.keep_alive,
    };
    return this.stream("/api/chat", {
      method: "POST",
      body: JSON.stringify(data),
    });
  }

  // ─── Generate ─────

  async generate(request: OllamaGenerateRequest, stream: boolean = false): Promise<ReadableStream<Uint8Array> | any> {
    const path = `/api/generate?stream=${stream ? "true" : "false"}`;
    if (stream) {
      return this.stream(path, {
        method: "POST",
        body: JSON.stringify(request),
      });
    }
    return this.fetchWithRetry(path.replace("?stream=false", ""), {
      method: "POST",
      body: JSON.stringify({ ...request, stream: false }),
    });
  }

  async generateStream(request: OllamaGenerateRequest): Promise<ReadableStream<Uint8Array>> {
    const data: OllamaGenerateRequest = {
      model: request.model,
      prompt: request.prompt,
      stream: true,
      options: request.options,
      keep_alive: request.keep_alive,
    };
    return this.stream("/api/generate", {
      method: "POST",
      body: JSON.stringify(data),
    });
  }

  // ─── Embed ─────

  async embed(request: OllamaEmbedRequest): Promise<OllamaEmbedResponse> {
    return this.fetchWithRetry("/api/embed", {
      method: "POST",
      body: JSON.stringify(request),
    });
  }

  async embedKeepAlive(request: OllamaEmbedRequest): Promise<OllamaEmbedResponse> {
    return this.fetchWithRetry("/api/embed", {
      method: "POST",
      body: JSON.stringify({ ...request, keep_alive: 300 }),
    });
  }

  async embeddings(request: OllamaEmbedRequest): Promise<OllamaEmbedResponse> {
    return this.fetchWithRetry("/api/embed", {
      method: "POST",
      body: JSON.stringify(request),
    });
  }

  // ─── Raw fetch helper ─────

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
      const err = new Error(`Ollama ${res.status}: ${body}`) as any;
      err.status = res.status;
      throw err;
    }
    return res;
  }
}
