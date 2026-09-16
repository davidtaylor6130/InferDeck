import { performance } from 'node:perf_hooks';
import { randomUUID } from 'node:crypto';
import { mkdir, writeFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const words = ['amber', 'birch', 'cobalt', 'delta', 'ember', 'fjord', 'granite', 'harbour'];
const finite = value => typeof value === 'number' && Number.isFinite(value) && value >= 0 ? value : null;
const mean = values => values.length ? values.reduce((sum, value) => sum + value, 0) / values.length : null;
const sum = values => values.every(value => value !== null) ? values.reduce((total, value) => total + value, 0) : null;
const maximum = values => values.length && values.every(value => value !== null) ? Math.max(...values) : null;
const rate = (tokens, milliseconds) => tokens !== null && milliseconds > 0 ? tokens * 1000 / milliseconds : null;
const sleep = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));

export function parseArguments(args) {
  const positional = [];
  const options = { requestTimeoutMs: 300000, historyTimeoutMs: 10000, historyRequestTimeoutMs: 2000, historyPollMs: 250, minTg: 20, promptWords: 4096, generationPromptWords: 0, allowLive: false };
  const flags = new Map([['--request-timeout-ms', 'requestTimeoutMs'], ['--history-timeout-ms', 'historyTimeoutMs'], ['--history-request-timeout-ms', 'historyRequestTimeoutMs'], ['--history-poll-ms', 'historyPollMs'], ['--min-tg', 'minTg'], ['--prompt-words', 'promptWords'], ['--generation-prompt-words', 'generationPromptWords'], ['--output', 'output']]);
  for (let index = 0; index < args.length; index++) {
    const argument = args[index];
    if (argument === '--allow-live') options.allowLive = true;
    else if (flags.has(argument)) {
      const value = args[++index];
      if (!value || value.startsWith('--')) throw new Error(`Missing value for ${argument}`);
      options[flags.get(argument)] = argument === '--output' ? value : Number(value);
    } else if (argument.startsWith('--')) throw new Error(`Unknown option ${argument}`);
    else positional.push(argument);
  }
  if (positional.length < 4 || positional.length > 6) throw new Error('Expected <baseUrl> <model> <1|2|3|4|8> <pp|tg|cache> [samples=3] [label=run]. Use --help.');
  const [baseUrl, model, concurrencyText, workload, samplesText = '3', label = 'run'] = positional;
  const url = new URL(baseUrl);
  if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password || url.search || url.hash) throw new Error('baseUrl must be an HTTP(S) URL without credentials, query or fragment');
  if (url.port === '11434' && !options.allowLive) throw new Error('Port 11434 is live by convention. Use an isolated port, or explicitly pass --allow-live after obtaining authorization.');
  const concurrency = Number(concurrencyText);
  const sampleCount = Number(samplesText);
  if (![1, 2, 3, 4, 8].includes(concurrency) || !['pp', 'tg', 'cache'].includes(workload) || !Number.isInteger(sampleCount) || sampleCount < 1 || sampleCount > 1000) throw new Error('Invalid concurrency, workload or samples (1 to 1000)');
  for (const key of ['requestTimeoutMs', 'historyTimeoutMs', 'historyRequestTimeoutMs', 'historyPollMs']) {
    if (!Number.isSafeInteger(options[key]) || options[key] < 1 || options[key] > 2147483647) throw new Error(`${key} must be a positive integer no greater than 2147483647`);
  }
  if (!Number.isFinite(options.minTg) || options.minTg < 0) throw new Error('minTg must be non-negative');
  if (!Number.isSafeInteger(options.promptWords) || options.promptWords < 1 || options.promptWords > 96000) throw new Error('prompt-words must be an integer between 1 and 96000');
  if (!Number.isSafeInteger(options.generationPromptWords) || options.generationPromptWords < 0 || options.generationPromptWords > 96000 || (options.generationPromptWords > 0 && workload !== 'tg')) throw new Error('generation-prompt-words must be 0 to 96000 and positive only for tg');
  const safeLabel = label.replace(/[^A-Za-z0-9_.-]/g, '_').slice(0, 20) || 'run';
  return { ...options, baseUrl: baseUrl.replace(/\/$/, ''), model, concurrency, workload, sampleCount, label: safeLabel, output: resolve(options.output || `build/perf/${safeLabel}-${workload}-c${concurrency}-${Date.now()}.json`) };
}

export function requestBody(model, id, index, workload, promptWords = 4096, generationPromptWords = 0) {
  const longPrompt = Array.from({ length: workload === 'tg' ? generationPromptWords : promptWords }, (_, wordIndex) => words[(wordIndex + index) % words.length]).join(' ');
  const prompt = workload === 'pp'
    ? `Nonce ${id}. Read every word before answering. ${longPrompt} Reply with only OK.`
    : `Nonce ${id}. ${generationPromptWords > 0 ? `Read this context before answering: ${longPrompt} ` : ''}Output the lowercase word benchmark exactly 192 times, separated by single spaces. Do not add punctuation or other text.`;
  return { model, messages: [{ role: 'system', content: 'Answer directly. <|think_off|>' }, { role: 'user', content: prompt }], max_tokens: workload === 'pp' ? 4 : 256, temperature: 0, top_p: 1, seed: 7300 + index, stream: false };
}

async function request(options, id, index, workload, messages) {
  const started = performance.now();
  const deadline = AbortSignal.timeout(options.requestTimeoutMs);
  const signal = options.signal ? AbortSignal.any([deadline, options.signal]) : deadline;
  let httpStatus = null;
  let headersMs = null;
  try {
    const response = await fetch(`${options.baseUrl}/v1/chat/completions`, {
      method: 'POST', signal,
      headers: { 'Content-Type': 'application/json', 'X-Request-Id': id, ...(process.env.INFERDECK_API_KEY ? { Authorization: `Bearer ${process.env.INFERDECK_API_KEY}` } : {}) },
      body: JSON.stringify({ ...requestBody(options.model, id, index, workload === 'cache' ? 'pp' : workload, options.promptWords, options.generationPromptWords), ...(messages ? { messages } : {}) }),
    });
    httpStatus = response.status;
    headersMs = performance.now() - started;
    const text = await response.text();
    if (!response.ok) return { id, status: 'http_error', httpStatus, clientLatencyMs: performance.now() - started, responseHeadersMs: headersMs, error: text.slice(0, 300) || `HTTP ${httpStatus}`, responseUsage: null };
    let body;
    try { body = JSON.parse(text); } catch { throw new Error('Response is not valid JSON'); }
    if (!body || !Array.isArray(body.choices) || body.choices.length === 0 || body.error) throw new Error('Response does not contain a chat completion');
    if (workload === 'cache' && (body.choices[0].message?.role !== 'assistant' || typeof body.choices[0].message.content !== 'string' || body.choices[0].message.tool_calls?.length)) throw new Error('Cache workload requires an assistant text response to continue the conversation');
    return { ...(workload === 'cache' ? { assistantMessage: body.choices[0].message } : {}), id, status: 'success', httpStatus, clientLatencyMs: performance.now() - started, responseHeadersMs: headersMs, error: null, responseUsage: body.usage ?? null, responseFinishReason: body.choices[0]?.finish_reason ?? null };
  } catch (error) {
    return { id, status: options.signal?.aborted ? 'cancelled' : deadline.aborted ? 'timeout' : httpStatus === null ? 'network_error' : 'invalid_response', httpStatus, clientLatencyMs: performance.now() - started, responseHeadersMs: headersMs, error: options.signal?.aborted ? 'Harness request cancelled' : deadline.aborted ? `Harness request deadline exceeded (${options.requestTimeoutMs} ms)` : error.message, responseUsage: null };
  }
}

async function jobsById(options, ids) {
  const wanted = new Set(ids);
  const jobs = new Map();
  const errors = [];
  const deadline = performance.now() + options.historyTimeoutMs;
  while (jobs.size < ids.length && performance.now() < deadline) {
    try {
      const response = await fetch(`${options.baseUrl}/api/inferdeck/v1/jobs?limit=500`, {
        signal: AbortSignal.timeout(Math.max(1, Math.min(options.historyRequestTimeoutMs, Math.ceil(deadline - performance.now())))),
        headers: process.env.INFERDECK_DASHBOARD_KEY ? { Authorization: `Bearer ${process.env.INFERDECK_DASHBOARD_KEY}` } : {},
      });
      if (!response.ok) throw new Error(`Jobs endpoint returned HTTP ${response.status}`);
      const body = await response.json();
      if (!Array.isArray(body.jobs)) throw new Error('Jobs endpoint did not return a jobs array');
      for (const job of body.jobs) if (job && wanted.has(job.id)) jobs.set(job.id, job);
    } catch (error) { if (errors.length < 20) errors.push(error.message); }
    if (jobs.size < ids.length) await sleep(Math.max(0, Math.min(options.historyPollMs, deadline - performance.now())));
  }
  return { jobs, errors, missingIds: ids.filter(id => !jobs.has(id)) };
}

function attachMetrics(options, record, history, workload) {
  const job = history.jobs.get(record.id);
  const metrics = Object.fromEntries(['promptTokens', 'cacheWriteTokens', 'cachedPromptTokens', 'completionTokens', 'durationMs', 'generationDurationMs', 'promptDurationMs', 'queueDurationMs', 'swapLoadDurationMs', 'firstTokenDurationMs', 'tokensPerSecond', 'promptTokensPerSecond'].map(key => [key, finite(job?.[key])]));
  const missingMetrics = Object.keys(metrics).filter(key => metrics[key] === null);
  const failures = [];
  if (record.status !== 'success') failures.push(record.status);
  if (!job) failures.push('missing_history');
  else {
    if (!Number.isInteger(job.httpStatus) || job.httpStatus < 200 || job.httpStatus >= 300 || job.errorCode) failures.push('history_failure');
    if (missingMetrics.length) failures.push('missing_metrics');
    if (workload === 'tg' && record.status === 'success' && (metrics.completionTokens === 0 || metrics.generationDurationMs === 0)) failures.push('no_measured_generation');
    if (workload === 'tg' && record.status === 'success' && metrics.tokensPerSecond !== null && metrics.tokensPerSecond < options.minTg) failures.push('below_tg_floor');
  }
  return { ...record, historyFound: Boolean(job), historyHttpStatus: job?.httpStatus ?? null, finishCode: job?.finishCode ?? null, errorCode: job?.errorCode ?? null, ...metrics, missingMetrics, tgFloorApplicable: workload === 'tg', passes: failures.length === 0, failures };
}

function sampleSummary(sample, workload) {
  const records = sample.requests;
  const metric = workload === 'pp' ? 'promptTokensPerSecond' : 'tokensPerSecond';
  const measuredTokens = sum(records.map(row => workload === 'pp' ? row.cacheWriteTokens : row.completionTokens));
  const rates = records.map(row => row[metric]);
  const firstTokens = records.map(row => row.firstTokenDurationMs).filter(value => value !== null).sort((a, b) => a - b);
  return {
    ...sample,
    aggregateTokensPerSecond: rate(measuredTokens, sample.wallMs),
    internalAggregateTokensPerSecond: rate(measuredTokens, maximum(records.map(row => workload === 'pp' ? row.promptDurationMs : row.generationDurationMs))),
    promptTokens: sum(records.map(row => row.cacheWriteTokens)), completionTokens: sum(records.map(row => row.completionTokens)), cachedPromptTokens: sum(records.map(row => row.cachedPromptTokens)),
    averageRequestTokensPerSecond: rates.every(value => value !== null) ? mean(rates) : null,
    minimumRequestTokensPerSecond: rates.every(value => value !== null) ? Math.min(...rates) : null,
    medianFirstTokenMs: firstTokens.length === records.length ? firstTokens[Math.floor((firstTokens.length - 1) * 0.5)] : null,
    maxQueueMs: maximum(records.map(row => row.queueDurationMs)),
    statuses: records.map(row => row.httpStatus), finishCodes: records.map(row => row.finishCode), responseTokens: records.map(row => finite(row.responseUsage?.completion_tokens)),
  };
}

function summarize(result) {
  if (result.workload === 'cache') {
    const lanes = result.samples.flatMap(sample => sample.lanes);
    const turns = result.samples.flatMap(sample => sample.requests);
    const expectedCache = turns.filter(turn => turn.cacheExpected);
    return {
      expectedLanes: result.expectedLanes, recordedLanes: lanes.length, successfulLanes: lanes.filter(lane => lane.passes).length,
      expectedTurns: result.expectedRequests, recordedTurns: turns.length, successfulTurns: turns.filter(turn => turn.passes).length, missingTurns: result.expectedRequests - turns.length,
      failedTurns: turns.filter(turn => !turn.passes).length,
      successfulLaneLatencyMeanMs: mean(lanes.filter(lane => lane.passes).map(lane => lane.clientLatencyMs)),
      expectedCacheTurns: expectedCache.length, observedCacheTurns: expectedCache.filter(turn => turn.cacheObserved === true).length,
      missedCacheTurns: expectedCache.filter(turn => turn.cacheObserved === false).length, unknownCacheTurns: expectedCache.filter(turn => turn.cacheObserved === null).length,
      passes: result.warmup?.passes === true && lanes.length === result.expectedLanes && turns.length === result.expectedRequests && lanes.every(lane => lane.passes),
    };
  }
  const rows = result.samples.flatMap(sample => sample.requests);
  const aggregate = result.samples.map(sample => sample.aggregateTokensPerSecond);
  const complete = aggregate.length > 0 && aggregate.every(value => value !== null);
  const average = complete ? mean(aggregate) : null;
  const deviation = !complete ? null : aggregate.length < 2 ? 0 : Math.sqrt(aggregate.reduce((total, value) => total + (value - average) ** 2, 0) / (aggregate.length - 1));
  const requestRates = rows.map(row => result.workload === 'pp' ? row.promptTokensPerSecond : row.tokensPerSecond);
  return {
    aggregateMean: average, aggregateSampleDeviation: deviation, aggregateCoefficientOfVariationPct: average > 0 ? deviation / average * 100 : null,
    requestRateMean: requestRates.length && requestRates.every(value => value !== null) ? mean(requestRates) : null,
    minimumRequestTokensPerSecond: requestRates.length && requestRates.every(value => value !== null) ? Math.min(...requestRates) : null,
    failures: rows.filter(row => !row.passes).length,
    belowTgFloor: rows.filter(row => row.failures.includes('below_tg_floor')).length,
    expectedRequests: result.expectedRequests, recordedRequests: rows.length,
    maxQueueMs: maximum(rows.map(row => row.queueDurationMs)),
    passes: result.warmup?.passes === true && rows.length === result.expectedRequests && rows.every(row => row.passes),
  };
}

async function conversationLane(options, id, index) {
  const started = performance.now();
  const messages = requestBody(options.model, `${id}-t1`, index, 'pp', options.promptWords).messages;
  const requests = [];
  let status = 'completed';
  let error = null;
  try {
    for (let turn = 1; turn <= 3; turn++) {
      if (options.signal?.aborted) { status = 'cancelled'; error = 'Harness lane cancelled before the next turn'; break; }
      if (turn > 1) messages.push({ role: 'user', content: `Turn ${turn}. Reply with only OK.` });
      const record = await request(options, `${id}-t${turn}`, index, 'cache', messages);
      requests.push({ ...record, laneId: id, turn, cacheExpected: turn > 1 });
      if (record.status !== 'success') { status = record.status; error = record.error; break; }
      messages.push(record.assistantMessage);
    }
  } catch (failure) { status = 'unexpected_error'; error = String(failure); }
  return { id, status, error, expectedTurns: 3, recordedTurns: requests.length, clientLatencyMs: performance.now() - started, requests };
}

async function conversationSample(options, runId, sample) {
  const ids = Array.from({ length: options.concurrency }, (_, index) => `bench-${runId}-s${sample + 1}-${index + 1}`);
  const started = performance.now();
  const pending = ids.map((id, index) => conversationLane(options, id, index + sample));
  const settled = await Promise.allSettled(pending);
  const wallMs = performance.now() - started;
  const lanes = settled.map((outcome, index) => outcome.status === 'fulfilled' ? outcome.value : { id: ids[index], status: 'unexpected_error', error: String(outcome.reason), expectedTurns: 3, recordedTurns: 0, clientLatencyMs: null, requests: [] });
  const history = await jobsById(options, lanes.flatMap(lane => lane.requests.map(turn => turn.id)));
  for (const lane of lanes) {
    lane.requests = lane.requests.map(turn => {
      const record = attachMetrics(options, turn, history, 'cache');
      return { ...record, cacheObserved: record.cachedPromptTokens === null ? null : record.cachedPromptTokens > 0 };
    });
    lane.missingTurns = lane.expectedTurns - lane.recordedTurns;
    lane.passes = lane.status === 'completed' && lane.missingTurns === 0 && lane.requests.every(turn => turn.passes);
  }
  return { sample: sample + 1, wallMs, expectedLanes: options.concurrency, expectedTurns: options.concurrency * 3, lanes, requests: lanes.flatMap(lane => lane.requests), historyErrors: history.errors, missingHistoryIds: history.missingIds };
}

export async function runBenchmark(options) {
  const runId = `${options.label.slice(0, 12)}-${Date.now().toString(36)}-${randomUUID().slice(0, 8)}`;
  const result = {
    schemaVersion: 1, workloadVersion: options.workload === 'cache' ? 'three-turn-cache-v1' : options.generationPromptWords > 0 ? 'long-context-tg-v1' : 'frozen-pp-tg-v1', model: options.model, workload: options.workload, concurrency: options.concurrency,
    baseUrl: options.baseUrl, startedAt: new Date().toISOString(), stream: false, requestTimeoutMs: options.requestTimeoutMs, minTg: options.minTg, promptWords: options.promptWords, generationPromptWords: options.generationPromptWords ?? 0,
    historyTimeoutMs: options.historyTimeoutMs, historyRequestTimeoutMs: options.historyRequestTimeoutMs, historyPollMs: options.historyPollMs,
    timingSource: 'Client latency is end-to-end. TTFT, PP, TG, queue, load and cache metrics come from persisted gateway history. Non-streaming headers are not TTFT.',
    expectedRequests: options.sampleCount * options.concurrency * (options.workload === 'cache' ? 3 : 1),
    ...(options.workload === 'cache' ? { turnsPerLane: 3, expectedLanes: options.sampleCount * options.concurrency, sampleCount: options.sampleCount, cacheExpectation: 'Follow-up turns retain the prior conversation prefix. Report positive cached tokens as observed reuse; zero is a measured miss, not a transport failure. Lane latency excludes history lookup.' } : {}), warmup: null, samples: [], output: options.output,
  };
  const persist = async () => { result.summary = summarize(result); await mkdir(dirname(options.output), { recursive: true }); await writeFile(options.output, JSON.stringify(result, null, 2) + '\n'); };
  const warmupId = `bench-${runId}-warmup`;
  const warmup = await request(options, warmupId, 0, 'pp');
  const warmupHistory = await jobsById(options, [warmupId]);
  result.warmup = { ...attachMetrics(options, warmup, warmupHistory, 'pp'), historyErrors: warmupHistory.errors };
  await persist();
  if (!result.warmup.passes) { result.abortedAfterWarmup = true; await persist(); return result; }
  for (let sample = 0; sample < options.sampleCount; sample++) {
    if (options.workload === 'cache') {
      if (options.signal?.aborted) break;
      result.samples.push(await conversationSample(options, runId, sample));
      await persist();
      continue;
    }
    const ids = Array.from({ length: options.concurrency }, (_, index) => `bench-${runId}-s${sample + 1}-${index + 1}`);
    const started = performance.now();
    const settled = await Promise.allSettled(ids.map((id, index) => request(options, id, index + sample, options.workload)));
    const wallMs = performance.now() - started;
    const history = await jobsById(options, ids);
    const requests = settled.map((outcome, index) => attachMetrics(options, outcome.status === 'fulfilled' ? outcome.value : { id: ids[index], status: 'unexpected_error', httpStatus: null, clientLatencyMs: null, responseHeadersMs: null, responseUsage: null, error: String(outcome.reason) }, history, options.workload));
    result.samples.push(sampleSummary({ sample: sample + 1, wallMs, requests, historyErrors: history.errors, missingHistoryIds: history.missingIds }, options.workload));
    await persist();
  }
  result.finishedAt = new Date().toISOString();
  await persist();
  return result;
}

const help = `Usage: node Testing/measure-concurrency.mjs <baseUrl> <model> <1|2|3|4|8> <pp|tg|cache> [samples=3] [label=run] [options]

Frozen default workload: PP uses 4096 repeated words and max_tokens=4;
TG requests 192 benchmark words with max_tokens=256. One PP warmup is recorded
separately. Sample and prompt-index semantics match build/perf/measure-concurrency.mjs.
CACHE uses 3 sequential turns per parallel lane: the long PP prompt followed by
2 short requests, carrying actual prior assistant replies. No automatic retry.
Each sample starts fresh lanes. Expected turns = samples * lanes * 3, excluding
warmup. Per-turn telemetry and complete lane latency are retained on failures.
Cache hits/misses are observations, with no assumed gain or tiny-output TG floor.
Ctrl+C cancels CACHE requests and writes the collected evidence before exiting.

--request-timeout-ms N          Whole HTTP response deadline (default 300000)
--history-timeout-ms N          Total history lookup budget per batch (default 10000)
--history-request-timeout-ms N  Individual history request deadline (default 2000)
--history-poll-ms N             History poll interval (default 250)
--prompt-words N                PP/cache words, 1..96000 (default 4096; not tokens)
--generation-prompt-words N     Add 1..96000 context words to TG (default 0; separately versioned workload)
--min-tg N                      Each TG request must reach this rate (default 20)
--output FILE                  Persist warmup and each completed sample as JSON
--allow-live                   Explicitly allow port 11434; obtain authorization first

Authentication, if required: INFERDECK_API_KEY and INFERDECK_DASHBOARD_KEY.
Credentials are not recorded as run settings. Default output is build/perf/<run>.json.
Unknown metrics remain null. Missing history, failed responses, timeouts and TG
floor breaches fail the run. PP's four-token output is not a TG responsiveness test.
The harness deadline is not evidence of an n8n or other client's timeout policy.

Example (isolated gateway only):
node Testing/measure-concurrency.mjs http://127.0.0.1:11435 qwen3.8-27b 4 tg 3 baseline --output build/perf/baseline-c4-tg.json
node Testing/measure-concurrency.mjs http://127.0.0.1:11435 qwen3.8-27b 4 cache 3 cache-baseline --output build/perf/baseline-c4-cache.json
`;

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  if (process.argv.slice(2).includes('--help')) process.stdout.write(help);
  else {
    let interrupt;
    try {
      const options = parseArguments(process.argv.slice(2));
      if (options.workload === 'cache') {
        const controller = new AbortController();
        options.signal = controller.signal;
        interrupt = () => controller.abort();
        process.once('SIGINT', interrupt);
      }
      const result = await runBenchmark(options);
      process.stdout.write(JSON.stringify(result, null, 2) + '\n');
      process.exitCode = result.summary.passes ? 0 : 1;
    } catch (error) { process.stderr.write(`Benchmark failed: ${error.message}\n`); process.exitCode = 1; }
    finally { if (interrupt) process.removeListener('SIGINT', interrupt); }
  }
}
