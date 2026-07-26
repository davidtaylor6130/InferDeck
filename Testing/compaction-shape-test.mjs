import assert from "node:assert/strict";

import {
  assertChatChunk,
  assertCompletionResponse,
  createDeadline,
  formatFailure,
  readJsonResponse,
  readSseResponse,
} from "./harness-utils.mjs";

const BASE = process.env.GATEWAY_URL ?? "http://127.0.0.1:11435";
const MODEL = process.env.GATEWAY_MODEL ?? "qwen2.5-coder-3b";

const messages = [
  { role: "system", content: "Summarize the conversation below." },
  { role: "user", content: "List the files in src/." },
  {
    role: "assistant",
    content: "",
    tool_calls: [{
      id: "call_1",
      type: "function",
      function: { name: "list_files", arguments: "{\"dir\":\"src/\"}" },
    }],
  },
  { role: "tool", tool_call_id: "call_1", content: "main.c util.c README.md" },
  { role: "user", content: "Now summarize everything above into a short paragraph." },
];

async function runNonStreaming() {
  const deadline = createDeadline("non-stream compaction");
  try {
    const response = await fetch(`${BASE}/v1/chat/completions`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ model: MODEL, stream: false, max_tokens: 128, messages }),
      signal: deadline.signal,
    });
    const body = await readJsonResponse(response, "non-stream compaction");
    assert.equal(response.status, 200, `non-stream compaction: HTTP ${response.status}`);
    const choice = assertCompletionResponse(body, "non-stream compaction");
    const output = `${choice.message.reasoning_content ?? ""}${choice.message.content ?? ""}`;
    assert(output.trim().length > 0, "non-stream compaction: empty assistant output");
    return output;
  } finally {
    deadline.clear();
  }
}

async function runStreaming() {
  const deadline = createDeadline("stream compaction");
  try {
    const response = await fetch(`${BASE}/v1/chat/completions`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ model: MODEL, stream: true, max_tokens: 128, messages }),
      signal: deadline.signal,
    });
    assert.equal(response.status, 200, `stream compaction: HTTP ${response.status}`);
    const events = await readSseResponse(response, "stream compaction");
    let output = "";
    let finishReason = null;
    for (const [index, event] of events.entries()) {
      assert(!event.error, `stream compaction: error event ${JSON.stringify(event.error)}`);
      const choice = assertChatChunk(event, `stream compaction event ${index}`);
      if (!choice) continue;
      output += choice.delta.reasoning_content ?? "";
      output += choice.delta.content ?? "";
      if (choice.finish_reason !== null) finishReason = choice.finish_reason;
    }
    assert(output.trim().length > 0, "stream compaction: empty assistant output");
    assert.equal(typeof finishReason, "string", "stream compaction: missing terminal finish_reason");
    return output;
  } finally {
    deadline.clear();
  }
}

try {
  const nonStreaming = await runNonStreaming();
  const streaming = await runStreaming();
  console.log(
    `COMPACTION-SHAPE PASS (non-stream=${nonStreaming.length} chars, stream=${streaming.length} chars)`,
  );
} catch (error) {
  console.error(`COMPACTION-SHAPE FAIL: ${formatFailure(error)}`);
  process.exitCode = 1;
}
