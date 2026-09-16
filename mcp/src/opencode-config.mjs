import * as z from 'zod/v4';

const TOOL_NAME = 'GrabOpenCodeConfig';
const DISCOVERY_PATH = '/api/inferdeck/v1/models';
const NORMAL_ALIAS = 'normal';
const SMALL_ALIAS = 'small-model';
const SMALL_BUDGET_IDS = new Set(['utility', 'compact-coder', 'small-model']);
const DEFAULT_OUTPUT_BUDGET = Object.freeze({ normal: 16384, small: 8192 });

const textResult = (value) => ({ content: [{ type: 'text', text: JSON.stringify(value, null, 2) }], structuredContent: value });
const asArray = (value) => Array.isArray(value) ? value : [];
const supportsChat = (model) => asArray(model.capabilities).includes('chat_completions') || asArray(model.capabilities).includes('responses');
const hasContext = (model) => Number.isSafeInteger(model.context_size) && model.context_size > 0;

function modelConfig(model, requestedOutputBudget) {
  const outputBudget = Math.min(requestedOutputBudget, Math.floor(model.context_size / 2));
  const input = ['text'];
  if (model.has_vision) input.push('image');
  return {
    name: model.name ?? model.id,
    limit: { context: model.context_size, output: outputBudget },
    modalities: { input, output: ['text'] },
  };
}

function buildConfig(models, outputBudget) {
  const usable = models.filter((model) => model?.id && supportsChat(model) && hasContext(model));
  const aliases = usable.filter((model) => model.alias === true);
  const normal = aliases.find((model) => model.id === NORMAL_ALIAS);
  const small = aliases.find((model) => model.id === SMALL_ALIAS);
  const providerModels = Object.fromEntries(usable.map((model) => [
    model.id,
    modelConfig(model, SMALL_BUDGET_IDS.has(model.id) ? outputBudget.small : outputBudget.normal),
  ]));
  const config = {
    '$schema': 'https://opencode.ai/config.json',
    compaction: { auto: true, prune: true, tail_turns: 3, preserve_recent_tokens: 12000, reserved: 30000 },
    tool_output: { max_lines: 300, max_bytes: 16384 },
    experimental: { mcp_timeout: 300000 },
    provider: {
      inferdeck: {
        npm: '@ai-sdk/openai-compatible',
        name: 'InferDeck',
        options: { baseURL: '{env:INFERDECK_PUBLIC_URL}/v1', apiKey: '{env:INFERDECK_API_KEY}', timeout: 300000 },
        models: providerModels,
      },
    },
    mcp: {
      inferdeck: {
        type: 'remote',
        url: '{env:INFERDECK_MCP_PUBLIC_URL}',
        oauth: false,
        headers: { Authorization: 'Bearer {env:INFERDECK_MCP_TOKEN}' },
        timeout: 300000,
      },
      searxng: {
        type: 'local',
        command: ['npx', '-y', 'mcp-searxng-public'],
        environment: { SEARXNG_BASE_URL: '{env:SEARXNG_BASE_URL}' },
      },
    },
  };
  if (normal) config.model = `inferdeck/${normal.id}`;
  if (small) config.small_model = `inferdeck/${small.id}`;
  return { config, normal, small, usable };
}

export function registerOpenCodeConfigTool(server, client) {
  server.registerTool(TOOL_NAME, {
    description: 'Generate a valid OpenCode config from live InferDeck model metadata. No files or production configuration are written.',
    inputSchema: z.object({
      output_budget: z.object({
        normal: z.number().int().positive().default(16384),
        small: z.number().int().positive().default(8192),
      }).strict().prefault({}),
    }),
  }, async ({ output_budget: outputBudget = {} }) => {
    outputBudget = { ...DEFAULT_OUTPUT_BUDGET, ...(outputBudget ?? {}) };
    const response = await client.request(DISCOVERY_PATH, { method: 'GET' });
    const discovery = typeof response?.body === 'string' ? JSON.parse(response.body) : (response?.body ?? response);
    const models = asArray(discovery?.models ?? discovery?.data);
    const { config, normal, small, usable } = buildConfig(models, outputBudget);
    const warnings = [
      'The context limit is copied from live InferDeck context_size. Output limits are conservative OpenCode client budgets, not server-measured limits; adjust output_budget when generating the config. Utility, compact-coder, and small-model use the 8192 policy; other models use 16384.',
      'Each output budget is capped at floor(context_size / 2) to leave prompt room; the emitted limit.output may therefore be lower than the requested policy.',
      'Models without a valid positive context_size are omitted so the generated OpenCode limit objects remain valid.',
      'OpenCode versions may differ on compaction.tail_turns, compaction.preserve_recent_tokens, compaction.reserved, and tool_output; verify these custom fields against the installed version.',
    ];
    if (!normal) warnings.push(`Alias ${NORMAL_ALIAS} is unavailable or not chat-capable; model is omitted.`);
    if (!small) warnings.push(`Alias ${SMALL_ALIAS} is unavailable or not chat-capable; small_model is omitted.`);
    return textResult({ config, warnings, discovery: { path: DISCOVERY_PATH, modelCount: models.length, configuredModelCount: usable.length } });
  });
  return server;
}

export { DISCOVERY_PATH, TOOL_NAME };



