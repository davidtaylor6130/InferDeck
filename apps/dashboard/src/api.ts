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

// ===========================================================================
// Audio Endpoints
// ===========================================================================

export interface TranscriptionResponse {
  text: string;
  language?: string;
  duration?: number;
  model: string;
  segments?: Array<{ id: number; start: number; end: number; text: string }>;
  task?: string;
  no_speech_prob?: number;
}

export interface SpeechResponse {
  object: string;
  model: string;
  voice: string;
  format: string;
  duration_ms: number;
  sample_rate: number;
}

export async function transcribeAudio(formData: FormData): Promise<TranscriptionResponse> {
  const response = await fetch(`${API_BASE}/v1/audio/transcriptions`, {
    method: 'POST',
    body: formData,
    signal: AbortSignal.timeout(120000),
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: { message: response.statusText } }));
    throw new Error(error.error?.message || response.statusText);
  }

  return response.json() as Promise<TranscriptionResponse>;
}

export async function translateAudio(formData: FormData): Promise<TranscriptionResponse> {
  const response = await fetch(`${API_BASE}/v1/audio/translations`, {
    method: 'POST',
    body: formData,
    signal: AbortSignal.timeout(120000),
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: { message: response.statusText } }));
    throw new Error(error.error?.message || response.statusText);
  }

  return response.json() as Promise<TranscriptionResponse>;
}

export async function synthesizeSpeech(input: string, model: string = 'piper-en', voice: string = 'alloy', format: string = 'wav'): Promise<Blob> {
  const response = await fetch(`${API_BASE}/v1/audio/speech`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ input, model, voice, response_format: format }),
    signal: AbortSignal.timeout(60000),
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: { message: response.statusText } }));
    throw new Error(error.error?.message || response.statusText);
  }

  return response.blob();
}

// ===========================================================================
// Image Generation
// ===========================================================================

export interface ImageGenerationResult {
  url?: string;
  b64_json?: string;
  revised_prompt: string;
}

export interface ImageGenerationResponse {
  created: number;
  data: ImageGenerationResult[];
}

export async function generateImage(prompt: string, options?: {
  model?: string;
  response_format?: string;
  size?: number;
  width?: number;
  height?: number;
  n?: number;
  img2img?: boolean;
  image?: string;
}): Promise<ImageGenerationResponse> {
  const body: Record<string, any> = { prompt };
  if (options?.model) body.model = options.model;
  if (options?.response_format) body.response_format = options.response_format;
  if (options?.size) body.size = options.size;
  if (options?.width) body.width = options.width;
  if (options?.height) body.height = options.height;
  if (options?.n) body.n = options.n;
  if (options?.img2img) { body.img2img = true; body.image = options.image; }

  const response = await fetch(`${API_BASE}/v1/images/generate`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(300000),
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: { message: response.statusText } }));
    throw new Error(error.error?.message || response.statusText);
  }

  return response.json() as Promise<ImageGenerationResponse>;
}

// ===========================================================================
// Document CRUD & Search
// ===========================================================================

export interface DocumentRecord {
  id: string;
  object: string;
  title: string;
  content: string;
  embedding_dimension: number;
  metadata: Record<string, any>;
  created_at: number;
  updated_at: number;
}

export interface DocumentListResponse {
  object: string;
  data: DocumentRecord[];
  count: number;
}

export interface DocumentSearchResponse {
  object: string;
  data: Array<{
    id: string;
    title: string;
    content: string;
    similarity: number;
    embedding?: number[];
  }>;
  count: number;
}

export async function listDocuments(): Promise<DocumentListResponse> {
  const response = await fetch(`${API_BASE}/v1/documents`, {
    signal: AbortSignal.timeout(10000),
  });

  if (!response.ok) {
    throw new Error(`Failed to list documents: ${response.statusText}`);
  }

  return response.json() as Promise<DocumentListResponse>;
}

export async function createDocument(content: string, options?: {
  title?: string;
  metadata?: Record<string, any>;
  embedding?: number[];
  generate_embedding?: boolean;
}): Promise<DocumentRecord> {
  const body: Record<string, any> = { content };
  if (options?.title) body.title = options.title;
  if (options?.metadata) body.metadata = options.metadata;
  if (options?.embedding) body.embedding = options.embedding;
  if (options?.generate_embedding !== undefined) body.generate_embedding = options.generate_embedding;

  const response = await fetch(`${API_BASE}/v1/documents`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(30000),
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: { message: response.statusText } }));
    throw new Error(error.error?.message || response.statusText);
  }

  return response.json() as Promise<DocumentRecord>;
}

export async function getDocument(id: string): Promise<DocumentRecord> {
  const response = await fetch(`${API_BASE}/v1/documents/${id}`, {
    signal: AbortSignal.timeout(10000),
  });

  if (!response.ok) {
    throw new Error(`Failed to get document: ${response.statusText}`);
  }

  return response.json() as Promise<DocumentRecord>;
}

export async function deleteDocument(id: string): Promise<{ id: string; object: string; deleted: boolean }> {
  const response = await fetch(`${API_BASE}/v1/documents/${id}`, {
    method: 'DELETE',
    signal: AbortSignal.timeout(10000),
  });

  if (!response.ok) {
    throw new Error(`Failed to delete document: ${response.statusText}`);
  }

  return response.json();
}

export async function searchDocuments(query: string, options?: {
  top_k?: number;
  include_embedding?: boolean;
  embedding?: number[];
}): Promise<DocumentSearchResponse> {
  const body: Record<string, any> = { query };
  if (options?.top_k) body.top_k = options.top_k;
  if (options?.include_embedding !== undefined) body.include_embedding = options.include_embedding;
  if (options?.embedding) body.embedding = options.embedding;

  const response = await fetch(`${API_BASE}/v1/documents/search`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
    signal: AbortSignal.timeout(30000),
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: { message: response.statusText } }));
    throw new Error(error.error?.message || response.statusText);
  }

  return response.json() as Promise<DocumentSearchResponse>;
}

// ===========================================================================
// Fine-Tuning Jobs
// ===========================================================================

export interface FineTuningJob {
  id: string;
  object: string;
  model: string;
  status: string;
  created_at: number;
  finished_at: number;
  result_files: any[];
  hyperparameters: Record<string, any>;
  training_file: string;
  trained_tokens: number;
  validation_file?: string;
  integrations?: any[];
  seed?: number;
  suffix?: string;
}

export interface FineTuningListResponse {
  object: string;
  data: FineTuningJob[];
  has_more: boolean;
}

export interface FineTuningCreateRequest {
  model: string;
  training_file: string;
  epochs?: number;
  learning_rate?: number;
  batch_size?: number;
  max_steps?: number;
  validation_file?: string;
  seed?: number;
  suffix?: string;
}

export async function listFineTuningJobs(): Promise<FineTuningListResponse> {
  const response = await fetch(`${API_BASE}/v1/fine_tuning/jobs`, {
    signal: AbortSignal.timeout(10000),
  });

  if (!response.ok) {
    throw new Error(`Failed to list fine-tuning jobs: ${response.statusText}`);
  }

  return response.json() as Promise<FineTuningListResponse>;
}

export async function createFineTuningJob(request: FineTuningCreateRequest): Promise<FineTuningJob> {
  const response = await fetch(`${API_BASE}/v1/fine_tuning/jobs`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(request),
    signal: AbortSignal.timeout(300000),
  });

  if (!response.ok) {
    const error = await response.json().catch(() => ({ error: { message: response.statusText } }));
    throw new Error(error.error?.message || response.statusText);
  }

  return response.json() as Promise<FineTuningJob>;
}

export async function getFineTuningJob(id: string): Promise<FineTuningJob> {
  const response = await fetch(`${API_BASE}/v1/fine_tuning/jobs/${id}`, {
    signal: AbortSignal.timeout(10000),
  });

  if (!response.ok) {
    throw new Error(`Failed to get fine-tuning job: ${response.statusText}`);
  }

  return response.json() as Promise<FineTuningJob>;
}

export async function cancelFineTuningJob(id: string): Promise<{ id: string; object: string; cancelled: boolean }> {
  const response = await fetch(`${API_BASE}/v1/fine_tuning/jobs/${id}/cancel`, {
    method: 'POST',
    signal: AbortSignal.timeout(30000),
  });

  if (!response.ok) {
    throw new Error(`Failed to cancel fine-tuning job: ${response.statusText}`);
  }

  return response.json();
}
