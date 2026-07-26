import assert from "node:assert/strict";

import {
  assertErrorResponse,
  createDeadline,
  formatFailure,
  readJsonResponse,
  readSseResponse,
} from "./harness-utils.mjs";

const BASE = process.env.GATEWAY_URL ?? "http://127.0.0.1:11434";
const MODEL = process.env.GATEWAY_MODEL ?? "qwen2.5-coder-3b";

const hugeText = "alpha bravo charlie delta echo foxtrot golf hotel ".repeat(5000);
const body = (stream) => ({
  model: MODEL,
  stream,
  max_tokens: 16,
  messages: [
    { role: "system", content: "You are a helpful assistant." },
    { role: "user", content: hugeText + "\nSummarize the above." },
  ],
});

async function checkNonStreaming() {
  const deadline = createDeadline("non-stream overflow");
  try {
    const response = await fetch(`${BASE}/v1/chat/completions`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body(false)),
      signal: deadline.signal,
    });
    const payload = await readJsonResponse(response, "non-stream overflow");
    assert.equal(response.status, 400, `non-stream overflow: expected HTTP 400, got ${response.status}`);
    assertErrorResponse(payload, "context_length_exceeded", "non-stream overflow");
  } finally {
    deadline.clear();
  }
}

async function checkStreaming() {
  const deadline = createDeadline("stream overflow");
  try {
    const response = await fetch(`${BASE}/v1/chat/completions`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body(true)),
      signal: deadline.signal,
    });
    assert.equal(response.status, 200, `stream overflow: expected HTTP 200, got ${response.status}`);
    const events = await readSseResponse(response, "stream overflow");
    assert.equal(events.length, 1, "stream overflow: expected exactly one error event");
    assertErrorResponse(events[0], "context_length_exceeded", "stream overflow");
  } finally {
    deadline.clear();
  }
}

try {
  await checkNonStreaming();
  await checkStreaming();
  console.log("OVERFLOW PASS");
} catch (error) {
  console.error(`OVERFLOW FAIL: ${formatFailure(error)}`);
  process.exitCode = 1;
}
