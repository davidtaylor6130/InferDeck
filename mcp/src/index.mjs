import { timingSafeEqual } from 'node:crypto';
import { createServer } from 'node:http';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createMcpHandler } from '@modelcontextprotocol/server';
import { hostHeaderValidation, originValidation, toNodeHandler } from '@modelcontextprotocol/node';
import { InferDeckClient, MCP_CLIENT_LIMITS } from './inferdeck-client.mjs';
import { createInferDeckMcpServer } from './server.mjs';

const DEFAULT_HOSTS = ['localhost', '127.0.0.1', '[::1]'];
const DEFAULT_MAX_BODY_BYTES = 256 * 1024;

function writeJson(response, status, body) {
  if (response.headersSent) return;
  response.writeHead(status, { 'content-type': 'application/json', 'cache-control': 'no-store' });
  response.end(JSON.stringify(body));
}

function sameSecret(presented, expected) {
  const left = Buffer.from(presented);
  const right = Buffer.from(expected);
  return left.length === right.length && timingSafeEqual(left, right);
}

function validBearer(request, token) {
  const authorization = request.headers.authorization;
  return typeof authorization === 'string'
    && authorization.startsWith('Bearer ')
    && authorization.length > 7
    && !authorization.slice(7).includes(' ')
    && sameSecret(authorization.slice(7), token);
}

function splitHosts(value, fallback = DEFAULT_HOSTS) {
  if (typeof value !== 'string' || value.trim() === '') return fallback;
  const hosts = value.split(',').map((host) => host.trim()).filter(Boolean);
  if (hosts.some((host) => host.includes('/') || (host.includes(':') && !host.startsWith('[')))) {
    throw new Error('MCP_ALLOWED_HOSTS contains an invalid hostname');
  }
  return [...new Set(hosts)];
}

function bodyLimit(value) {
  if (value === undefined) return DEFAULT_MAX_BODY_BYTES;
  const parsed = Number.parseInt(value, 10);
  if (!Number.isSafeInteger(parsed) || parsed < 1024 || parsed > 16 * 1024 * 1024) {
    throw new Error('MCP_MAX_BODY_BYTES must be between 1024 and 16777216');
  }
  return parsed;
}

async function readJsonBody(request, maxBytes) {
  const contentLength = request.headers['content-length'];
  if (contentLength !== undefined
    && (Array.isArray(contentLength) || !/^\d+$/.test(contentLength) || Number(contentLength) > maxBytes)) {
    throw Object.assign(new Error('request body exceeds the MCP limit'), { status: 413 });
  }
  const chunks = [];
  let total = 0;
  for await (const chunk of request) {
    total += chunk.byteLength;
    if (total > maxBytes) throw Object.assign(new Error('request body exceeds the MCP limit'), { status: 413 });
    chunks.push(chunk);
  }
  if (total === 0) return undefined;
  try {
    return JSON.parse(Buffer.concat(chunks, total).toString('utf8'));
  } catch {
    throw Object.assign(new Error('request body must be valid JSON'), { status: 400 });
  }
}

function clientForRequest(client, requestInfo) {
  const signal = requestInfo?.signal;
  if (!signal) return client;
  return new Proxy(client, {
    get(target, property) {
      if (property === 'request') {
        return (path, options = {}) => target.request(path, { ...options, signal });
      }
      const value = Reflect.get(target, property, target);
      return typeof value === 'function' ? value.bind(target) : value;
    },
  });
}

export function createMcpHttpServer({
  client,
  mcpToken,
  allowedHostnames = DEFAULT_HOSTS,
  allowedOriginHostnames = allowedHostnames,
  maxBodyBytes = DEFAULT_MAX_BODY_BYTES,
} = {}) {
  if (!client || typeof client.getHealth !== 'function') throw new Error('An InferDeck client is required');
  if (typeof mcpToken !== 'string' || mcpToken.length === 0) throw new Error('MCP bearer token is required');
  if (!Number.isSafeInteger(maxBodyBytes) || maxBodyBytes < 1024 || maxBodyBytes > 16 * 1024 * 1024) throw new Error('maxBodyBytes is invalid');

  const handler = toNodeHandler(createMcpHandler(
    ({ requestInfo } = {}) => createInferDeckMcpServer(clientForRequest(client, requestInfo)),
    { legacy: 'stateless' },
  ));
  const validateHost = hostHeaderValidation(allowedHostnames);
  const validateOrigin = originValidation(allowedOriginHostnames);
  return createServer(async (request, response) => {
    if (request.url !== '/mcp') {
      writeJson(response, 404, { error: 'not_found' });
      return;
    }
    if (!validateHost(request, response) || !validateOrigin(request, response)) return;
    if (!validBearer(request, mcpToken)) {
      response.setHeader('www-authenticate', 'Bearer realm="InferDeck MCP"');
      writeJson(response, 401, { error: 'unauthorized' });
      return;
    }
    if (request.method !== 'GET' && request.method !== 'POST') {
      response.setHeader('allow', 'GET, POST');
      writeJson(response, 405, { error: 'method_not_allowed' });
      return;
    }
    try {
      const body = request.method === 'POST' ? await readJsonBody(request, maxBodyBytes) : undefined;
      await handler(request, response, body);
    } catch (error) {
      if (response.headersSent) return;
      const status = error?.status === 413 ? 413 : error?.status === 400 ? 400 : 500;
      writeJson(response, status, { error: status === 500 ? 'internal_error' : error.message });
    }
  });
}

export function createMcpRuntime(env = process.env) {
  const bindHost = env.MCP_BIND_HOST ?? '127.0.0.1';
  const allowedHostnames = splitHosts(env.MCP_ALLOWED_HOSTS);
  if (!DEFAULT_HOSTS.includes(bindHost) && !env.MCP_ALLOWED_HOSTS) {
    throw new Error('MCP_ALLOWED_HOSTS is required when MCP_BIND_HOST is not loopback');
  }
  const port = Number.parseInt(env.MCP_PORT ?? '11436', 10);
  if (!Number.isSafeInteger(port) || port < 1 || port > 65535) throw new Error('MCP_PORT must be a valid TCP port');
  const client = new InferDeckClient({
    baseUrl: env.INFERDECK_URL,
    apiKey: env.INFERDECK_API_KEY,
    controlToken: env.INFERDECK_CONTROL_TOKEN,
    mediaOperatorOptIn: env.MCP_ENABLE_MEDIA === 'true',
    timeoutMs: Number.parseInt(env.MCP_GATEWAY_TIMEOUT_MS ?? '30000', 10),
    generationTimeoutMs: Number.parseInt(env.MCP_GENERATION_TIMEOUT_MS ?? '300000', 10),
  });
  return {
    server: createMcpHttpServer({
      client,
      mcpToken: env.MCP_BEARER_TOKEN,
      allowedHostnames,
      allowedOriginHostnames: splitHosts(env.MCP_ALLOWED_ORIGINS, allowedHostnames),
      maxBodyBytes: bodyLimit(env.MCP_MAX_BODY_BYTES),
    }),
    bindHost,
    port,
  };
}

export function startFromEnv(env = process.env) {
  const runtime = createMcpRuntime(env);
  runtime.server.listen(runtime.port, runtime.bindHost, () => {
    console.log('InferDeck MCP listening on http://' + runtime.bindHost + ':' + runtime.port + '/mcp');
  });
  return runtime.server;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  startFromEnv();
}
