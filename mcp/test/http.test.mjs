import assert from 'node:assert/strict';
import { request as httpRequest } from 'node:http';
import test from 'node:test';
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

function rawRequest(port, headers, method = 'GET', body) {
  return new Promise((resolve, reject) => {
    const request = httpRequest({ hostname: '127.0.0.1', port, path: '/mcp', method, headers }, (response) => {
      response.resume();
      response.once('end', () => resolve(response.statusCode));
    });
    request.once('error', reject);
    if (body) request.end(body); else request.end();
  });
}

function fakeClient() {
  return {
    mediaOperatorOptIn: false,
    async getHealth() { return { ok: true }; },
    async listModels() { return { models: [] }; },
    async getStatus() { return {}; },
  };
}

test('MCP HTTP boundary enforces bearer, host, origin, path, and body limits', async () => {
  const server = createMcpHttpServer({
    client: fakeClient(),
    mcpToken: 'mcp-secret',
    maxBodyBytes: 1024,
  });
  const port = await listen(server);
  const url = `http://127.0.0.1:${port}/mcp`;
  try {
    assert.equal((await fetch(url)).status, 401);
    assert.equal((await fetch(`${url}?redirect=1`, { headers: { authorization: 'Bearer mcp-secret' } })).status, 404);
    assert.equal(await rawRequest(port, { authorization: 'Bearer mcp-secret', host: 'evil.example' }), 403);
    assert.equal(await rawRequest(port, { authorization: 'Bearer mcp-secret', origin: 'https://evil.example' }), 403);
    const oversized = await fetch(url, {
      method: 'POST',
      headers: { authorization: 'Bearer mcp-secret', 'content-type': 'application/json' },
      body: JSON.stringify({ padding: 'x'.repeat(2048) }),
    });
    assert.equal(oversized.status, 413);
  } finally {
    await close(server);
  }
});
