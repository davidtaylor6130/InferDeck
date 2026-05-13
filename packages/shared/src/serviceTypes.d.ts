export type ServiceKind = "gateway" | "ollama" | "comfyui" | "rag" | "speech";
export type ServiceStatus = "unknown" | "starting" | "ready" | "running" | "stopped" | "error" | "unhealthy";
export interface ServiceRecord {
    id: string;
    name: string;
    kind: ServiceKind;
    status: ServiceStatus;
    pid: number | null;
    baseUrl: string | null;
    lastHealthcheckAt: string | null;
    lastError: string | null;
    updatedAt: string;
    metadata?: Record<string, unknown>;
}
export interface ServiceActionRequest {
    id: string;
}
export interface ServiceHealthResponse {
    service: ServiceRecord;
    healthy: boolean;
    lastCheck: string;
    latencyMs: number | null;
    error: string | null;
}
export interface ServiceListResponse {
    services: ServiceRecord[];
    healthyCount: number;
    unhealthyCount: number;
}
//# sourceMappingURL=serviceTypes.d.ts.map