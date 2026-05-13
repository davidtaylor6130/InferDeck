import type { FastifyRequest } from "fastify";

export function assertNotSelfProxy(req: FastifyRequest, backendBaseUrl: string): void {
  const hostHeader = req.headers.host;
  if (!hostHeader) return;

  const backend = new URL(backendBaseUrl);
  const backendPort = backend.port || (backend.protocol === "https:" ? "443" : "80");
  const backendHostPort = `${backend.hostname}:${backendPort}`;

  if (hostHeader === backendHostPort) {
    throw new Error(`Ollama backend URL points back at the gateway (${backendBaseUrl})`);
  }
}
