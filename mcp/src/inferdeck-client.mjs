const DEFAULT_TIMEOUT_MS = 30_000;
const DEFAULT_MAX_REQUEST_BYTES = 256 * 1024;
const DEFAULT_MAX_RESPONSE_BYTES = 32 * 1024 * 1024;

const DATA_PLANE_ROUTES = new Map([
  ['/v1/models', new Set(['GET'])],
  ['/v1/audio/speech', new Set(['POST'])],
]);
const CONTROL_READ_ROUTES = new Map([
  ['/api/inferdeck/v1/health', new Set(['GET'])],
  ['/api/inferdeck/v1/status', new Set(['GET'])],
  ['/api/inferdeck/v1/models', new Set(['GET'])],
  ['/api/inferdeck/v1/media/jobs', new Set(['GET'])],
]);
const CONTROL_MEDIA_ROUTES = [
  { method: 'POST', pattern: /^\/api\/inferdeck\/v1\/media\/(?:images|audio)\/generations$/ },
  { method: 'GET', pattern: /^\/api\/inferdeck\/v1\/media\/jobs\/[0-9]+\/outputs\/[0-9]+$/ },
  { method: 'POST', pattern: /^\/api\/inferdeck\/v1\/media\/jobs\/[0-9]+\/cancel$/ },
];

function normalizeBaseUrl(baseUrl) {
  if (typeof baseUrl !== 'string' || baseUrl.length === 0) throw new Error('INFERDECK_URL is required');
  const parsedUrl = new URL(baseUrl);
  if (!['http:', 'https:'].includes(parsedUrl.protocol)) throw new Error('INFERDECK_URL must use http or https');
  if (parsedUrl.username || parsedUrl.password || parsedUrl.search || parsedUrl.hash) {
    throw new Error('INFERDECK_URL must not contain credentials, a query, or a fragment');
  }
  return parsedUrl.toString().replace(/\/$/, '');
}

function safeErrorMessage(error, secrets) {
  let message = error instanceof Error ? error.message : String(error);
  for (const secret of secrets) if (secret) message = message.split(secret).join('[redacted]');
  return message.slice(0, 512);
}

function routeKind(path, method, mediaOperatorOptIn) {
  if (DATA_PLANE_ROUTES.get(path)?.has(method)) return 'data';
  if (mediaOperatorOptIn && method === 'POST' && path === '/api/inferdeck/v1/video/generations') return 'data';
  if (CONTROL_READ_ROUTES.get(path)?.has(method)) return 'control-read';
  if (mediaOperatorOptIn && CONTROL_MEDIA_ROUTES.some((route) => route.method === method && route.pattern.test(path))) {
    return 'control-media';
  }
  return null;
}

function validatePath(path) {
  if (
    typeof path !== 'string' || !path.startsWith('/') || path.includes('\\')
    || path.includes('?') || path.includes('#') || path.includes('//')
    || path.split('/').includes('..') || path.split('/').includes('.')
  ) throw new Error('InferDeck route is not allowed');
}

async function cancelResponseBody(response) {
  try {
    await response.body?.cancel?.();
  } catch {
    // The response is already being discarded.
  }
}

async function readBytes(response, maxBytes) {
  const length = response.headers?.get?.('content-length');
  if (length && (!/^\d+$/.test(length) || Number(length) > maxBytes)) {
    await cancelResponseBody(response);
    throw new Error('InferDeck response exceeds the MCP response limit');
  }
  if (response.body?.getReader) {
    const reader = response.body.getReader();
    const chunks = [];
    let total = 0;
    try {
      while (true) {
        const item = await reader.read();
        if (item.done) break;
        total += item.value.byteLength;
        if (total > maxBytes) {
          await reader.cancel();
          throw new Error('InferDeck response exceeds the MCP response limit');
        }
        chunks.push(item.value);
      }
    } finally {
      reader.releaseLock?.();
    }
    const bytes = new Uint8Array(total);
    let offset = 0;
    for (const chunk of chunks) {
      bytes.set(chunk, offset);
      offset += chunk.byteLength;
    }
    return bytes;
  }
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (bytes.byteLength > maxBytes) throw new Error('InferDeck response exceeds the MCP response limit');
  return bytes;
}

function contentTypeOf(response) {
  return response.headers?.get?.('content-type')?.split(';', 1)[0]?.trim().toLowerCase() ?? '';
}

export class InferDeckClient {
  #baseUrl;
  #dataHeaders;
  #controlHeaders;
  #apiKey;
  #controlToken;
  #fetch;
  #timeoutMs;
  #generationTimeoutMs;
  #maxRequestBytes;
  #maxResponseBytes;

  constructor({
    baseUrl, apiKey, controlToken, mediaOperatorOptIn = false,
    timeoutMs = DEFAULT_TIMEOUT_MS,
    generationTimeoutMs = 5 * 60_000,
    maxRequestBytes = DEFAULT_MAX_REQUEST_BYTES,
    maxResponseBytes = DEFAULT_MAX_RESPONSE_BYTES,
    fetchImpl = globalThis.fetch,
  }) {
    this.#baseUrl = normalizeBaseUrl(baseUrl);
    if (!fetchImpl) throw new Error('A fetch implementation is required');
    if (!Number.isSafeInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 300_000) throw new Error('timeoutMs must be between 1 and 300000');
    if (!Number.isSafeInteger(generationTimeoutMs) || generationTimeoutMs < 1 || generationTimeoutMs > 30 * 60_000) throw new Error('generationTimeoutMs must be between 1 and 1800000');
    if (!Number.isSafeInteger(maxRequestBytes) || maxRequestBytes < 1 || maxRequestBytes > 16 * 1024 * 1024) throw new Error('maxRequestBytes is invalid');
    if (!Number.isSafeInteger(maxResponseBytes) || maxResponseBytes < 1 || maxResponseBytes > 100 * 1024 * 1024) throw new Error('maxResponseBytes is invalid');
    this.#apiKey = typeof apiKey === 'string' ? apiKey : '';
    this.#controlToken = typeof controlToken === 'string' ? controlToken : '';
    this.#dataHeaders = { accept: 'application/json, application/octet-stream', ...(this.#apiKey ? { authorization: 'Bearer ' + this.#apiKey } : {}) };
    this.#controlHeaders = { accept: 'application/json, application/octet-stream', ...(this.#controlToken ? { authorization: 'Bearer ' + this.#controlToken } : {}) };
    this.#fetch = fetchImpl;
    this.#timeoutMs = timeoutMs;
    this.#generationTimeoutMs = generationTimeoutMs;
    this.#maxRequestBytes = maxRequestBytes;
    this.#maxResponseBytes = maxResponseBytes;
    this.mediaOperatorOptIn = mediaOperatorOptIn === true;
    // Media registration probes this value before the first tool call. The
    // request method always refreshes live metadata before sending video.
    this.videoGenerationAvailable = this.mediaOperatorOptIn;
    this.baseUrl = this.#baseUrl;
  }

  async request(path, { method = 'GET', body, signal } = {}) {
    validatePath(path);
    if (typeof method !== 'string') throw new Error('InferDeck request method is invalid');
    const normalizedMethod = method.toUpperCase();
    const kind = routeKind(path, normalizedMethod, this.mediaOperatorOptIn);
    if (!kind) throw new Error('InferDeck route or method is not allowed');
    if (normalizedMethod === 'GET' && body !== undefined) throw new Error('GET requests must not have a body');
    if (path === '/api/inferdeck/v1/video/generations' && normalizedMethod === 'POST') {
      await this.#refreshRuntimeCapabilities();
      if (this.videoGenerationAvailable !== true) {
        throw new Error('InferDeck video generation is not currently available; install a runtime model with video_generation');
      }
    }

    let serializedBody;
    if (body !== undefined) {
      if (body === null || typeof body !== 'object' || body instanceof Uint8Array || Buffer.isBuffer(body)) {
        throw new Error('InferDeck request body must be a JSON object or array');
      }
      try {
        serializedBody = JSON.stringify(body);
      } catch {
        throw new Error('InferDeck request body is not serializable');
      }
      if (Buffer.byteLength(serializedBody, 'utf8') > this.#maxRequestBytes) {
        throw new Error('InferDeck request body exceeds the MCP request limit');
      }
    }

    const headers = kind === 'data' ? this.#dataHeaders : this.#controlHeaders;
    const generation = normalizedMethod === 'POST'
      && (path === '/v1/audio/speech' || path.endsWith('/generations'));
    const controller = new AbortController();
    let timedOut = false;
    const abortFromCaller = () => controller.abort();
    if (signal?.aborted) abortFromCaller();
    else signal?.addEventListener('abort', abortFromCaller, { once: true });
    const timer = setTimeout(
      () => { timedOut = true; controller.abort(); },
      generation ? this.#generationTimeoutMs : this.#timeoutMs,
    );
    let response;
    try {
      response = await this.#fetch(this.#baseUrl + path, {
        method: normalizedMethod,
        headers: {
          ...headers,
          ...(serializedBody !== undefined ? { 'content-type': 'application/json' } : {}),
        },
        ...(serializedBody !== undefined ? { body: serializedBody } : {}),
        redirect: 'error',
        signal: controller.signal,
      });
    } catch (error) {
      clearTimeout(timer);
      signal?.removeEventListener('abort', abortFromCaller);
      if (timedOut) throw new Error('InferDeck request timed out');
      if (signal?.aborted) throw new Error('InferDeck request cancelled');
      throw new Error('InferDeck is unavailable: ' + safeErrorMessage(error, [this.#apiKey, this.#controlToken]));
    } finally {
      if (!response) {
        clearTimeout(timer);
        signal?.removeEventListener('abort', abortFromCaller);
      }
    }
    try {
      if (response.status >= 300 && response.status < 400) {
        await cancelResponseBody(response);
        throw new Error('InferDeck redirects are not allowed');
      }
      if (!response.ok) {
        await cancelResponseBody(response);
        throw new Error('InferDeck ' + path + ' returned HTTP ' + response.status);
      }

      const contentType = contentTypeOf(response);
      const bytes = await readBytes(response, this.#maxResponseBytes);
      const binary = contentType.startsWith('audio/')
        || contentType.startsWith('image/') || contentType.startsWith('video/');
      let parsedBody = bytes;
      if (!binary) {
        const text = new TextDecoder().decode(bytes);
        if (text.length > 0) {
          try {
            parsedBody = JSON.parse(text);
          } catch {
            throw new Error('InferDeck returned an invalid JSON response');
          }
        } else {
          parsedBody = null;
        }
      }
      return { body: parsedBody, status: response.status, headers: response.headers, contentType };
    } catch (error) {
      await cancelResponseBody(response);
      if (timedOut) throw new Error('InferDeck request timed out');
      if (signal?.aborted) throw new Error('InferDeck request cancelled');
      throw error;
    } finally {
      clearTimeout(timer);
      signal?.removeEventListener('abort', abortFromCaller);
    }
  }

  async listModels() {
    const result = await this.request('/api/inferdeck/v1/models', { method: 'GET' });
    this.#setRuntimeCapabilities(result.body);
    return result.body;
  }

  async #refreshRuntimeCapabilities() {
    await this.listModels();
  }

  #setRuntimeCapabilities(discovery) {
    const models = Array.isArray(discovery?.data)
      ? discovery.data
      : Array.isArray(discovery?.models) ? discovery.models : [];
    const capabilities = new Set();
    for (const model of models) {
      if (model?.runtime_available === false) continue;
      for (const capability of Array.isArray(model?.capabilities) ? model.capabilities : []) {
        if (typeof capability === 'string') capabilities.add(capability);
      }
      if (typeof model?.modality === 'string') capabilities.add(model.modality);
    }
    this.capabilities = Object.fromEntries([...capabilities].map((capability) => [capability, true]));
    this.videoGenerationAvailable = capabilities.has('video_generation');
    this.mediaCapabilities = this.capabilities;
  }

  async getStatus() {
    return (await this.request('/api/inferdeck/v1/status', { method: 'GET' })).body;
  }

  async getHealth() {
    return (await this.request('/api/inferdeck/v1/health', { method: 'GET' })).body;
  }
}

export const MCP_CLIENT_LIMITS = Object.freeze({
  defaultTimeoutMs: DEFAULT_TIMEOUT_MS,
  defaultMaxRequestBytes: DEFAULT_MAX_REQUEST_BYTES,
  defaultMaxResponseBytes: DEFAULT_MAX_RESPONSE_BYTES,
});
