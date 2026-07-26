import assert from "node:assert/strict";

import {
  assertCompletionResponse,
  createDeadline,
  formatFailure,
  readJsonResponse,
} from "./harness-utils.mjs";

const BASE = process.env.GATEWAY_URL ?? "http://127.0.0.1:11434";
const MODEL = process.env.GATEWAY_MODEL ?? "qwen3.6-35b-a3b";

const filler = "The quick brown fox jumps over the lazy dog. ".repeat(400);
const base = [
  { role: "system", content: "You are concise." },
  { role: "user", content: filler + "\nSay OK." },
];

async function ask(messages, label) {
  const t0 = Date.now();
  const deadline = createDeadline(label);
  try {
    const res = await fetch(`${BASE}/v1/chat/completions`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        model: MODEL,
        stream: false,
        max_tokens: 32,
        chat_template_kwargs: { enable_thinking: false },
        messages,
      }),
      signal: deadline.signal,
    });
    const body = await readJsonResponse(res, label);
    assert.equal(res.status, 200, `${label}: HTTP ${res.status} ${JSON.stringify(body).slice(0, 300)}`);
    const choice = assertCompletionResponse(body, label);
    const content = choice.message.content ?? "";
    assert.equal(typeof content, "string", `${label}: content must be a string`);
    assert(content.length > 0, `${label}: content must not be empty`);
    const cached = body.usage.prompt_tokens_details?.cached_tokens;
    assert(Number.isInteger(cached) && cached >= 0, `${label}: invalid cached_tokens`);
    console.log(
      `${label}: status=${res.status} prompt=${body.usage.prompt_tokens} cached=${cached} wall=${Date.now() - t0}ms`,
    );
    return { content, usage: body.usage };
  } finally {
    deadline.clear();
  }
}

try {
  const first = await ask(base, "turn 1");
  const followup = [
    ...base,
    { role: "assistant", content: first.content },
    { role: "user", content: "Now say DONE." },
  ];
  const second = await ask(followup, "turn 2");
  assert(
    second.usage.prompt_tokens > first.usage.prompt_tokens,
    "turn 2: prompt token count must include the added conversation",
  );
  assert(
    second.usage.prompt_tokens_details.cached_tokens > 0,
    "turn 2: expected a reused prompt prefix",
  );
  console.log("CACHE-REUSE PASS");
} catch (error) {
  console.error(`CACHE-REUSE FAIL: ${formatFailure(error)}`);
  process.exitCode = 1;
}
