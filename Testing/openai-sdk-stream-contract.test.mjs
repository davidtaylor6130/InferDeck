import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import OpenAI from 'openai';

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
