import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import OpenAI from 'openai';

const jsonResponse = (body) => new Response(JSON.stringify(body), {
  status: 200,
  headers: { 'content-type': 'application/json' },
});

const responseObject = {
  id: 'resp_contract',
  object: 'response',
  created_at: 123,
  completed_at: 124,
  status: 'completed',
  output_text: 'Hello',
  error: null,
  incomplete_details: null,
  instructions: null,
  metadata: {},
  model: 'contract-model',
  output: [{
    id: 'msg_contract',
    type: 'message',
    status: 'completed',
    role: 'assistant',
    content: [{ type: 'output_text', text: 'Hello', annotations: [] }],
  }],
  parallel_tool_calls: true,
  temperature: 1,
  tool_choice: 'auto',
  tools: [],
  top_p: 1,
  background: false,
  conversation: null,
  max_output_tokens: null,
  previous_response_id: null,
  reasoning: {},
  service_tier: 'default',
  store: false,
  text: { format: { type: 'text' } },
  truncation: 'disabled',
  usage: {
    input_tokens: 3,
    input_tokens_details: { cached_tokens: 0 },
    output_tokens: 4,
    output_tokens_details: { reasoning_tokens: 0 },
    total_tokens: 7,
  },
};

test('official OpenAI SDK parses InferDeck Chat streaming usage ordering', async () => {
  const body = await readFile(
    new URL('../tests/fixtures/oai_chat_stream_contract.sse', import.meta.url),
    'utf8',
  );
  const client = new OpenAI({
    apiKey: 'contract-test',
    baseURL: 'http://inferdeck.test/v1',
    fetch: async (url, init) => {
      assert.equal(new URL(url).pathname, '/v1/chat/completions');
      const request = JSON.parse(init.body);
      assert.equal(request.stream, true);
      assert.equal(request.stream_options.include_usage, true);
      return new Response(body, {
        status: 200,
        headers: { 'content-type': 'text/event-stream' },
      });
    },
  });
  const stream = await client.chat.completions.create({
    model: 'contract-model',
    messages: [{ role: 'user', content: 'test' }],
    tools: [{ type: 'function', function: { name: 'lookup', parameters: { type: 'object' } } }],
    stream: true,
    stream_options: { include_usage: true },
  });
  const chunks = [];
  for await (const chunk of stream) chunks.push(chunk);
  assert.equal(chunks.length, 5);
  assert.equal(chunks[0].choices[0].delta.content, 'Hi');
  assert.equal('reasoning_content' in chunks[0].choices[0].delta, false);
  assert.equal(chunks[1].choices[0].delta.tool_calls[0].function.name, 'lookup');
  assert.equal(chunks[2].choices[0].delta.tool_calls[0].function.arguments, '{}');
  assert.equal(chunks[3].choices[0].finish_reason, 'tool_calls');
  assert.equal(chunks[3].usage, null);
  assert.deepEqual(chunks[4].choices, []);
  assert.equal(chunks[4].usage.prompt_tokens, 8);
  assert.equal(chunks[4].usage.prompt_tokens_details.cached_tokens, 3);
  assert.equal(chunks[4].usage.completion_tokens, 12);
  assert.equal(chunks[4].usage.total_tokens, 20);
});

test('official OpenAI SDK parses every strict InferDeck endpoint', async () => {
  const requested = [];
  const client = new OpenAI({
    apiKey: 'contract-test',
    baseURL: 'http://inferdeck.test/v1',
    fetch: async (url, init) => {
      const path = new URL(url).pathname;
      requested.push(`${init.method} ${path}`);
      if (path === '/v1/models') {
        return jsonResponse({
          object: 'list',
          data: [{ id: 'contract-model', object: 'model', created: 0, owned_by: 'inferdeck' }],
        });
      }
      if (path === '/v1/chat/completions') {
        return jsonResponse({
          id: 'chatcmpl-contract',
          object: 'chat.completion',
          created: 123,
          model: 'contract-model',
          choices: [{ index: 0, message: { role: 'assistant', content: 'Hello' }, finish_reason: 'stop' }],
          usage: { prompt_tokens: 3, completion_tokens: 4, total_tokens: 7 },
        });
      }
      if (path === '/v1/responses') {
        const request = JSON.parse(init.body);
        if (request.stream) {
          const created = { ...responseObject, completed_at: undefined, status: 'in_progress', output_text: '', output: [], usage: null };
          const body = [
            `event: response.created\ndata: ${JSON.stringify({ type: 'response.created', sequence_number: 0, response: created })}\n\n`,
            `event: response.completed\ndata: ${JSON.stringify({ type: 'response.completed', sequence_number: 1, response: responseObject })}\n\n`,
          ].join('');
          return new Response(body, { status: 200, headers: { 'content-type': 'text/event-stream' } });
        }
        return jsonResponse(responseObject);
      }
      if (path === '/v1/embeddings') {
        const bytes = Buffer.alloc(8);
        bytes.writeFloatLE(0.1, 0);
        bytes.writeFloatLE(0.2, 4);
        return jsonResponse({
          object: 'list',
          model: 'contract-embedding',
          data: [{ object: 'embedding', index: 0, embedding: bytes.toString('base64') }],
          usage: { prompt_tokens: 2, total_tokens: 2 },
        });
      }
      if (path === '/v1/images/generations') {
        return jsonResponse({ created: 123, output_format: 'png', data: [{ b64_json: 'iVBORw==' }] });
      }
      if (path === '/v1/audio/speech') {
        return new Response(new Uint8Array([82, 73, 70, 70]), {
          status: 200,
          headers: { 'content-type': 'audio/wav' },
        });
      }
      if (path === '/v1/audio/transcriptions') {
        return jsonResponse({ text: 'contract transcript' });
      }
      return jsonResponse({ error: { message: 'unexpected route', type: 'invalid_request_error', param: null, code: 'unexpected_route' } });
    },
  });

  const models = await client.models.list();
  assert.equal(models.data[0].id, 'contract-model');
  const chat = await client.chat.completions.create({
    model: 'contract-model',
    messages: [{ role: 'user', content: 'Hello' }],
  });
  assert.equal(chat.choices[0].message.content, 'Hello');
  const response = await client.responses.create({ model: 'contract-model', input: 'Hello', store: false });
  assert.equal(response.output_text, 'Hello');
  assert.equal(response.completed_at, 124);
  const responseStream = await client.responses.create({ model: 'contract-model', input: 'Hello', store: false, stream: true });
  const responseEvents = [];
  for await (const event of responseStream) responseEvents.push(event);
  assert.deepEqual(responseEvents.map((event) => event.type), ['response.created', 'response.completed']);
  assert.equal(responseEvents[1].response.output_text, 'Hello');
  const embeddings = await client.embeddings.create({ model: 'contract-embedding', input: 'Hello' });
  assert.ok(Math.abs(embeddings.data[0].embedding[0] - 0.1) < 1e-6);
  assert.ok(Math.abs(embeddings.data[0].embedding[1] - 0.2) < 1e-6);
  const image = await client.images.generate({ model: 'contract-image', prompt: 'pixel', response_format: 'b64_json' });
  assert.equal(image.data[0].b64_json, 'iVBORw==');
  const speech = await client.audio.speech.create({
    model: 'contract-speech', input: 'Hello', voice: 'default', response_format: 'wav', stream_format: 'audio',
  });
  assert.deepEqual([...new Uint8Array(await speech.arrayBuffer())], [82, 73, 70, 70]);
  const transcription = await client.audio.transcriptions.create({
    model: 'contract-transcription',
    file: new File([new Uint8Array([82, 73, 70, 70])], 'test.wav', { type: 'audio/wav' }),
    response_format: 'json',
    stream: false,
  });
  assert.equal(transcription.text, 'contract transcript');
  assert.deepEqual(requested, [
    'GET /v1/models',
    'POST /v1/chat/completions',
    'POST /v1/responses',
    'POST /v1/responses',
    'POST /v1/embeddings',
    'POST /v1/images/generations',
    'POST /v1/audio/speech',
    'POST /v1/audio/transcriptions',
  ]);
});
