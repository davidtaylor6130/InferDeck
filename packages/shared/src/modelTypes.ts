export interface ModelInfo {
  name: string;
  model: string;
  size: number;
  digest: string;
  details: {
    parent_model: string;
    format: string;
    family: string;
    families: string[];
    parameter_size: string;
    quantization_level: string;
  };
}

export interface ModelDetails {
  parent_model: string;
  format: string;
  family: string;
  families: string[];
  parameter_size: string;
  quantization_level: string;
}

export interface RunningModelInfo {
  name: string;
  model: string;
  size: number;
  digest: string;
  details: ModelDetails;
  size_vram: number;
  expires_at: string;
  ttl: number;
}

export interface ModelPullProgress {
  status: string;
  digest: string;
  total: number;
  completed: number;
}

export interface ListModelsResponse {
  models: ModelInfo[];
}

export interface ListRunningModelsResponse {
  running: RunningModelInfo[];
}

export interface GetModelInfoResponse extends ModelInfo {}

export interface ModelUnloadRequest {
  name: string;
  keep_alive: string;
}

export interface ModelPullRequest {
  name: string;
  stream: boolean;
}
