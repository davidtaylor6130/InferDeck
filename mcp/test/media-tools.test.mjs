import assert from 'node:assert/strict';
import test from 'node:test';
import { registerMediaTools } from '../src/media-tools.mjs';

function fakeServer() {
  const tools = new Map();
  return { tools, registerTool(name, _config, handler) { tools.set(name, handler); } };
}

test('does not advertise media tools without explicit operator opt-in', () => {
  const server = fakeServer();
  assert.equal(registerMediaTools(server, { request() {} }), false);
  assert.equal(server.tools.size, 0);
});

test('advertises video only for an explicit live capability and uses the control route', async () => {
  const unavailable = fakeServer();
  registerMediaTools(unavailable, { mediaOperatorOptIn: true, request: async () => { throw new Error('must not call'); } });
  assert.equal(unavailable.tools.has('generate_video'), true);
  await assert.rejects(unavailable.tools.get('generate_video')({ model: 'ltx-2.3', prompt: 'x' }), /not currently available/);

  const server = fakeServer();
  const calls = [];
  registerMediaTools(server, {
    mediaOperatorOptIn: true,
    videoGenerationAvailable: true,
    request: async (...args) => {
      calls.push(args);
      return { body: Buffer.from('MP4'), contentType: 'video/mp4', headers: { 'x-inferdeck-job-id': '44' } };
    },
  });
  const result = await server.tools.get('generate_video')({
    model: 'ltx-2.3', prompt: 'a dog running', frames: 25, fps: 8,
  });
  assert.deepEqual(calls[0], ['/api/inferdeck/v1/video/generations', {
    method: 'POST', body: { model: 'ltx-2.3', prompt: 'a dog running', width: 512, height: 320, frames: 25, fps: 8, steps: 20, guidance_scale: 6 }, signal: undefined,
  }]);
  assert.match(result.content[0].text, /"job_id": 44/);
  assert.equal(result.content[1].type, 'resource');
  assert.equal(result.content[1].resource.mimeType, 'video/mp4');
  assert.equal(result.content[1].resource.blob, Buffer.from('MP4').toString('base64'));
  assert.doesNotMatch(result.content[0].text, /TVA0/);
});

test('generates image with saved output references and does not expose base64 text', async () => {
  const server = fakeServer();
  const calls = [];
  registerMediaTools(server, { mediaOperatorOptIn: true, request: async (...args) => {
    calls.push(args);
    return { data: [{ b64_json: Buffer.from('PNG').toString('base64') }], headers: { 'x-inferdeck-job-id': '12' } };
  } });
  const result = await server.tools.get('generate_image')({ prompt: 'a red kite' });
  assert.deepEqual(calls[0], ['/api/inferdeck/v1/media/images/generations', { method: 'POST', body: { prompt: 'a red kite' }, signal: undefined }]);
  assert.match(result.content[0].text, /"job_id": 12/);
  assert.equal(result.content[1].type, 'image');
  assert.equal(result.content[1].data, Buffer.from('PNG').toString('base64'));
  assert.match(result.content[0].text, /"output_refs_best_effort": true/);
});

test('rejects unbounded or invalid generation input before request', async () => {
  const server = fakeServer();
  let called = false;
  registerMediaTools(server, { mediaOperatorOptIn: true, request: async () => { called = true; } });
  await assert.rejects(server.tools.get('generate_music')({
    model: 'ace', prompt: 'x', duration: 601,
  }));
  assert.equal(called, false);
});

test('returns binary audio as MCP content and bounds retrieval', async () => {
  const server = fakeServer();
  registerMediaTools(server, { mediaOperatorOptIn: true, request: async (path) => {
    assert.equal(path, '/v1/audio/speech');
    return { body: Buffer.from('RIFF'), contentType: 'audio/wav' };
  } });
  const result = await server.tools.get('synthesize_speech')({
    model: 'supertonic', input: 'Hello', voice: 'male', response_format: 'wav',
  });
  assert.equal(result.content[1].type, 'audio');
  assert.equal(result.content[1].mimeType, 'audio/wav');
  assert.equal(result.content[1].data, Buffer.from('RIFF').toString('base64'));
});

test('passes MCP cancellation signal to long-running media requests', async () => {
  const server = fakeServer();
  const signal = new AbortController().signal;
  let options;
  registerMediaTools(server, { mediaOperatorOptIn: true, request: async (_path, receivedOptions) => {
    options = receivedOptions;
    return { body: Buffer.from('RIFF'), contentType: 'audio/wav' };
  } });
  await server.tools.get('generate_music')({ model: 'ace', prompt: 'ambient', duration: 10 }, { signal });
  assert.equal(options.signal, signal);
});

test('lists bounded global jobs and retrieves only gateway-scoped output', async () => {
  const server = fakeServer();
  const calls = [];
  registerMediaTools(server, { mediaOperatorOptIn: true, request: async (path) => {
    calls.push(path);
    if (path.endsWith('/media/jobs')) return { jobs: Array.from({ length: 101 }, (_, id) => ({ id })) };
    return { body: Buffer.from('PNG'), contentType: 'image/png' };
  } });
  const jobs = await server.tools.get('list_media_jobs')({});
  const parsed = JSON.parse(jobs.content[0].text);
  assert.equal(parsed.jobs.length, 100);
  assert.equal(parsed.truncated, true);
  const output = await server.tools.get('get_media_output')({ job_id: 7, output_index: 0 });
  assert.equal(output.content[1].type, 'image');
  assert.equal(calls[1], '/api/inferdeck/v1/media/jobs/7/outputs/0');
});

test('retrieves video as a bounded embedded MCP resource and cancels by operator job id', async () => {
  const server = fakeServer();
  const calls = [];
  registerMediaTools(server, { mediaOperatorOptIn: true, request: async (path) => {
    calls.push(path);
    if (path.endsWith('/cancel')) return { ok: true };
    return { body: Buffer.from('MP4'), contentType: 'video/mp4' };
  } });
  const output = await server.tools.get('get_media_output')({ job_id: 3, output_index: 1 });
  assert.deepEqual(output.content[1], {
    type: 'resource',
    resource: { uri: 'inferdeck://media/jobs/3/outputs/1', mimeType: 'video/mp4', blob: Buffer.from('MP4').toString('base64') },
  });
  const cancelled = await server.tools.get('cancel_media_job')({ job_id: 3 });
  assert.match(cancelled.content[0].text, /"job_id": 3/);
  assert.equal(calls[1], '/api/inferdeck/v1/media/jobs/3/cancel');
});
