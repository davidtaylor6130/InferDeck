/// @file api.ts
/// @brief API client for InferDeck C++ gateway service.
/// Fetches from /v1/... endpoints with OpenAI-compatible schema.

const API_BASE = process.env.NEXT_PUBLIC_API_URL || 'https://localhost:8080';

interface ChatMessage {
  role: 'system' | 'user' | 'assistant' | 'tool';
  content: string;
}

interface ChatCompletionRequest {
  model?: string;
  messages: ChatMessage[];
  max_tokens?: number;
  temperature?: number;
  top_p?: number;
  stream?: boolean;
}

interface ChatCompletionResponse {
  id: string;
  object: string;
  created: number;
  model: string;
  choices: Array<{
    index: number;
    message: { role: string; content: string };
    finish_reason: string;
  }>;
  usage: {
    prompt_tokens: number;
    completion_tokens: number;
    total_tokens: number;
  };
}

interface ModelInfo {
  id: string;
  object: string;
  created: number;
  owned_by: string;
}

interface ModelsResponse {
  object: string;
  data: ModelInfo[];
}

interface HealthResponse {
  status: string;
  version: string;
  uptime: number;
}

interface MetricsResponse {
  counters: Record<string, number>;
  gauges: Record<string, number>;
  histograms: Record<string, { min: number; max: number; avg: number; count: number; sum: number }>;
}

export async function chatCompletion(request: ChatCompletionRequest): Promise<ChatCompletionResponse> {
  const response = await fetch(`${API_BASE}/v1/chat/completions`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(request),
    signal: AbortSignal.timeout(60000),
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: { message: response.statusText } }));
    throw new Error(error.error?.message || response.statusText);
  }

  return response.json() as Promise<ChatCompletionResponse>;
}

export async function streamChatCompletion(request: ChatCompletionRequest): Promise<AsyncGenerator<string>> {
  const response = await fetch(`${API_BASE}/v1/chat/completions`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ ...request, stream: true }),
    signal: AbortSignal.timeout(120000),
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: { message: response.statusText } }));
    throw new Error(error.error?.message || response.statusText);
  }

  const reader = response.body?.getReader();
  if (!reader) throw new Error('No response body');

  const decoder = new TextDecoder();
  let buffer = '';

  while (true) {
    const { done, value } = await reader.read();
    if (done) break;

    buffer += decoder.decode(value, { stream: true });
    const lines = buffer.split('\n');
    buffer = lines.pop() || '';

    for (const line of lines) {
      if (line.startsWith('data: [DONE]')) break;
      if (line.startsWith('data: ')) {
        try {
          const data = JSON.parse(line.slice(6));
          const content = data.choices?.[0]?.delta?.content;
          if (content) yield content;
        } catch {
          // Ignore parse errors for malformed SSE
        }
      }
    }
  }
}

export async function getModels(): Promise<ModelsResponse> {
  const response = await fetch(`${API_BASE}/v1/models`, {
    signal: AbortSignal.timeout(10000),
  });

  if (!response.ok) {
    throw new Error(`Failed to get models: ${response.statusText}`);
  }

  return response.json() as Promise<ModelsResponse>;
}

export async function getHealth(): Promise<HealthResponse> {
  const response = await fetch(`${API_BASE}/v1/health`, {
    signal: AbortSignal.timeout(10000),
  });

  if (!response.ok) {
    throw new Error(`Health check failed: ${response.statusText}`);
  }

  return response.json() as Promise<HealthResponse>;
}

export async function getMetrics(): Promise<MetricsResponse> {
  const response = await fetch(`${API_BASE}/inferdeck/metrics`, {
    signal: AbortSignal.timeout(10000),
  });

  if (!response.ok) {
    throw new Error(`Failed to get metrics: ${response.statusText}`);
  }

  return response.json() as Promise<MetricsResponse>;
}

export async function getStatus(): Promise<Record<string, any>> {
  const response = await fetch(`${API_BASE}/inferdeck/status`, {
    signal: AbortSignal.timeout(10000),
  });

  if (!response.ok) {
    throw new Error(`Failed to get status: ${response.statusText}`);
  }

  return response.json();
}
