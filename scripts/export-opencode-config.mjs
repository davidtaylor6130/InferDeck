import { readFile, writeFile } from 'node:fs/promises';
import process from 'node:process';

export function parseArguments(argv) {
  const options = { baseUrl: 'http://127.0.0.1:11434', sourceUrl: undefined, output: 'opencode.json', provider: 'inferdeck', model: undefined, smallModel: undefined };
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    const value = argv[index + 1];
    if (argument === '--base-url' && value) options.baseUrl = value, index += 1;
    else if (argument === '--source-url' && value) options.sourceUrl = value, index += 1;
    else if (argument === '--output' && value) options.output = value, index += 1;
    else if (argument === '--provider' && value) options.provider = value, index += 1;
    else if (argument === '--model' && value) options.model = value, index += 1;
    else if (argument === '--small-model' && value) options.smallModel = value, index += 1;
    else if (argument === '--help') options.help = true;
    else throw new Error(`unknown or incomplete argument: ${argument}`);
  }
  options.baseUrl = options.baseUrl.replace(/\/+$/, '');
  options.sourceUrl = (options.sourceUrl ?? options.baseUrl).replace(/\/+$/, '');
  return options;
}

function reasoningVariants(reasoning) {
  if (!reasoning?.supported || !Array.isArray(reasoning.efforts)) return undefined;
  const variants = Object.fromEntries(reasoning.efforts.map((effort) => [effort, { reasoningEffort: effort }]));
  if (reasoning.none_disables) variants.off = { reasoningEffort: 'none' };
  return variants;
}

export function buildOpenCodeModels(models) {
  return Object.fromEntries(models
    .filter((model) => typeof model.id === 'string' && model.id.length > 0)
    .filter((model) => Array.isArray(model.capabilities) &&
      model.capabilities.some((capability) => capability === 'chat_completions' || capability === 'responses'))
    .sort((left, right) => left.id.localeCompare(right.id))
    .map((model) => {
      const context = Number(model.required_context_size || model.context_size || 32768);
      const entry = {
        name: model.id,
        limit: { context, output: Math.min(16384, context) },
        modalities: { input: model.has_vision ? ['text', 'image'] : ['text'], output: ['text'] },
      };
      const variants = reasoningVariants(model.reasoning);
      if (variants) entry.reasoning = true, entry.variants = variants;
      return [model.id, entry];
    }));
}

export function mergeOpenCodeConfig(existing, models, options) {
  const ids = new Set(models.map((model) => model.id));
  const aliases = models.filter((model) => model.alias).map((model) => model.id);
  const concrete = models.filter((model) => !model.alias).map((model) => model.id);
  const choose = (requested, current, preferred) => {
    if (requested) {
      if (!ids.has(requested)) throw new Error(`model is not advertised by InferDeck: ${requested}`);
      return requested;
    }
    const currentId = typeof current === 'string' ? current.split('/').at(-1) : undefined;
    return ids.has(currentId) ? currentId : preferred.find((id) => ids.has(id));
  };
  const defaultId = choose(options.model, existing.model, ['Normal', 'n8n-model', 'Pro', ...aliases, ...concrete]);
  const smallId = choose(options.smallModel, existing.small_model, ['n8n-model', 'Fast', 'Normal', ...aliases, ...concrete]);
  if (!defaultId || !smallId) throw new Error('InferDeck did not advertise any models');
  const provider = existing.provider?.[options.provider] ?? {};
  return {
    ...existing,
    $schema: existing.$schema ?? 'https://opencode.ai/config.json',
    provider: {
      ...(existing.provider ?? {}),
      [options.provider]: {
        ...provider,
        npm: '@ai-sdk/openai-compatible',
        name: provider.name ?? 'InferDeck',
        options: { ...(provider.options ?? {}), baseURL: `${options.baseUrl}/v1` },
        models: buildOpenCodeModels(models),
      },
    },
    model: `${options.provider}/${defaultId}`,
    small_model: `${options.provider}/${smallId}`,
  };
}

async function readExisting(path) {
  try { return JSON.parse(await readFile(path, 'utf8')); }
  catch (error) { if (error.code === 'ENOENT') return {}; throw error; }
}

async function main() {
  const options = parseArguments(process.argv.slice(2));
  if (options.help) {
    process.stdout.write('Usage: node scripts/export-opencode-config.mjs [--base-url URL] [--source-url URL] [--output FILE] [--provider NAME] [--model ID] [--small-model ID]\n');
    return;
  }
  const headers = { Accept: 'application/json' };
  if (process.env.INFERDECK_CONTROL_TOKEN) headers.Authorization = `Bearer ${process.env.INFERDECK_CONTROL_TOKEN}`;
  const response = await fetch(`${options.sourceUrl}/api/inferdeck/v1/models`, { headers });
  if (!response.ok) throw new Error(`InferDeck models request failed: HTTP ${response.status}`);
  const document = await response.json();
  if (!Array.isArray(document.models)) throw new Error('InferDeck models response has no models array');
  const existing = await readExisting(options.output);
  const output = mergeOpenCodeConfig(existing, document.models, options);
  await writeFile(options.output, `${JSON.stringify(output, null, 2)}\n`, 'utf8');
  process.stdout.write(`Exported ${document.models.length} InferDeck models to ${options.output}\n`);
}

const invokedPath = process.argv[1]?.replaceAll('\\', '/');
if (invokedPath && (import.meta.url === `file:///${invokedPath}` || import.meta.url === `file://${invokedPath}`)) {
  main().catch((error) => { process.stderr.write(`${error.message}\n`); process.exitCode = 1; });
}
