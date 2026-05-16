export interface LlamaHealthResponse {
  status: string;
  gpu?: Array<{
    uuid: string;
    name: string;
    vram: { total: number; free: number; used: number };
    temperature?: number;
  }>;
  model_info?: {
    path: string;
    n_ctx?: number;
    n_gpu_layers?: number;
    n_batch?: number;
  };
}

export interface LlamaModelInfo {
  name: string;
  path: string;
  size: number;
  modified_at: string;
  format: "gguf";
}

export interface LlamaChatMessage {
  role: "user" | "assistant" | "system" | "tool";
  content: string;
}

export interface LlamaChatRequest {
  model: string;
  messages: LlamaChatMessage[];
  stream?: boolean;
  temperature?: number;
  top_p?: number;
  max_tokens?: number;
  stop?: string[];
}

export interface LlamaChatResponse {
  id: string;
  object: string;
  created: number;
  model: string;
  choices: Array<{
    index: number;
    message: { role: string; content: string };
    finish_reason: string | null;
  }>;
  usage: {
    prompt_tokens: number;
    completion_tokens: number;
    total_tokens: number;
  };
}

export interface LlamaCompletionRequest {
  model: string;
  prompt: string;
  stream?: boolean;
  temperature?: number;
  max_tokens?: number;
  stop?: string[];
}

export interface LlamaCompletionResponse {
  id: string;
  object: string;
  created: number;
  model: string;
  choices: Array<{
    index: number;
    text: string;
    finish_reason: string | null;
  }>;
  usage: {
    prompt_tokens: number;
    completion_tokens: number;
    total_tokens: number;
  };
}

export interface LlamaEmbedRequest {
  model: string;
  input: string | string[];
}

export interface LlamaEmbedResponse {
  object: string;
  data: Array<{
    object: string;
    embedding: number[];
    index: number;
  }>;
  model: string;
  usage: {
    prompt_tokens: number;
    total_tokens: number;
  };
}
