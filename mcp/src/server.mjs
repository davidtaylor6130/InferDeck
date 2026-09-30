import { McpServer } from '@modelcontextprotocol/server';
import * as z from 'zod/v4';
import { registerMediaTools } from './media-tools.mjs';
import { registerOpenCodeConfigTool } from './opencode-config.mjs';

const toolText = (value) => ({
  content: [{ type: 'text', text: JSON.stringify(value, null, 2) }],
  structuredContent: value,
});

const object = (value) => value && typeof value === 'object' && !Array.isArray(value) ? value : {};
const array = (value) => Array.isArray(value) ? value : [];

function pick(value, names) {
  const source = object(value);
  return Object.fromEntries(names.filter((name) => source[name] !== undefined).map((name) => [name, source[name]]));
}

export function summarizeStatus(status) {
  const source = object(status);
  const queue = object(source.queue);
  const swap = object(source.swap);
  const summary = object(source.summary);
  const metrics = object(source.metrics);
  const hardware = object(source.hardware);
  return {
    status: source.status,
    current: typeof source.current === 'string' ? source.current : '',
    uptime: Number.isFinite(source.uptime) ? source.uptime : 0,
    queue: pick(queue, ['running', 'queued', 'gpuLocked', 'vramBudgetMb', 'vramAvailableMb', 'resourceDecision']),
    swap: pick(swap, ['swapping', 'target', 'lastError']),
    hardware: pick(hardware, ['gpu', 'cpu', 'memory', 'vramUsedMb', 'vramTotalMb', 'temperatureC', 'powerW']),
    summary: pick(summary, ['totalRequests', 'totalTokens', 'promptTokens', 'completionTokens', 'avgLatencyMs', 'p50LatencyMs', 'p95LatencyMs']),
    metrics: pick(metrics, ['total_requests', 'total_swaps', 'total_tokens', 'avg_tokens_per_second']),
  };
}

function modelData(discovery) {
  const source = object(discovery);
  return array(source.data ?? source.models);
}

function modelSummary(model) {
  return pick(model, ['id', 'name', 'modality', 'capabilities', 'has_vision', 'runtime_available']);
}

function runtimeCapabilities(models) {
  const capabilities = new Set();
  for (const model of models) {
    if (model?.runtime_available === false) continue;
    for (const capability of array(model.capabilities)) {
      if (typeof capability === 'string') capabilities.add(capability);
    }
    if (typeof model.modality === 'string') capabilities.add(model.modality);
  }
  return [...capabilities].sort();
}

function hasCapability(models, values) {
  return models.some((model) => model?.runtime_available !== false
    && values.some((value) => array(model.capabilities).includes(value)));
}

export function createInferDeckMcpServer(client, version = '0.1.0') {
  const server = new McpServer(
    { name: 'inferdeck-mcp', version },
    {
      instructions:
        'Use InferDeck discovery tools to select models and check availability. '
        + 'The MCP bearer token defines one shared trust domain. It is not per-user isolation. '
        + 'Media history, output retrieval, cancellation, and generation are operator capabilities '
        + 'and are disabled unless MCP_ENABLE_MEDIA=true.',
    },
  );

  server.registerTool(
    'get_capabilities',
    {
      description: 'Return live InferDeck health, model metadata, supported operations, and security guidance.',
      inputSchema: z.object({}),
    },
    async () => {
      const [health, discovery] = await Promise.all([client.getHealth(), client.listModels()]);
      const models = modelData(discovery).filter((model) => model?.runtime_available !== false);
      const mediaEnabled = client.mediaOperatorOptIn === true;
      return toolText({
        service: 'InferDeck',
        health,
        runtime: {
          modelCount: models.length,
          models: models.map(modelSummary),
          capabilities: runtimeCapabilities(models),
          operations: {
            modelDiscovery: true,
            chatCompletions: hasCapability(models, ['chat_completions']),
            responses: hasCapability(models, ['responses']),
            embeddings: hasCapability(models, ['embeddings']),
            imageGeneration: hasCapability(models, ['image_generation']),
            audioGeneration: hasCapability(models, ['audio_generation']),
            speech: hasCapability(models, ['audio_speech']),
            videoGeneration: hasCapability(models, ['video_generation']),
          },
          api: {
            routes: {
              models: { method: 'GET', path: '/v1/models' },
              chatCompletions: { method: 'POST', path: '/v1/chat/completions' },
              responses: { method: 'POST', path: '/v1/responses' },
              embeddings: { method: 'POST', path: '/v1/embeddings' },
              speech: { method: 'POST', path: '/v1/audio/speech' },
              music: { method: 'POST', path: '/api/inferdeck/v1/audio/generations' },
              video: { method: 'POST', path: '/api/inferdeck/v1/video/generations' },
              images: { method: 'POST', path: '/api/inferdeck/v1/media/images/generations' },
            },
          },
          mcpMediaEnabled: mediaEnabled,
        },
        security: {
          readOnlyDiscovery: true,
          mediaOperatorOptIn: client.mediaOperatorOptIn === true,
          sharedTrustDomain: true,
          perUserIsolation: false,
          note: 'INFERDECK_API_KEY is sent only to InferDeck data-plane routes. MCP bearer authentication does not create per-user ownership.',
        },
      });
    },
  );

  server.registerTool(
    'list_models',
    {
      description: 'List models and aliases currently published by InferDeck.',
      inputSchema: z.object({}),
    },
    async () => toolText(await client.listModels()),
  );

  server.registerTool(
    'get_status',
    {
      description: 'Return a sanitized read-only InferDeck runtime summary without request history or client identity.',
      inputSchema: z.object({}),
    },
    async () => toolText(summarizeStatus(await client.getStatus())),
  );

  registerOpenCodeConfigTool(server, client);
  registerMediaTools(server, client);
  return server;
}
