export interface OllamaModelInfo {
  name: string;
  model: string;
  size: number;
  digest: string;
  details: OllamaModelDetails;
  modified_at: string;
}

export interface OllamaModelDetails {
  parent_model: string;
  format: string;
  family: string;
  families: string[] | null;
  parameter_size: string;
  quantization_level: string;
}

export interface OllamaRunningModel {
  name: string;
  model: string;
  size: number;
  digest: string;
  details: OllamaModelDetails;
  size_vram: number;
  expires_at: string;
  ttl: number;
}

export interface OllamaChatRequest {
  model: string;
  messages?: OllamaChatMessage[];
  stream?: boolean;
  options?: Record<string, unknown>;
  keep_alive?: string | number | null;
}

export interface OllamaChatMessage {
  role: "user" | "assistant" | "system" | "tool";
  content: string;
  images?: string[];
}

export interface OllamaChatResponse {
  model: string;
  created_at: string;
  message: OllamaChatMessage;
  done: boolean;
  total_duration?: number;
  load_duration?: number;
  prompt_eval_count?: number;
  prompt_eval_duration?: number;
  eval_count?: number;
  eval_duration?: number;
}

export interface OllamaChatStreamChunk {
  model: string;
  created_at: string;
  message: {
    role: string;
    content: string;
    images?: string[];
  };
  done: boolean;
  done_reason?: string;
  total_duration?: number;
  load_duration?: number;
  prompt_eval_count?: number;
  prompt_eval_duration?: number;
  eval_count?: number;
  eval_duration?: number;
}

export interface OllamaGenerateRequest {
  model: string;
  prompt: string;
  stream?: boolean;
  options?: Record<string, unknown>;
  keep_alive?: string | number | null;
  system?: string;
  template?: string;
  context?: number[];
}

export interface OllamaGenerateResponse {
  model: string;
  created_at: string;
  response: string;
  done: boolean;
  total_duration?: number;
  load_duration?: number;
  prompt_eval_count?: number;
  prompt_eval_duration?: number;
  eval_count?: number;
  eval_duration?: number;
}

export interface OllamaGenerateStreamChunk {
  model: string;
  created_at: string;
  response: string;
  done: boolean;
  done_reason?: string;
  total_duration?: number;
  load_duration?: number;
  prompt_eval_count?: number;
  prompt_eval_duration?: number;
  eval_count?: number;
  eval_duration?: number;
}

export interface OllamaEmbedRequest {
  model: string;
  input: string | string[];
  truncate?: boolean;
  keep_alive?: string | number | null;
}

export interface OllamaEmbedResponse {
  model: string;
  embeddings: number[];
  total_duration?: number;
  load_duration?: number;
  prompt_eval_count?: number;
}

export interface OllamaPullProgress {
  status: string;
  digest: string;
  total: number;
  completed: number;
}

export interface OllamaPullStream {
  status: string;
  digest?: string;
  total?: number;
  completed?: number;
}

export interface OllamaHealthStatus {
  status: string;
  gpu: string[];
  ollama_version: string;
}
