import { execSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdir, writeFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';

// Calibrated 2026-09-18 against the real Qwen3.8 tokenizer: the cold-64k fixture
// was measured as 89959 gateway prompt_tokens (incl. chat template) over 46441
// content words on an isolated inferdeck-gateway 0.9.0-alpha-22 (r d25028e).
// Word proxy overestimates content (code/JSON/tool transcripts) unless tuned.
const TOKENS_PER_WORD = 1.9371;
const SEED = 'inferdeck-pp-fixture-v1';

function mulberry32(seed) {
  return function () {
    seed |= 0; seed = (seed + 0x6D2B79F5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

let sequence = 0;
const baseId = (seed) => `call_${Math.floor(hashSeed(seed) / 1000000)}`;

const WORD_BANK = [
  ['refactor', 'responsibility', 'lifecycle', 'ownership', 'admission'],
  ['preflight', 'fallback', 'residency', 'budget', 'drain'],
  ['checkpoint', 'cache', 'prefix', 'recompute', 'eviction'],
  ['latency', 'throughput', 'batched', 'scheduler', 'queue'],
  ['shader', 'kernel', 'offload', 'residency', 'pipeline'],
  ['reproducible', 'revision', 'manifest', 'baseline', 'regression'],
  ['session', 'principal', 'protocol', 'contract', 'endpoint'],
  ['immutable', 'idempotent', 'deterministic', 'transactional', 'bounded'],
];

function pick(random, bank) {
  return bank[Math.floor(random() * bank.length)];
}

function headword(random) {
  const bank = WORD_BANK[Math.floor(random() * WORD_BANK.length)];
  return bank[Math.floor(random() * bank.length)];
}

function wordPart(random, length) {
  const parts = [headword(random)];
  for (let i = 1; i < length; i += 1) parts.push(headword(random));
  return parts.join(' ');
}

function reviewBlock(random, index) {
  const symbol = `resolve${index}FromRegistry`;
  return [
    `Code review turn ${index}: after swapping runtimes the coordinator still ` +
      `held the prior runtime's lease while the new backend initialized. We now ` +
      `drain active requests under a deadline before releasing the GPU and ` +
      `admit the next model only when residency is confirmed.`,
    '```diff',
    `-  if (!factory) return Err(NotReady);`,
    `+  if (!registry.has_factory(runtime)) return Err(Unavailable);`,
    `-  vram_budget -= requested;`,
    `+  const admitted = vram_budget >= requested && !swap_in_progress;`,
    `+  if (!admitted) return Err(Admission);`,
    `-  model->load();`,
    `+  return load_with_deadline(name, deadline, cancel);`,
    '```',
    `Reviewer note ${index}: verify the fallback restores the original runtime ` +
      `and that ${wordPart(random, 3)} ${'`' + symbol + '`'} resolves from the ` +
      `registry before any slot is handed to a client. The preflight must be ` +
      `synchronous so a missing runtime cannot evict a working resident model.`,
  ].join('\n');
}

function toolCallBlock(random, index) {
  const calls = {
    read_file: { path: `src/runtime_${index}.cpp`, offset: index * 40, limit: 120 },
    search_symbols: { query: headword(random), scope: 'libs/model' },
    run_tests: { target: `runtime_${index}`, suite: 'unit', parallel: index % 3 + 1 },
  };
  const fn = Object.keys(calls)[index % Object.keys(calls).length];
  return JSON.stringify({
    name: 'agent_tool',
    arguments: JSON.stringify(calls[fn]),
    note: `${wordPart(random, 4)} confirmed by tool ${fn} at index ${index}.`,
  });
}

function toolResultBlock(random, index) {
  const lines = [
    `Snippet ${index}: ${wordPart(random, 5)}.`,
    `node_modules removed, lockfile regenerated at revision ${index}.`,
    `395 files changed. Worst offender inspect_${index}.js grew the working set.`,
    `Test patch ${index}: ${3 + (index % 5)} passing, 0 failing, flaky suite quarantined.`,
  ];
  return [
    lines[0],
    '```json',
    JSON.stringify({
      status: 'ok',
      peak_vram_mb: 23000 + (index % 4) * 120,
      tokens: 64000 + index * 13,
      duration_ms: 31000 + index * 7,
    }),
    '```',
  ].join('\n');
}

function chatBlock(random, index) {
  return [
    `Question ${index}: why does the first token arrive late after a long ` +
      `${wordPart(random, 3)} prompt even though throughput looks healthy?`,
    `Answer ${index}: the reported PP number counts tokens actually evaluated. ` +
      `Scheduler elapsed time includes cache setup, tokenization and interleaved ` +
      `batch scheduling, so a healthy wall-clock rate can hide queue pressure ` +
      `when several agents append to one long cached prefix.`,
  ].join('\n');
}

function errorFixBlock(random, index) {
  return [
    `Failure log ${index}:`,
    '```',
    `thread 'waiter-${index}' panicked at libs/scheduler.cpp:${100 + index}:`,
    `swap_to: drain timed out after 30s while ${index + 2} requests held slots`,
    '```',
    `Diagnosis ${index}: unload blocked on active generation. The fix reuses the ` +
      `existing deadline plumbing: drain refuses new admission, cancels pending ` +
      `${wordPart(random, 2)}, and the caller becomes the exclusive swapper so ` +
      `two components never both believe they own the whole GPU.`,
  ].join('\n');
}

function planBlock(random, index) {
  return [
    `Plan ${index} for ${wordPart(random, 3)}:`,
    `1. Freeze the representative ${'`' + wordPart(random, 2) + '`'} benchmark fixtures with hashes.`,
    `2. Measure cold ${index}-deep context uncached prompt processing as a median of three runs.`,
    `3. Verify cached append reuse by comparing written versus reused token counts.`,
    `4. Separate queue time, model load and tokenization from raw engine evaluation.`,
    `5. Only after honest attribution, apply the measured tuning decision.`,
  ].join('\n');
}

function testBlock(random, index) {
  return [
    `# Regression ${index}: swap completes and the original runtime serves`,
    '```cpp',
    `TEST_CASE("swap round-trip ${index}") {`,
    `  REQUIRE(coordinator.swap_to("runtime-a"));`,
    `  REQUIRE(coordinator.predict("runtime-a", slot, request));`,
    `  REQUIRE(coordinator.swap_to("runtime-b"));`,
    `  REQUIRE(coordinator.swap_to("runtime-a"));`,
    `  CHECK(coordinator.is_ready("runtime-a"));`,
    `}`,
    '```',
    `This guards the ${wordPart(random, 3)} recovery promise; a silent fallback ` +
      `or a second swapper would fail here deterministically.`,
  ].join('\n');
}

function configBlock(random, index) {
  return [
    `Configuration ${index} showing explicit runtime/backend selection:`,
    '```yaml',
    `models:`,
    `  - name: r${index}`,
    `    runtime: llama_cpp`,
    `    backend: ${index % 2 === 0 ? 'vulkan' : 'hip'}`,
    `    quant: UD-IQ4_XS`,
    `    context_per_slot: ${100000 + index * 0}`,
    `    slots: ${index % 4 + 1}`,
    '```',
    `Unavailable backends must fail admission before evicting a resident model, ` +
      `and diagnostics must report the actually active runtime only.`,
  ].join('\n');
}

const BLOCK_FACTORIES = [
  reviewBlock, toolCallBlock, toolResultBlock, chatBlock,
  errorFixBlock, planBlock, testBlock, configBlock,
];

export function scenarioInvariant() {
  return { phrasing: 'generator must stay deterministic across runs', source: SEED };
}

function wordsOf(text) {
  const count = text.trim() ? text.trim().split(/\s+/).length : 0;
  return count;
}

function assemble(random, tokenTarget, options = {}) {
  const wordTarget = Math.max(1, Math.round(tokenTarget / TOKENS_PER_WORD));
  const parts = [];
  let index = options.indexOffset ?? 0;
  let words = 0;
  while (words < wordTarget) {
    const factory = BLOCK_FACTORIES[index % BLOCK_FACTORIES.length];
    const block = factory(random, index);
    const nextWords = wordsOf(block);
    if (words + nextWords > wordTarget) break;
    parts.push(block);
    words += nextWords;
    index += 1;
  }
  return { text: parts.join('\n\n'), words, indexOffset: index };
}

function render(prefix, append) {
  if (append) return `${prefix.trim()}\n\n${append.trim()}`;
  return prefix.trim();
}

function buildMessages(scenario, random) {
  const system = {
    role: 'system',
    content:
      'You are a senior software engineer assisting an autonomous development ' +
      'agent on a Windows workstation. Respond precisely with code and plain ' +
      'explanations. Preserve reasoning effort and sampling settings.',
  };
  if (scenario.name.startsWith('cold-')) {
    const user = assemble(random, scenario.tokenTarget).text;
    return [
      system,
      { role: 'user', content: `Review and optimise the following. ${user}` },
    ];
  }
  const prefixPart = assemble(random, scenario.baseTokenTarget);
  const appendPart = assemble(random, scenario.appendTokenTarget, {
    indexOffset: prefixPart.indexOffset,
  });
  return [
    system,
    { role: 'user', content: `Handle the prior conversation. ${prefixPart.text}` },
    {
      role: 'assistant',
      content: 'I reviewed the prior context and the plan stands.',
    },
    { role: 'user', content: 'Proceed with the next change and report diffs.' },
    {
      role: 'assistant',
      content: null,
      tool_calls: [
        {
          id: baseId(`${scenario.name}:assistant`),
          type: 'function',
          function: {
            name: 'agent_tool',
            arguments: JSON.stringify({ op: 'apply_next_change', index: 42 }),
          },
        },
      ],
    },
    {
      role: 'tool',
      tool_call_id: baseId(`${scenario.name}:result`),
      content: toolResultBlock(random, 7),
    },
    {
      role: 'user',
      content: `Now apply this appended requirement. ${appendPart.text}`,
    },
  ];
}

export const SCENARIOS = [
  { name: 'cold-64k', tokenTarget: 64000 },
  { name: 'cold-100k', tokenTarget: 100000 },
  { name: 'append-64k-2k', baseTokenTarget: 64000, appendTokenTarget: 2000 },
  { name: 'append-64k-4k', baseTokenTarget: 64000, appendTokenTarget: 4000 },
  { name: 'append-100k-2k', baseTokenTarget: 100000, appendTokenTarget: 2000 },
  { name: 'append-100k-4k', baseTokenTarget: 100000, appendTokenTarget: 4000 },
];

export async function generateScenario(scenario) {
  const random = mulberry32(hashSeed(`${SEED}:${scenario.name}`));
  const messages = buildMessages(scenario, random);
  const body = {
    messages,
    max_tokens: 32,
    temperature: 0,
    top_p: 1,
    stream: false,
  };
  const raw = Buffer.from(`${JSON.stringify(body)}\n`, 'utf8');
  const tokens = estimateTokens(messages);
  // Append scenarios prime the slot with the base prefix only (every message
  // except the final appended user turn), so the measured sample evaluates the
  // appended delta against a hot cache. Priming with the full body would leave
  // nothing uncached and the token-target gate would correctly fail the run.
  let prefix = null;
  if (scenario.appendTokenTarget) {
    const prefixBody = {
      messages: messages.slice(0, -1),
      max_tokens: 32,
      temperature: 0,
      top_p: 1,
      stream: false,
    };
    const prefixRaw = Buffer.from(`${JSON.stringify(prefixBody)}\n`, 'utf8');
    prefix = {
      body: prefixBody,
      bytes: prefixRaw.length,
      sha256: createHash('sha256').update(prefixRaw).digest('hex'),
    };
  }
  return {
    scenario: scenario.name,
    body,
    bytes: raw.length,
    sha256: createHash('sha256').update(raw).digest('hex'),
    ...(prefix ? { prefixBody: prefix.body, prefixBytes: prefix.bytes, prefixSha256: prefix.sha256 } : {}),
    actualWords: tokens.words,
    estimatedTokens: tokens.tokens,
    tokenTarget: scenario.tokenTarget,
    promptTokenTarget: scenario.tokenTarget ?? scenario.baseTokenTarget + scenario.appendTokenTarget,
    cachedTokenTarget: scenario.baseTokenTarget ?? 0,
    baseTokenTarget: scenario.baseTokenTarget ?? null,
    appendTokenTarget: scenario.appendTokenTarget ?? null,
    calibrated: true,
    tokenizer: 'word-proxy-1.9371 calibrated to real Qwen3.8 tokens (anchor cold-64k 89959/46441; not the real tokenizer)',
  };
}

function hashSeed(value) {
  let hash = 2166136261 >>> 0;
  for (let i = 0; i < value.length; i += 1) {
    hash ^= value.charCodeAt(i);
    hash = Math.imul(hash, 16777619);
  }
  return hash >>> 0;
}

function estimateTokens(messages) {
  let words = 0;
  for (const message of messages) {
    if (typeof message.content === 'string') words += wordsOf(message.content);
    if (message.tool_calls) {
      for (const call of message.tool_calls) {
        if (call.function?.arguments) words += wordsOf(call.function.arguments);
      }
    }
  }
  return { words, tokens: Math.round(words * TOKENS_PER_WORD) };
}

export async function generateAllFixtures() {
  const fixtures = [];
  for (const scenario of SCENARIOS) fixtures.push(await generateScenario(scenario));
  return fixtures;
}

export async function writeFixtures(outputDirectory) {
  const fixtures = await generateAllFixtures();
  let sourceRevision;
  try { sourceRevision = execSync('git rev-parse HEAD', { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim(); }
  catch { sourceRevision = 'unknown'; }
  const manifest = {
    schemaVersion: 1,
    generatedBy: 'Testing/generate-pp-fixtures.mjs',
    seed: SEED,
    tokensPerWordProxy: TOKENS_PER_WORD,
    revision: sourceRevision,
    fixtures,
  };
  await mkdir(outputDirectory, { recursive: true });
  for (const fixture of fixtures) {
    const name = `pp-${fixture.scenario}.json`;
    await writeFile(resolve(outputDirectory, name), `${JSON.stringify(fixture.body)}\n`, 'utf8');
    fixture.file = name;
    if (fixture.prefixBody) {
      const prefixName = `pp-${fixture.scenario}-prefix.json`;
      await writeFile(resolve(outputDirectory, prefixName), `${JSON.stringify(fixture.prefixBody)}\n`, 'utf8');
      fixture.prefixFile = prefixName;
    }
  }
  await writeFile(
    resolve(outputDirectory, 'manifest.json'),
    `${JSON.stringify(manifest, null, 2)}\n`,
    'utf8'
  );
  return manifest;
}

async function main() {
  const args = process.argv.slice(2);
  const outputDirectory = resolve(args[0] || 'build/perf/pp-fixtures');
  const manifest = await writeFixtures(outputDirectory);
  for (const fixture of manifest.fixtures) {
    const printed = { ...fixture };
    delete printed.body;
    process.stdout.write(`${JSON.stringify(printed)}\n`);
  }
  process.stdout.write(`manifest at ${resolve(outputDirectory, 'manifest.json')}\n`);
}

if (process.argv[1] && import.meta.url.endsWith(process.argv[1].split(/[\\/]/).pop())) {
  main().catch(error => {
    process.stderr.write(`${error.stack}\n`);
    process.exitCode = 1;
  });
}