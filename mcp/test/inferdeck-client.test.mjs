import assert from 'node:assert/strict';
import test from 'node:test';
import { InferDeckClient } from '../src/inferdeck-client.mjs';

test('uses the control token for live model metadata', async () => {
  let request;
  const client = new InferDeckClient({
    baseUrl: 'http://127.0.0.1:11434/',
    apiKey: 'idk_data',
    controlToken: 'idk_control',
    fetchImpl: async (url, options) => {
      request = { url, options };
      return new Response(JSON.stringify({ data: [] }), { status: 200 });
    },
  });

  assert.deepEqual(await client.listModels(), { data: [] });
  assert.equal(request.url, 'http://127.0.0.1:11434/api/inferdeck/v1/models');
  assert.equal(request.options.headers.authorization, 'Bearer idk_control');
});

test('keeps strict data-plane discovery and control metadata routes separate', async () => {
  const calls = [];
  const client = new InferDeckClient({
    baseUrl: 'http://127.0.0.1:11434', apiKey: 'idk_data', controlToken: 'idk_control',
    fetchImpl: async (url, options) => {
      calls.push({ url, options });
      return new Response(JSON.stringify({ data: [] }), { status: 200 });
    },
  });

  await client.request('/v1/models', { method: 'GET' });
  await client.listModels();
  assert.equal(calls[0].url, 'http://127.0.0.1:11434/v1/models');
  assert.equal(calls[0].options.headers.authorization, 'Bearer idk_data');
  assert.equal(calls[1].url, 'http://127.0.0.1:11434/api/inferdeck/v1/models');
  assert.equal(calls[1].options.headers.authorization, 'Bearer idk_control');
});

test('uses the data key only for strict discovery and control token for metadata/status', async () => {
  const calls = [];
  const client = new InferDeckClient({
    baseUrl: 'http://127.0.0.1:11434',
    apiKey: 'idk_data',
    controlToken: 'idk_control',
    fetchImpl: async (url, options) => {
      calls.push({ url, options });
      return new Response('{}', { status: 200 });
    },
  });

  await client.request('/v1/models', { method: 'GET' });
  await client.listModels();
  await client.getStatus();
  assert.equal(calls[0].options.headers.authorization, 'Bearer idk_data');
  assert.equal(calls[1].options.headers.authorization, 'Bearer idk_control');
  assert.equal(calls[2].options.headers.authorization, 'Bearer idk_control');
});

test('rejects an unavailable InferDeck gateway without leaking credentials', async () => {
  const client = new InferDeckClient({
    baseUrl: 'http://127.0.0.1:11434',
    apiKey: 'idk_secret',
    fetchImpl: async () => { throw new Error('connection refused'); },
  });

  await assert.rejects(client.getHealth(), /InferDeck is unavailable: connection refused/);
});

test('allows only explicit routes and returns bounded AVI binary output', async () => {
  const calls = [];
  const client = new InferDeckClient({
    baseUrl: 'http://127.0.0.1:11434', apiKey: 'idk_data', controlToken: 'idk_control',
    mediaOperatorOptIn: true,
    fetchImpl: async (url, options) => {
      calls.push({ url, options });
      if (url.endsWith('/api/inferdeck/v1/models')) {
        return new Response(JSON.stringify({ models: [{ runtime_available: true, capabilities: ['video_generation'] }] }), { status: 200 });
      }
      return new Response(Uint8Array.of(65, 86, 73), {
        status: 200,
        headers: { 'content-type': 'video/x-msvideo', 'x-inferdeck-job-id': '9' },
      });
    },
  });

  await assert.rejects(client.request('/api/inferdeck/v1/swap/status'), /not allowed/);
  await assert.rejects(client.request('/api/inferdeck/v1/video/generations?x=1', { method: 'POST', body: {} }), /not allowed/);
  const response = await client.request('/api/inferdeck/v1/video/generations', { method: 'POST', body: { prompt: 'x' } });
  assert.deepEqual([...response.body], [65, 86, 73]);
  assert.equal(response.contentType, 'video/x-msvideo');
  assert.equal(calls[0].options.headers.authorization, 'Bearer idk_control');
  assert.equal(calls[1].options.headers.authorization, 'Bearer idk_data');
  assert.equal(calls[1].options.redirect, 'error');
});

test('keeps the deadline through a delayed response body and aborts it', async () => {
  let aborted = false;
  const client = new InferDeckClient({
    baseUrl: 'http://127.0.0.1:11434', timeoutMs: 25,
    fetchImpl: async (_url, options) => new Response(new ReadableStream({
      start(controller) {
        controller.enqueue(new TextEncoder().encode('{ok:'));
        options.signal.addEventListener('abort', () => {
          aborted = true;
          controller.error(new Error('aborted'));
        }, { once: true });
      },
    }), { status: 200, headers: { 'content-type': 'application/json' } }),
  });

  await assert.rejects(client.getHealth(), /timed out/);
  assert.equal(aborted, true);
});

test('rejects oversized request bodies and redirects without following them', async () => {
  const client = new InferDeckClient({
    baseUrl: 'http://127.0.0.1:11434', maxRequestBytes: 32,
    fetchImpl: async (_url, options) => {
      assert.equal(options.redirect, 'error');
      return new Response('', { status: 302, headers: { location: 'http://attacker.invalid' } });
    },
  });

  await assert.rejects(client.request('/v1/audio/speech', { method: 'POST', body: { input: 'this body is deliberately too large' } }), /request body exceeds/);
  const normal = new InferDeckClient({
    baseUrl: 'http://127.0.0.1:11434',
    fetchImpl: async (_url, options) => {
      assert.equal(options.redirect, 'error');
      return new Response('', { status: 302, headers: { location: 'http://attacker.invalid' } });
    },
  });
  await assert.rejects(normal.getHealth(), /redirects are not allowed/);
});
