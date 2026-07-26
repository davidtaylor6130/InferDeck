import assert from "node:assert/strict";

import {
  assertCompletionResponse,
  createDeadline,
  formatFailure,
  readJsonResponse,
} from "./harness-utils.mjs";

const BASE = process.env.GATEWAY_URL ?? "http://127.0.0.1:11434";
const MODEL = process.env.GATEWAY_MODEL ?? "qwen3.6-35b-a3b";

const shapes = {
  assistant_null_content: [
    { role: "user", content: "hi" },
    { role: "assistant", content: null },
    { role: "user", content: "summarize" },
  ],
  assistant_missing_content: [
    { role: "user", content: "hi" },
    { role: "assistant" },
    { role: "user", content: "summarize" },
  ],
  content_parts_array: [
    { role: "user", content: [{ type: "text", text: "hi" }] },
    { role: "assistant", content: "hello" },
    { role: "user", content: [{ type: "text", text: "summarize" }] },
  ],
  assistant_parts_with_tool_calls: [
    { role: "user", content: "hi" },
    {
      role: "assistant",
      content: [{ type: "text", text: "calling" }],
      tool_calls: [{ id: "c1", type: "function", function: { name: "f", arguments: "{}" } }],
    },
    { role: "tool", tool_call_id: "c1", content: "ok" },
    { role: "user", content: "summarize" },
  ],
  tool_content_parts: [
    { role: "user", content: "hi" },
    {
      role: "assistant", content: "",
      tool_calls: [{ id: "c1", type: "function", function: { name: "f", arguments: "{}" } }],
    },
    { role: "tool", tool_call_id: "c1", content: [{ type: "text", text: "ok" }] },
    { role: "user", content: "summarize" },
  ],
  developer_role: [
    { role: "developer", content: "be brief" },
    { role: "user", content: "summarize" },
  ],
  tool_call_args_object: [
    { role: "user", content: "hi" },
    {
      role: "assistant", content: "",
      tool_calls: [{ id: "c1", type: "function", function: { name: "f", arguments: { dir: "src" } } }],
    },
    { role: "tool", tool_call_id: "c1", content: "ok" },
    { role: "user", content: "summarize" },
  ],
};

let failures = 0;
for (const [name, messages] of Object.entries(shapes)) {
  const deadline = createDeadline(name);
  try {
    const response = await fetch(`${BASE}/v1/chat/completions`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ model: MODEL, stream: false, max_tokens: 16, messages }),
      signal: deadline.signal,
    });
    const body = await readJsonResponse(response, name);
    assert.equal(response.status, 200, `${name}: HTTP ${response.status} ${JSON.stringify(body).slice(0, 220)}`);
    assertCompletionResponse(body, name);
    console.log(`${name}: PASS`);
  } catch (error) {
    failures++;
    console.error(`${name}: FAIL - ${formatFailure(error)}`);
  } finally {
    deadline.clear();
  }
}

if (failures > 0) {
  console.error(`SHAPE-SWEEP FAIL (${failures}/${Object.keys(shapes).length})`);
  process.exitCode = 1;
} else {
  console.log(`SHAPE-SWEEP PASS (${Object.keys(shapes).length} shapes)`);
}
