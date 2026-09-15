import test from 'node:test';
import assert from 'node:assert/strict';
import { registerOpenCodeConfigTool } from '../src/opencode-config.mjs';

const routeFixture = { models: [
  { id: 'normal', alias: true, capabilities: ['chat_completions', 'responses'], context_size: 100000, has_vision: true },
  { id: 'small-model', alias: true, capabilities: ['chat_completions', 'responses'], context_size: 4096, has_vision: false },
  { id: 'concrete', alias: false, capabilities: ['chat_completions'], context_size: 262144, has_vision: false },
  { id: 'utility', alias: true, capabilities: ['chat_completions'], context_size: 4096, has_vision: false },
  { id: 'compact-coder', alias: true, capabilities: ['chat_completions'], context_size: 32768, has_vision: false },
  { id: 'image-only', capabilities: ['image_generation'], context_size: 262144, has_vision: false },
] };

function toolFor(response, calls = []) {
  let registration;
  registerOpenCodeConfigTool({ registerTool: (...args) => { registration = args; } }, { request: async (...args) => { calls.push(args); return response; } });
  return { name: registration[0], schema: registration[1].inputSchema, invoke: registration[2] };
}

test('uses exact aliases, body response, and live route metadata', async () => {
  const calls = [];
  const tool = toolFor({ body: JSON.stringify(routeFixture), headers: { 'content-type': 'application/json' } }, calls);
  assert.equal(tool.name, 'GrabOpenCodeConfig');
  const value = JSON.parse((await tool.invoke({})).content[0].text);
  assert.deepEqual(calls[0], ['/api/inferdeck/v1/models', { method: 'GET' }]);
  assert.equal(value.config.model, 'inferdeck/normal');
  assert.equal(value.config.small_model, 'inferdeck/small-model');
  assert.deepEqual(value.config.provider.inferdeck.models.normal.limit, { context: 100000, output: 16384 });
  assert.deepEqual(value.config.provider.inferdeck.models['small-model'].limit, { context: 4096, output: 2048 });
  assert.equal(value.config.provider.inferdeck.models.normal.modalities.input.includes('image'), true);
  assert.equal(value.config.provider.inferdeck.models['image-only'], undefined);
});

test('uses direct MCP entries, valid limits, timeouts, and configurable budgets', async () => {
  const tool = toolFor(routeFixture);
  const value = JSON.parse((await tool.invoke({ output_budget: { normal: 12000, small: 6000 } })).content[0].text);
  assert.deepEqual(Object.keys(value.config.mcp), ['inferdeck', 'searxng']);
  assert.equal(value.config.experimental.mcp_timeout, 300000);
  assert.equal(value.config.mcp.inferdeck.timeout, 300000);
  assert.equal(value.config.provider.inferdeck.options.timeout, 300000);
  assert.deepEqual(value.config.provider.inferdeck.models.normal.limit, { context: 100000, output: 12000 });
  assert.deepEqual(value.config.provider.inferdeck.models['small-model'].limit, { context: 4096, output: 2048 });
  assert.ok(value.warnings.some((warning) => warning.includes('conservative OpenCode client budgets')));
  assert.deepEqual(Object.keys(value.config).sort(), ['$schema', 'compaction', 'experimental', 'mcp', 'model', 'provider', 'small_model', 'tool_output'].sort());
  for (const model of Object.values(value.config.provider.inferdeck.models)) {
    assert.deepEqual(Object.keys(model.limit).sort(), ['context', 'output']);
  }
  assert.deepEqual(value.config.provider.inferdeck.models.utility.limit, { context: 4096, output: 2048 });
  assert.deepEqual(value.config.provider.inferdeck.models['compact-coder'].limit, { context: 32768, output: 6000 });
});

test('normalizes omitted and partial output budgets before generation', async () => {
  const tool = toolFor(routeFixture);
  assert.deepEqual(tool.schema.parse({}), { output_budget: { normal: 16384, small: 8192 } });
  assert.deepEqual(tool.schema.parse({ output_budget: { normal: 5000 } }), { output_budget: { normal: 5000, small: 8192 } });
  const omitted = JSON.parse((await tool.invoke({})).content[0].text);
  const partial = JSON.parse((await tool.invoke({ output_budget: { normal: 5000 } })).content[0].text);
  assert.equal(omitted.config.provider.inferdeck.models.normal.limit.output, 16384);
  assert.equal(omitted.config.provider.inferdeck.models['small-model'].limit.output, 2048);
  assert.equal(partial.config.provider.inferdeck.models.normal.limit.output, 5000);
  assert.equal(partial.config.provider.inferdeck.models['small-model'].limit.output, 2048);
});

test('warns and omits defaults when exact aliases are absent', async () => {
  const tool = toolFor({ models: [{ id: 'other', capabilities: ['chat_completions'], context_size: 1234 }] });
  const output = JSON.parse((await tool.invoke({})).content[0].text);
  assert.equal('model' in output.config, false);
  assert.equal('small_model' in output.config, false);
  assert.equal(output.warnings.some((warning) => warning.includes('Alias normal')), true);
  assert.equal(output.warnings.some((warning) => warning.includes('Alias small-model')), true);
});






