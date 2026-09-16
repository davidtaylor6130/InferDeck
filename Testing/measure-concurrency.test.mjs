import test from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { mkdtemp, readFile, rm, rmdir } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArguments, requestBody } from './measure-concurrency.mjs';

const script = fileURLToPath(new URL('./measure-concurrency.mjs', import.meta.url));
const pause = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
const job = (id, overrides = {}) => ({ id, httpStatus: 200, errorCode: '', finishCode: 'stop', promptTokens: 4096, cacheWriteTokens: 3072, cachedPromptTokens: 1024, completionTokens: 192, durationMs: 8500, generationDurationMs: 8000, promptDurationMs: 300, queueDurationMs: 100, swapLoadDurationMs: 100, firstTokenDurationMs: 700, tokensPerSecond: 24, promptTokensPerSecond: 10240, ...overrides });
const json = (response, value, status = 200) => { response.writeHead(status, { 'Content-Type': 'application/json' }); response.end(JSON.stringify(value)); };
const completion = response => json(response, { choices: [{ message: { role: 'assistant', content: 'benchmark' }, finish_reason: 'stop' }], usage: { prompt_tokens: 4096, completion_tokens: 192 } });

async function fixture(t, handler) {
  const directory = await mkdtemp(join(tmpdir(), 'inferdeck-concurrency-test-'));
  const state = { calls: [], jobs: [], polls: 0 };
  const server = createServer(async (request, response) => {
    try {
      if (request.method === 'POST') {
        let text = ''; for await (const chunk of request) text += chunk;
        const call = { id: request.headers['x-request-id'], body: JSON.parse(text) };
        state.calls.push(call);
        if (call.id.endsWith('-warmup')) { state.jobs.push(job(call.id)); completion(response); return; }
        await handler({ kind: 'request', request, response, call, state });
      } else { state.polls++; await handler({ kind: 'history', request, response, state }); }
    } catch (error) { if (!response.headersSent) json(response, { error: error.message }, 500); else response.destroy(); }
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); await rm(join(directory, 'result.json'), { force: true }); await rmdir(directory); });
  return { directory, state, baseUrl: `http://127.0.0.1:${server.address().port}` };
}

async function execute(fixture, extra = [], { concurrency = 4, samples = 1, workload = 'tg', interrupt = false } = {}) {
  const output = join(fixture.directory, 'result.json');
  const env = { ...process.env }; delete env.INFERDECK_API_KEY; delete env.INFERDECK_DASHBOARD_KEY;
  const preload = interrupt ? ['--import', 'data:text/javascript,' + encodeURIComponent("process.once('message', () => { process.emit('SIGINT'); process.disconnect(); });")] : [];
  const child = spawn(process.execPath, [...preload, script, fixture.baseUrl, 'mock-model', String(concurrency), workload, String(samples), 'test', '--output', output, '--request-timeout-ms', '1000', '--history-timeout-ms', '1000', '--history-request-timeout-ms', '200', '--history-poll-ms', '5', ...extra], { windowsHide: true, env, stdio: interrupt ? ['ignore', 'pipe', 'pipe', 'ipc'] : ['ignore', 'pipe', 'pipe'] });
  if (interrupt) fixture.state.interrupt = () => child.send('interrupt');
  let stdout = ''; let stderr = '';
  child.stdout.on('data', data => { stdout += data; }); child.stderr.on('data', data => { stderr += data; });
  const timer = setTimeout(() => child.kill(), 10000);
  const code = await new Promise((resolve, reject) => { child.once('error', reject); child.once('close', resolve); });
  clearTimeout(timer);
  assert.equal(stderr, '', `CLI stderr: ${stderr}`);
  const result = JSON.parse(await readFile(output, 'utf8'));
  assert.deepEqual(JSON.parse(stdout), result, 'Persisted and printed evidence disagree');
  return { code, result };
}

test('rejects implicit live port and invalid options before any requests', () => {
  assert.throws(() => parseArguments(['http://127.0.0.1:11434', 'model', '4', 'tg']), /11434/);
  assert.equal(parseArguments(['http://127.0.0.1:11434', 'model', '4', 'tg', '--allow-live']).allowLive, true);
  for (const concurrency of [1, 2, 3, 4, 8]) assert.equal(parseArguments(['http://127.0.0.1:11435', 'model', String(concurrency), 'pp']).concurrency, concurrency);
  assert.throws(() => parseArguments(['http://user:secret@127.0.0.1:11435', 'model', '4', 'tg']), /credentials/);
  assert.throws(() => parseArguments(['http://127.0.0.1:11435', 'model', '4x', 'tg']), /Invalid/);
  assert.throws(() => parseArguments(['http://127.0.0.1:11435', 'model', '4', 'tg', '--request-timeout-ms', 'NaN']), /positive integer/);
});

test('persists all 8 concurrent requests across samples and out-of-order partial history', async t => {
  const f = await fixture(t, async ({ kind, response, call, state }) => {
    if (kind === 'request') {
      const slot = Number(call.id.split('-').at(-1)); await pause((9 - slot) * 3);
      state.jobs.push(job(call.id, { tokensPerSecond: 20 + slot, queueDurationMs: slot, firstTokenDurationMs: 100 + slot, swapLoadDurationMs: slot * 10 })); completion(response);
    } else {
      const row = state.jobs[(state.polls - 1) % state.jobs.length];
      json(response, { jobs: [job('unrelated-request'), row] });
    }
  });
  const { code, result } = await execute(f, [], { concurrency: 8, samples: 2 });
  assert.equal(code, 0); assert.equal(result.summary.passes, true);
  assert.equal(result.summary.recordedRequests, 16); assert.equal(result.samples.length, 2);
  assert.equal(result.warmup.passes, true); assert.equal(result.warmup.tgFloorApplicable, false);
  const ids = new Set([result.warmup.id, ...result.samples.flatMap(sample => sample.requests.map(row => row.id))]);
  assert.equal(ids.size, 17); assert.ok([...ids].every(id => id.length <= 64));
  for (const sample of result.samples) {
    assert.deepEqual(sample.requests.map(row => row.tokensPerSecond), [21, 22, 23, 24, 25, 26, 27, 28]);
    assert.deepEqual(sample.requests.map(row => row.swapLoadDurationMs), [10, 20, 30, 40, 50, 60, 70, 80]);
    assert.deepEqual(sample.requests.map(row => row.firstTokenDurationMs), [101, 102, 103, 104, 105, 106, 107, 108]);
    assert.ok(sample.requests.every(row => row.clientLatencyMs > 0 && row.historyFound));
  }
  assert.equal(f.state.calls[0].body.max_tokens, 4);
  assert.equal(f.state.calls[0].body.messages[0].content, 'Answer directly. <|think_off|>');
  for (let index = 0; index < 16; index++) {
    const body = f.state.calls[index + 1].body;
    assert.equal(body.stream, false); assert.equal(body.max_tokens, 256); assert.equal(body.temperature, 0); assert.equal(body.top_p, 1);
    assert.equal(body.seed, 7300 + index % 8 + Math.floor(index / 8));
    assert.ok(body.messages[1].content.endsWith('Output the lowercase word benchmark exactly 192 times, separated by single spaces. Do not add punctuation or other text.'));
  }
});

test('retains HTTP errors, body timeouts and slow requests without losing successful peers', async t => {
  const f = await fixture(t, async ({ kind, response, call, state }) => {
    if (kind === 'history') { json(response, { jobs: [...state.jobs].reverse() }); return; }
    const slot = Number(call.id.split('-').at(-1));
    if (slot === 1) { state.jobs.push(job(call.id, { httpStatus: 503, errorCode: 'overloaded' })); json(response, { error: 'Busy' }, 503); }
    if (slot === 2) { response.writeHead(200, { 'Content-Type': 'application/json' }); response.write('{'); }
    if (slot === 3 || slot === 4) { state.jobs.push(job(call.id, { tokensPerSecond: slot === 3 ? 19 : 40 })); completion(response); }
  });
  const { code, result } = await execute(f, ['--request-timeout-ms', '100', '--history-timeout-ms', '100']);
  assert.equal(code, 1); assert.equal(result.summary.recordedRequests, 4); assert.equal(result.summary.failures, 3); assert.equal(result.summary.belowTgFloor, 1);
  const [failed, timedOut, slow, good] = result.samples[0].requests;
  assert.equal(failed.httpStatus, 503); assert.equal(failed.historyHttpStatus, 503); assert.equal(failed.status, 'http_error');
  assert.equal(timedOut.httpStatus, 200); assert.equal(timedOut.status, 'timeout'); assert.match(timedOut.error, /Harness request deadline exceeded/);
  assert.equal(timedOut.firstTokenDurationMs, null); assert.equal(timedOut.promptDurationMs, null); assert.equal(timedOut.historyFound, false);
  assert.deepEqual(result.samples[0].missingHistoryIds, [timedOut.id]);
  assert.equal(slow.tokensPerSecond, 19); assert.ok(slow.failures.includes('below_tg_floor')); assert.equal(good.passes, true);
  assert.equal(result.samples[0].aggregateTokensPerSecond, null, 'Missing history must not look like a zero-token success');
});

test('bounds an unresponsive history endpoint and records missing rows as failures', async t => {
  const f = await fixture(t, async ({ kind, response, call, state }) => {
    if (kind === 'request') { state.jobs.push(job(call.id)); completion(response); }
    else if (state.calls.length === 1) json(response, { jobs: state.jobs });
  });
  const started = Date.now();
  const { code, result } = await execute(f, ['--history-timeout-ms', '120', '--history-request-timeout-ms', '30'], { concurrency: 2 });
  assert.equal(code, 1); assert.ok(Date.now() - started < 4000);
  assert.equal(result.summary.recordedRequests, 2); assert.equal(result.samples[0].missingHistoryIds.length, 2);
  assert.ok(result.samples[0].historyErrors.length > 0);
  assert.ok(result.samples[0].requests.every(row => row.status === 'success' && row.failures.includes('missing_history') && row.tokensPerSecond === null));
});

test('preserves the PP workload and keeps four-token output outside the TG floor gate', async t => {
  const f = await fixture(t, async ({ kind, response, call, state }) => {
    if (kind === 'request') { state.jobs.push(job(call.id, { tokensPerSecond: 5, completionTokens: 4 })); completion(response); }
    else json(response, { jobs: state.jobs });
  });
  const { code, result } = await execute(f, [], { concurrency: 1, workload: 'pp' });
  assert.equal(code, 0); assert.equal(result.samples[0].requests[0].tgFloorApplicable, false);
  const body = f.state.calls[1].body;
  const prefix = `Nonce ${f.state.calls[1].id}. Read every word before answering. `;
  const suffix = ' Reply with only OK.';
  assert.equal(body.max_tokens, 4); assert.equal(body.seed, 7300);
  const text = body.messages[1].content;
  assert.ok(text.startsWith(prefix)); assert.ok(text.endsWith(suffix));
  const words = text.slice(prefix.length, -suffix.length).split(' ');
  assert.equal(words.length, 4096); assert.deepEqual(words.slice(0, 8), ['amber', 'birch', 'cobalt', 'delta', 'ember', 'fjord', 'granite', 'harbour']);
});


test('incomplete successful telemetry cannot pass a baseline', async t => {
  const f = await fixture(t, async ({ kind, response, call, state }) => {
    if (kind === 'request') { const row = job(call.id); delete row.swapLoadDurationMs; state.jobs.push(row); completion(response); }
    else json(response, { jobs: state.jobs });
  });
  const { code, result } = await execute(f, [], { concurrency: 1 });
  assert.equal(code, 1);
  const row = result.samples[0].requests[0];
  assert.equal(row.status, 'success'); assert.equal(row.swapLoadDurationMs, null);
  assert.deepEqual(row.missingMetrics, ['swapLoadDurationMs']); assert.ok(row.failures.includes('missing_metrics'));
});


test('cache lanes carry actual assistant replies independently and retain all turn telemetry', async t => {
  const f = await fixture(t, async ({ kind, response, call, state }) => {
    if (kind === 'history') { json(response, { jobs: [...state.jobs].reverse() }); return; }
    const [, laneText, turnText] = call.id.match(/-(\d+)-t(\d+)$/);
    const lane = Number(laneText); const turn = Number(turnText);
    if (lane !== 1 && turn === 2 && !state.slowFirstFinished) state.advancedIndependently = true;
    if (lane === 1 && turn === 1) { await pause(40); state.slowFirstFinished = true; }
    const missed = turn === 1 || (lane === 3 && turn === 3);
    state.jobs.push(job(call.id, { tokensPerSecond: 5, cachedPromptTokens: missed ? 0 : 4096, cacheWriteTokens: missed ? 4112 : 16, promptTokens: 4112, promptDurationMs: turn === 1 ? 100 : turn, completionTokens: 4 }));
    json(response, { choices: [{ message: { role: 'assistant', content: `actual reply from lane ${lane}, turn ${turn}`, reasoning_content: `reason-${lane}-${turn}` }, finish_reason: 'stop' }], usage: { completion_tokens: 4 } });
  });
  const { code, result } = await execute(f, [], { concurrency: 3, workload: 'cache' });
  assert.equal(code, 0); assert.equal(result.summary.passes, true); assert.equal(result.turnsPerLane, 3);
  assert.equal(result.expectedLanes, 3); assert.equal(result.expectedRequests, 9); assert.equal(result.summary.recordedTurns, 9);
  assert.equal(result.summary.expectedCacheTurns, 6); assert.equal(result.summary.observedCacheTurns, 5); assert.equal(result.summary.missedCacheTurns, 1);
  assert.equal(f.state.advancedIndependently, true, 'Fast lanes must continue without a barrier on another lane');
  for (const lane of result.samples[0].lanes) {
    const calls = f.state.calls.filter(call => call.id.startsWith(lane.id + '-t'));
    assert.equal(calls.length, 3); assert.equal(lane.recordedTurns, 3); assert.equal(lane.missingTurns, 0); assert.equal(lane.passes, true);
    assert.ok(lane.clientLatencyMs >= lane.requests.reduce((total, row) => total + row.clientLatencyMs, 0));
    for (let turn = 1; turn <= 3; turn++) {
      const body = calls[turn - 1].body;
      assert.equal(body.messages.length, turn * 2); assert.equal(body.max_tokens, 4); assert.equal(body.stream, false);
      assert.deepEqual(body.messages.slice(0, 2), calls[0].body.messages);
      assert.equal(lane.requests[turn - 1].turn, turn);
      assert.equal(lane.requests[turn - 1].cacheExpected, turn > 1);
      assert.equal(lane.requests[turn - 1].tgFloorApplicable, false);
      assert.equal(lane.requests[turn - 1].promptDurationMs, turn === 1 ? 100 : turn);
      if (turn > 1) {
        assert.deepEqual(body.messages[2 * turn - 2], lane.requests[turn - 2].assistantMessage);
        assert.deepEqual(body.messages[2 * turn - 1], { role: 'user', content: `Turn ${turn}. Reply with only OK.` });
      }
    }
  }
});

test('a failed cache lane stops while its peer completes, without fabricated or retried turns', async t => {
  const f = await fixture(t, async ({ kind, response, call, state }) => {
    if (kind === 'history') { json(response, { jobs: state.jobs }); return; }
    const failed = /-1-t2$/.test(call.id);
    state.jobs.push(job(call.id, failed ? { httpStatus: 503, errorCode: 'overloaded' } : { cachedPromptTokens: 2048 }));
    if (failed) json(response, { error: 'Busy' }, 503); else completion(response);
  });
  const { code, result } = await execute(f, [], { concurrency: 2, workload: 'cache' });
  assert.equal(code, 1); assert.equal(result.summary.expectedTurns, 6); assert.equal(result.summary.recordedTurns, 5); assert.equal(result.summary.missingTurns, 1);
  const [failed, complete] = result.samples[0].lanes;
  assert.equal(failed.status, 'http_error'); assert.equal(failed.recordedTurns, 2); assert.equal(failed.missingTurns, 1); assert.equal(failed.passes, false); assert.ok(failed.clientLatencyMs > 0);
  assert.equal(complete.status, 'completed'); assert.equal(complete.recordedTurns, 3); assert.equal(complete.passes, true);
  assert.equal(failed.requests[1].httpStatus, 503); assert.equal(failed.requests[1].errorCode, 'overloaded');
  assert.equal(f.state.calls.filter(call => call.id === failed.id + '-t2').length, 1);
  assert.equal(f.state.calls.filter(call => call.id === failed.id + '-t3').length, 0);
});

test('cache cancellation writes partial lane evidence and exits without sending another turn', async t => {
  const f = await fixture(t, async ({ kind, response, call, state }) => {
    if (kind === 'history') { json(response, { jobs: state.jobs }); return; }
    if (/-t2$/.test(call.id)) {
      state.jobs.push(job(call.id, { httpStatus: 499, errorCode: 'cancelled' }));
      response.writeHead(200, { 'Content-Type': 'application/json' }); response.write('{');
      state.interrupt();
    } else { state.jobs.push(job(call.id)); completion(response); }
  });
  const { code, result } = await execute(f, [], { concurrency: 1, workload: 'cache', interrupt: true });
  assert.equal(code, 1); assert.equal(result.summary.recordedTurns, 2); assert.equal(result.summary.missingTurns, 1);
  const lane = result.samples[0].lanes[0];
  assert.equal(lane.status, 'cancelled'); assert.equal(lane.requests[1].status, 'cancelled'); assert.match(lane.requests[1].error, /cancelled/);
  assert.equal(lane.requests[1].historyHttpStatus, 499); assert.ok(lane.clientLatencyMs > 0);
  assert.equal(f.state.calls.filter(call => /-t3$/.test(call.id)).length, 0);
});


test('a cache lane cannot invent a prior assistant message from an invalid response', async t => {
  const f = await fixture(t, async ({ kind, response, call, state }) => {
    if (kind === 'history') { json(response, { jobs: state.jobs }); return; }
    state.jobs.push(job(call.id));
    json(response, { choices: [{ message: { role: 'assistant', content: null }, finish_reason: 'stop' }] });
  });
  const { code, result } = await execute(f, [], { concurrency: 1, workload: 'cache' });
  assert.equal(code, 1);
  const lane = result.samples[0].lanes[0];
  assert.equal(lane.recordedTurns, 1); assert.equal(lane.missingTurns, 2); assert.equal(lane.status, 'invalid_response');
  assert.match(lane.error, /assistant text response/); assert.equal(f.state.calls.length, 2);
});


test('configurable long prompts retain the frozen default and propagate to cache lanes', async t => {
  const args = ['http://127.0.0.1:11435', 'model', '4', 'cache'];
  assert.equal(parseArguments(args).promptWords, 4096);
  for (const invalid of ['0', '-1', '96001', '1.5', 'NaN']) {
    assert.throws(() => parseArguments([...args, '--prompt-words', invalid]), /prompt-words/);
  }
  assert.deepEqual(requestBody('model', 'id', 0, 'pp'), requestBody('model', 'id', 0, 'pp', 4096));
  const f = await fixture(t, async ({ kind, response, call, state }) => {
    if (kind === 'request') { state.jobs.push(job(call.id)); completion(response); }
    else json(response, { jobs: state.jobs });
  });
  const { code, result } = await execute(f, ['--prompt-words', '16384'], { workload: 'cache', concurrency: 1, samples: 1 });
  assert.equal(code, 0);
  assert.equal(result.promptWords, 16384);
  assert.equal(f.state.calls.length, 4);
  for (const call of f.state.calls) {
    const content = call.body.messages[1].content;
    const words = content.split('Read every word before answering. ')[1].split(' Reply with only OK.')[0].split(' ');
    assert.equal(words.length, 16384);
  }
});


test('long-context generation sends the requested context without changing frozen TG', async t => {
  const normal = requestBody('model', 'id', 0, 'tg');
  assert(normal.messages[1].content.length < 300);
  assert.throws(() => parseArguments(['http://127.0.0.1:11435', 'model', '4', 'pp', '--generation-prompt-words', '512']), /generation-prompt-words/);
  const f = await fixture(t, async ({ kind, response, call, state }) => {
    if (kind === 'request') {
      assert(call.body.messages[1].content.includes('amber birch cobalt'));
      assert(call.body.messages[1].content.length > 2500);
      assert.equal(call.body.max_tokens, 256);
      state.jobs.push(job(call.id)); completion(response);
    } else json(response, { jobs: state.jobs });
  });
  const {code, result} = await execute(f, ['--generation-prompt-words', '512'], {concurrency: 1});
  assert.equal(code, 0);
  assert.equal(result.workloadVersion, 'long-context-tg-v1');
  assert.equal(result.generationPromptWords, 512);
  assert.equal(result.summary.recordedRequests, 1);
});
