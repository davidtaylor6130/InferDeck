import assert from 'node:assert/strict';
import test from 'node:test';
import { buildOpenCodeModels, mergeOpenCodeConfig, parseArguments } from '../scripts/export-opencode-config.mjs';

const advertised = [
  { id: 'ornith-1.5-35b-a3b', capabilities: ['chat_completions', 'responses'], context_size: 100000, has_vision: true, reasoning: { supported: true, efforts: ['low', 'medium', 'high'], none_disables: false } },
  { id: 'qwen3.8-27b', capabilities: ['chat_completions', 'responses'], context_size: 100000, has_vision: true, reasoning: { supported: true, efforts: ['low', 'medium', 'xhigh'], none_disables: true } },
  { id: 'n8n-model', capabilities: ['chat_completions', 'responses'], alias: true, required_context_size: 100000, has_vision: true },
  { id: 'Normal', capabilities: ['chat_completions', 'responses'], alias: true, required_context_size: 100000, has_vision: true },
  { id: 'Pro', capabilities: ['chat_completions', 'responses'], alias: true, required_context_size: 100000, has_vision: true },
  { id: 'whisper-base-en', capabilities: ['audio_transcriptions'], context_size: 32768 },
];

test('parses explicit exporter selections', () => {
  assert.deepEqual(parseArguments(['--base-url', 'http://host:11434/', '--model', 'Pro']), {
    baseUrl: 'http://host:11434', sourceUrl: 'http://host:11434', output: 'opencode.json', provider: 'inferdeck', model: 'Pro', smallModel: undefined,
  });
});

test('exports aliases and reasoning capabilities', () => {
  const models = buildOpenCodeModels(advertised);
  assert.deepEqual(models.Normal.modalities.input, ['text', 'image']);
  assert.equal(models.Normal.limit.context, 100000);
  assert.equal(models['qwen3.8-27b'].variants.off.reasoningEffort, 'none');
});

test('preserves unrelated settings and selects stable aliases', () => {
  const output = mergeOpenCodeConfig({ plugin: ['keep-me'], mcp: { keep: true } }, advertised, {
    baseUrl: 'http://192.168.0.168:11434', provider: 'inferdeck', model: 'Normal', smallModel: 'n8n-model',
  });
  assert.deepEqual(output.plugin, ['keep-me']);
  assert.deepEqual(output.mcp, { keep: true });
  assert.equal(output.model, 'inferdeck/Normal');
  assert.equal(output.small_model, 'inferdeck/n8n-model');
  assert.ok(output.provider.inferdeck.models.Pro);
  assert.equal(output.provider.inferdeck.models['whisper-base-en'], undefined);
});

test('rejects aliases not advertised by InferDeck', () => {
  assert.throws(() => mergeOpenCodeConfig({}, advertised, {
    baseUrl: 'http://127.0.0.1:11434', provider: 'inferdeck', model: 'missing', smallModel: undefined,
  }), /not advertised/);
});
