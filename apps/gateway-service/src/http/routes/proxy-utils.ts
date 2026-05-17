import type { FastifyRequest } from "fastify";
import type { AppContext } from "../../app";

export function assertNotSelfProxy(req: FastifyRequest, backendBaseUrl: string): void {
  const hostHeader = req.headers.host;
  if (!hostHeader) return;

  const backend = new URL(backendBaseUrl);
  const backendPort = backend.port || (backend.protocol === "https:" ? "443" : "80");
  const backendHostPort = `${backend.hostname}:${backendPort}`;

  if (hostHeader === backendHostPort) {
    throw new Error(`Backend URL points back at the gateway (${backendBaseUrl})`);
  }
}

/**
 * Ensure the backend has the requested model loaded.
 * If a different model is loaded, it unloads and loads the requested one.
 */
export async function ensureModel(bodyModel: string | undefined, ctx: AppContext): Promise<void> {
  if (!bodyModel || !ctx?.backend) return;
  await ctx.backend.loadModel(bodyModel);
  for (let i = 0; i < 120; i++) {
    const health = await ctx.backend.checkHealth();
    if (health.healthy) return;
    await new Promise((r) => setTimeout(r, 1000));
  }
  throw new Error(`Model "${bodyModel}" failed to load within 120s`);
}

// --- Ollama ↔ OpenAI format translation ---

interface OllamaMessage {
  role: string;
  content: string;
  images?: string[];
}

interface OllamaChatResponse {
  model: string;
  created_at: string;
  message: OllamaMessage;
  done: boolean;
  done_reason?: string;
  total_duration?: number;
  load_duration?: number;
  prompt_eval_count?: number;
  prompt_eval_duration?: number;
  eval_count?: number;
  eval_duration?: number;
}

/**
 * Convert a non-streaming OpenAI /v1/chat/completions response
 * to Ollama /api/chat response format.
 */
export function toOllamaChatResponse(openai: any, model: string): OllamaChatResponse {
  const choice = openai.choices?.[0] ?? {};
  return {
    model,
    created_at: openai.created
      ? new Date(openai.created * 1000).toISOString().replace("Z", "000000Z")
      : new Date().toISOString(),
    message: choice.message ?? { role: "assistant", content: "" },
    done: true,
    done_reason: choice.finish_reason === "stop" ? "stop" : (choice.finish_reason ?? "stop"),
    total_duration: 0,
    load_duration: 0,
    prompt_eval_count: openai.usage?.prompt_tokens ?? 0,
    prompt_eval_duration: 0,
    eval_count: openai.usage?.completion_tokens ?? 0,
    eval_duration: 0,
  };
}

/**
 * Convert a non-streaming OpenAI /v1/completions response
 * to Ollama /api/generate response format.
 */
export function toOllamaGenerateResponse(openai: any, model: string): Record<string, unknown> {
  const choice = openai.choices?.[0] ?? {};
  return {
    model,
    created_at: openai.created
      ? new Date(openai.created * 1000).toISOString().replace("Z", "000000Z")
      : new Date().toISOString(),
    response: choice.text ?? "",
    done: true,
    done_reason: choice.finish_reason === "stop" ? "stop" : (choice.finish_reason ?? "stop"),
    total_duration: 0,
    load_duration: 0,
    prompt_eval_count: openai.usage?.prompt_tokens ?? 0,
    prompt_eval_duration: 0,
    eval_count: openai.usage?.completion_tokens ?? 0,
    eval_duration: 0,
  };
}

/**
 * Create a TransformStream that converts OpenAI SSE chunks (data: {...})
 * to Ollama NDJSON lines for /api/chat streaming.
 */
export function openaiSseToOllamaChatStream(model: string): TransformStream<Uint8Array, Uint8Array> {
  const decoder = new TextDecoder();
  const encoder = new TextEncoder();
  let buffer = "";

  return new TransformStream({
    transform(chunk, controller) {
      buffer += decoder.decode(chunk, { stream: true });
      const lines = buffer.split("\n");
      buffer = lines.pop() ?? "";

      for (const line of lines) {
        const trimmed = line.trim();
        if (!trimmed || !trimmed.startsWith("data: ")) continue;
        const payload = trimmed.slice(6).trim();
        if (payload === "[DONE]") continue;
        try {
          const data = JSON.parse(payload);
          const choice = data.choices?.[0] ?? {};
          if (choice.finish_reason) {
            // final stop chunk – emit done signal and skip content
            const done = {
              model,
              created_at: new Date().toISOString(),
              message: { role: "assistant", content: "" },
              done: true,
              done_reason: choice.finish_reason === "stop" ? "stop" : choice.finish_reason,
            };
            controller.enqueue(encoder.encode(JSON.stringify(done) + "\n"));
            continue;
          }
          const delta = choice.delta ?? {};
          const content = delta.content ?? "";
          const ollama = {
            model,
            created_at: new Date().toISOString(),
            message: {
              role: delta.role ?? "assistant",
              content,
            },
            done: false,
          };
          controller.enqueue(encoder.encode(JSON.stringify(ollama) + "\n"));
        } catch {
          /* skip invalid SSE data lines */
        }
      }
    },
  });
}

/**
 * Create a TransformStream that converts OpenAI SSE chunks
 * to Ollama NDJSON lines for /api/generate streaming.
 */
export function openaiSseToOllamaGenerateStream(model: string): TransformStream<Uint8Array, Uint8Array> {
  const decoder = new TextDecoder();
  const encoder = new TextEncoder();
  let buffer = "";
  let doneSent = false;

  return new TransformStream({
    transform(chunk, controller) {
      buffer += decoder.decode(chunk, { stream: true });
      const lines = buffer.split("\n");
      buffer = lines.pop() ?? "";

      for (const line of lines) {
        const trimmed = line.trim();
        if (!trimmed || !trimmed.startsWith("data: ")) continue;
        const payload = trimmed.slice(6).trim();
        if (payload === "[DONE]") continue;
        try {
          const data = JSON.parse(payload);
          const choice = data.choices?.[0] ?? {};
          if (choice.finish_reason) {
            doneSent = true;
            const done = {
              model,
              created_at: new Date().toISOString(),
              response: "",
              done: true,
              done_reason: choice.finish_reason === "stop" ? "stop" : choice.finish_reason,
            };
            controller.enqueue(encoder.encode(JSON.stringify(done) + "\n"));
            continue;
          }
          const delta = choice.text ?? "";
          const ollama = {
            model,
            created_at: new Date().toISOString(),
            response: delta,
            done: false,
          };
          controller.enqueue(encoder.encode(JSON.stringify(ollama) + "\n"));
        } catch {
          /* skip invalid SSE data lines */
        }
      }
    },
    flush(controller) {
      if (doneSent) return;
      const done = {
        model,
        created_at: new Date().toISOString(),
        response: "",
        done: true,
        done_reason: "stop",
      };
      controller.enqueue(encoder.encode(JSON.stringify(done) + "\n"));
    },
  });
}
