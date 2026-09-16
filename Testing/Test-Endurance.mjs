import { mkdir, writeFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { randomUUID } from 'node:crypto';
import { performance } from 'node:perf_hooks';
import { parseArguments, requestBody, runBenchmark } from './measure-concurrency.mjs';

const [baseUrl, model, secondsText = '3600', directory = `build/perf/endurance-${randomUUID()}`] = process.argv.slice(2);
const seconds = Number(secondsText);
if (!baseUrl || !model || !Number.isInteger(seconds) || seconds < 60 || seconds > 86400) {
  throw new Error('Usage: node Testing/Test-Endurance.mjs <isolated-base-url> <model> [seconds=3600, min60] [new-output-directory]');
}
parseArguments([baseUrl, model, "4", "tg"]);
const output = resolve(directory);
await mkdir(output, { recursive: false });
const started = performance.now();
const report = { startedAt: new Date().toISOString(), model, requestedSeconds: seconds, rounds: [], streams: [], telemetry: [], finishedAt: null };
let stopping = false;
const auth = process.env.INFERDECK_DASHBOARD_KEY ? { Authorization: `Bearer ${process.env.INFERDECK_DASHBOARD_KEY}` } : {};
const monitor = (async () => {
  while (!stopping) {
    const at = new Date().toISOString();
    try {
      const response = await fetch(`${baseUrl}/api/inferdeck/v1/status`, { headers: auth, signal: AbortSignal.timeout(3000) });
      if (!response.ok) throw new Error(`status HTTP ${response.status}`);
      const state = await response.json();
      report.telemetry.push({ at, queue: state.queue, gpu: state.gpu, hardware: state.hardware });
    } catch (error) { report.telemetry.push({ at, error: error.message }); }
    for (let i = 0; i < 10 && !stopping; i++) await new Promise(resolve => setTimeout(resolve, 500));
  }
})();
async function streamProbe(index) {
  const begin = performance.now();
  const headers = { 'Content-Type': 'application/json', 'X-Request-ID': `endurance-stream-${randomUUID()}` };
  if (process.env.INFERDECK_API_KEY) headers.Authorization = `Bearer ${process.env.INFERDECK_API_KEY}`;
  const body = { ...requestBody(model, headers['X-Request-ID'], index, 'tg'), stream: true, reasoning_effort: 'none' };
  const response = await fetch(`${baseUrl}/v1/chat/completions`, { method: 'POST', headers, body: JSON.stringify(body), signal: AbortSignal.timeout(300000) });
  if (!response.ok || !response.body) throw new Error(`stream HTTP ${response.status}`);
  const decoder = new TextDecoder();
  let pending = '', done = false, contentEvents = 0, firstContentMs = null, previous = null, maxContentGapMs = 0;
  for await (const bytes of response.body) {
    pending += decoder.decode(bytes, { stream: true });
    let newline;
    while ((newline = pending.indexOf('\n')) >= 0) {
      const line = pending.slice(0, newline).trim();
      pending = pending.slice(newline + 1);
      if (!line.startsWith('data:')) continue;
      const data = line.slice(5).trim();
      if (data === '[DONE]') { done = true; continue; }
      const chunk = JSON.parse(data);
      if (chunk.error) throw new Error('stream returned an error');
      const delta = chunk.choices?.[0]?.delta;
      if (delta?.content || delta?.reasoning_content) {
        const now = performance.now();
        if (firstContentMs === null) firstContentMs = now - begin;
        if (previous !== null) maxContentGapMs = Math.max(maxContentGapMs, now - previous);
        previous = now;
        contentEvents++;
      }
    }
  }
  if (!done || contentEvents === 0) throw new Error(`stream incomplete: DONE=${done}, contentEvents=${contentEvents}`);
  return { firstContentMs, maxContentGapMs, contentEvents, elapsedMs: performance.now() - begin, done };
}
const cases = [{ c: 4, w: 'tg', words: 4096 }, { c: 4, w: 'pp', words: 4096 }, { c: 4, w: 'cache', words: 8192 }, { c: 8, w: 'tg', words: 4096 }];
try {
  do {
    const index = report.rounds.length;
    const scenario = cases[index % cases.length];
    const options = parseArguments([baseUrl, model, String(scenario.c), scenario.w, '1', `endurance-${index}`, '--prompt-words', String(scenario.words), '--min-tg', scenario.c === 4 ? '20' : '0', '--output', `${output}/round-${index}.json`]);
    const result = await runBenchmark(options);
    report.rounds.push({ index, concurrency: scenario.c, workload: scenario.w, summary: result.summary, durationSeconds: (performance.now() - started) / 1000 });
    await writeFile(`${output}/summary.json`, JSON.stringify(report, null, 2));
    process.stdout.write(JSON.stringify(report.rounds.at(-1)) + '\n');
    if (result.summary.failures > 0) throw new Error(`Request/telemetry failure in round ${index}`);
    if (index % cases.length === 0) {
      const probes = await Promise.allSettled([0, 1, 2, 3].map(streamProbe));
      report.streams.push({ round: index, probes: probes.map(probe => probe.status === 'fulfilled' ? probe.value : { error: probe.reason.message }) });
      if (probes.some(probe => probe.status === 'rejected')) throw new Error('streaming probe failed');
    }
  } while (performance.now() - started < seconds * 1000);
} catch (error) {
  report.error = error.message;
  throw error;
} finally {
  stopping = true;
  await monitor;
  report.elapsedSeconds = (performance.now() - started) / 1000;
  report.finishedAt = new Date().toISOString();
  report.passes = !report.error && report.elapsedSeconds >= seconds && report.rounds.length > 0 && report.rounds.every(round => round.summary.passes) && !report.telemetry.some(sample => sample.error);
  await writeFile(`${output}/summary.json`, JSON.stringify(report, null, 2));
}
if (!report.passes) process.exitCode = 1;
