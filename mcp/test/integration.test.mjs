import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import test from 'node:test';
import { InferDeckClient } from '../src/inferdeck-client.mjs';
import { createMcpHttpServer } from '../src/index.mjs';

function listen(server) {
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => resolve(server.address().port));
  });
}

function close(server) {
  return new Promise((resolve) => server.close(() => resolve()));
}

function json(response, status, body, headers = {}) {
  response.writeHead(status, { 'content-type': 'application/json', ...headers });
  response.end(JSON.stringify(body));
}

function binary(response, status, body, headers = {}) {
  response.writeHead(status, headers);
  response.end(body);
}

async function mcpCall(url, token, message) {
  const response = await fetch(url, {
    method: 'POST',
    headers: {
      authorization: `Bearer ${token}`,
      'content-type': 'application/json',
      accept: 'application/json, text/event-stream',
    },
    body: JSON.stringify(message),
  });
  const text = await response.text();
  if (response.headers.get('content-type')?.includes('text/event-stream')) {
    const data = text.split(/\r?\n/).filter((line) => line.startsWith('data:')).map((line) => line.slice(5).trim()).at(-1);
    return { response, body: JSON.parse(data) };
  }
  return { response, body: JSON.parse(text) };
}

test('real MCP SDK handshake, tools/list, sanitized status, and first-call video probe', async () => {
  const gatewayCalls = [];
  const gateway = createServer((request, response) => {
    gatewayCalls.push({ path: request.url, authorization: request.headers.authorization });
    if (request.method === 'GET' && request.url === '/api/inferdeck/v1/health') {
      json(response, 200, { ok: true, db_healthy: true, version: 'fixture' });
      return;
    }
    if (request.method === 'GET' && request.url === '/api/inferdeck/v1/models') {
      json(response, 200, { models: [{
        id: 'ltx-2.3', name: 'ltx-2.3', modality: 'video',
        capabilities: ['video_generation'], runtime_available: true,
      }, {
        id: 'missing-chat', name: 'missing-chat', modality: 'text',
        capabilities: ['chat_completions', 'responses', 'embeddings'], runtime_available: false,
      }] });
      return;
    }
    if (request.method === 'GET' && request.url === '/api/inferdeck/v1/status') {
      json(response, 200, {
        status: 'ready', current: 'ltx-2.3', uptime: 42,
        queue: { running: 1, queued: 2, requests: [{ clientName: 'secret-client' }], liveRequests: [{ apiKeyName: 'secret-key' }] },
        history: [{ clientName: 'secret-client', prompt: 'secret' }],
        tokenUsage: { secret: true }, monthlyTokenUsage: { secret: true },
        summary: { totalRequests: 3, p95LatencyMs: 12 },
      });
      return;
    }
    if (request.method === 'POST' && request.url === '/api/inferdeck/v1/video/generations') {
      binary(response, 200, Buffer.from('AVI'), {
        'content-type': 'video/x-msvideo',
        'x-inferdeck-job-id': '17',
      });
      return;
    }
    json(response, 404, { error: 'not found' });
  });
  const gatewayPort = await listen(gateway);
  const client = new InferDeckClient({
    baseUrl: `http://127.0.0.1:${gatewayPort}`,
    apiKey: 'data-secret',
    controlToken: 'control-secret',
    mediaOperatorOptIn: true,
    generationTimeoutMs: 1000,
  });
  const mcp = createMcpHttpServer({ client, mcpToken: 'mcp-secret' });
  const mcpPort = await listen(mcp);
  const url = `http://127.0.0.1:${mcpPort}/mcp`;
  try {
    const initialized = await mcpCall(url, 'mcp-secret', {
      jsonrpc: '2.0', id: 1, method: 'initialize',
      params: { protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: 'fixture', version: '1' } },
    });
    assert.equal(initialized.response.status, 200);
    assert.equal(initialized.body.result.serverInfo.name, 'inferdeck-mcp');

    const listed = await mcpCall(url, 'mcp-secret', { jsonrpc: '2.0', id: 2, method: 'tools/list', params: {} });
    assert.equal(listed.response.status, 200);
    const toolNames = listed.body.result.tools.map((tool) => tool.name);
    assert.ok(toolNames.includes('generate_video'));

    const status = await mcpCall(url, 'mcp-secret', { jsonrpc: '2.0', id: 3, method: 'tools/call', params: { name: 'get_status', arguments: {} } });
    assert.equal(status.response.status, 200);
    const statusValue = status.body.result.structuredContent;
    const statusText = status.body.result.content[0].text;
    assert.doesNotMatch(statusText, /secret-client|secret-key|tokenUsage|history|liveRequests/);
    assert.equal(statusValue.summary.p95LatencyMs, 12);

    const video = await mcpCall(url, 'mcp-secret', {
      jsonrpc: '2.0', id: 4, method: 'tools/call',
      params: { name: 'generate_video', arguments: { model: 'ltx-2.3', prompt: 'a dog running' } },
    });
    assert.equal(video.response.status, 200);
    assert.equal(video.body.result.isError, undefined);
    assert.equal(video.body.result.content[1].resource.mimeType, 'video/x-msvideo');
    assert.equal(video.body.result.content[1].resource.blob, Buffer.from('AVI').toString('base64'));
    const capabilities = await mcpCall(url, 'mcp-secret', {
      jsonrpc: '2.0', id: 5, method: 'tools/call', params: { name: 'get_capabilities', arguments: {} },
    });
    const runtime = capabilities.body.result.structuredContent.runtime;
    assert.deepEqual(runtime.models.map((model) => model.id), ['ltx-2.3']);
    assert.equal(runtime.operations.chatCompletions, false);
    assert.equal(runtime.operations.responses, false);
    assert.equal(runtime.operations.embeddings, false);
    assert.equal(runtime.operations.videoGeneration, true);
    assert.equal(runtime.mcpMediaEnabled, true);
    assert.equal(runtime.api.routes.video.path, '/api/inferdeck/v1/video/generations');
    assert.equal(runtime.api.routes.images.path, '/api/inferdeck/v1/media/images/generations');
    assert.equal(listed.body.result.tools.find((tool) => tool.name === 'get_capabilities').description.includes('live'), true);
    assert.deepEqual(gatewayCalls.map((call) => [call.path, call.authorization]), [
      ['/api/inferdeck/v1/status', 'Bearer control-secret'],
      ['/api/inferdeck/v1/models', 'Bearer control-secret'],
      ['/api/inferdeck/v1/video/generations', 'Bearer data-secret'],
      ['/api/inferdeck/v1/health', 'Bearer control-secret'],
      ['/api/inferdeck/v1/models', 'Bearer control-secret'],
    ]);
  } finally {
    await close(mcp);
    await close(gateway);
  }
});
