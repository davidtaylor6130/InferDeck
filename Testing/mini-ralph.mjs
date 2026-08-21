import assert from "node:assert/strict";

import {
  assertChatChunk,
  boundedEnvInteger,
  createDeadline,
  formatFailure,
  readSseResponse,
} from "./harness-utils.mjs";

const BASE = process.env.GATEWAY_URL ?? "http://127.0.0.1:11435";
const MODEL = process.env.GATEWAY_MODEL ?? "qwen2.5-coder-3b";
const ITERATIONS = boundedEnvInteger("ITERATIONS", 5, 1, 20);
const MAX_TOKENS = boundedEnvInteger("MAX_TOKENS", 256, 16, 1024);
const REQUIRED_FILES = ["src/main.c", "src/util.c"];

const tools = [
  {
    type: "function",
    function: {
      name: "read_file",
      description: "Read a file from the workspace and return its contents",
      parameters: {
        type: "object",
        properties: {
          path: {
            type: "string",
            enum: REQUIRED_FILES,
            description: "workspace-relative path",
          },
        },
        required: ["path"],
      },
    },
  },
  {
    type: "function",
    function: {
      name: "list_files",
      description: "List files in a workspace directory",
      parameters: {
        type: "object",
        properties: { dir: { type: "string", description: "workspace-relative directory" } },
        required: ["dir"],
      },
    },
  },
];

const fakeToolResult = (name, args) => {
  if (name === "list_files") {
    const dir = args.dir.replaceAll("\\", "/").replace(/\/+$/, "");
    return JSON.stringify({
      files: dir === "src" ? REQUIRED_FILES : [],
      instruction: "Call read_file once for every listed file before answering.",
    });
  }
  if (name === "read_file") {
    const path = args.path.replaceAll("\\", "/").replace(/^\.\/+/, "");
    const content = path === "src/main.c"
      ? '#include "util.h"\nint main(void) { return answer(); }\n'
      : path === "src/util.c"
        ? "int answer(void) { return 42; }\n"
        : null;
    if (content !== null) {
      return JSON.stringify({
        path,
        content,
        remaining: REQUIRED_FILES.filter((file) => !taskState.readFiles.has(file)),
        instruction: "Read every remaining file before answering.",
      });
    }
    return JSON.stringify({ error: `file not found: ${path}` });
  }
  return JSON.stringify({ error: `unknown tool: ${name}` });
};

const messages = [
  {
    role: "system",
    content:
      "You are a coding agent. Use the provided tools to explore the workspace. Always call a tool when you need information; do not guess.",
  },
  {
    role: "user",
    content:
      "Find out what source files exist under src/ and then read each one. Use tools one step at a time. When finished, name every source file and state the value returned by main.",
  },
];

async function streamOnce(iter) {
  const label = `iteration ${iter}`;
  const deadline = createDeadline(label);
  try {
    const res = await fetch(`${BASE}/v1/chat/completions`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        model: MODEL,
        messages,
        tools,
        stream: true,
        max_tokens: MAX_TOKENS,
        temperature: 0,
        top_p: 1,
      }),
      signal: deadline.signal,
    });
    if (!res.ok) throw new Error(`${label}: HTTP ${res.status}: ${(await res.text()).slice(0, 500)}`);

    let content = "";
    let reasoning = "";
    const toolCalls = [];
    let finishReason = null;
    let chunks = 0;
    const events = await readSseResponse(res, label);
    for (const [eventIndex, event] of events.entries()) {
      if (event.error) throw new Error(`${label}: stream error: ${JSON.stringify(event.error)}`);
      const choice = assertChatChunk(event, `${label} event ${eventIndex}`);
      if (!choice) continue;
      chunks++;
      const delta = choice.delta;
      if (delta.content !== undefined) {
        assert.equal(typeof delta.content, "string", `${label}: content delta must be a string`);
        content += delta.content;
      }
      if (delta.reasoning_content !== undefined) {
        assert.equal(
          typeof delta.reasoning_content,
          "string",
          `${label}: reasoning_content delta must be a string`,
        );
        reasoning += delta.reasoning_content;
      }
      assert(
        delta.tool_calls === undefined || Array.isArray(delta.tool_calls),
        `${label}: tool_calls delta must be an array`,
      );
      for (const tc of delta.tool_calls ?? []) {
        const idx = tc.index ?? 0;
        assert(Number.isInteger(idx) && idx >= 0, `${label}: invalid tool call index`);
        toolCalls[idx] ??= { id: "", name: "", arguments: "" };
        if (tc.id !== undefined) {
          assert.equal(typeof tc.id, "string", `${label}: tool call id must be a string`);
          toolCalls[idx].id = tc.id;
        }
        if (tc.function?.name !== undefined) {
          assert.equal(typeof tc.function.name, "string", `${label}: tool name must be a string`);
          toolCalls[idx].name = tc.function.name;
        }
        if (tc.function?.arguments !== undefined) {
          assert.equal(
            typeof tc.function.arguments,
            "string",
            `${label}: tool arguments delta must be a string`,
          );
          toolCalls[idx].arguments += tc.function.arguments;
        }
      }
      if (choice.finish_reason !== null) {
        assert(
          finishReason === null || finishReason === choice.finish_reason,
          `${label}: conflicting finish reasons`,
        );
        finishReason = choice.finish_reason;
      }
    }
    assert(chunks > 0, `${label}: stream contained no choice chunks`);
    assert.equal(typeof finishReason, "string", `${label}: missing terminal finish_reason`);
    assert(toolCalls.every(Boolean), `${label}: sparse tool call indices`);
    return { content, reasoning, toolCalls, finishReason, chunks };
  } finally {
    deadline.clear();
  }
}

let failures = 0;
let completed = false;
const taskState = {
  listedSrc: false,
  readFiles: new Set(),
};

try {
  for (let i = 1; i <= ITERATIONS; i++) {
    const t0 = Date.now();
    const result = await streamOnce(i);
    const secs = ((Date.now() - t0) / 1000).toFixed(1);
    const summary = result.toolCalls
      .map((tc) => `${tc.name}(${tc.arguments.slice(0, 80)})`)
      .join(", ");
    console.log(
      `iter ${i}: finish=${result.finishReason} chunks=${result.chunks} tool_calls=[${summary}] content=${JSON.stringify(result.content.slice(0, 120))} (${secs}s)`,
    );

    if (result.finishReason === "stop") {
      assert.equal(result.toolCalls.length, 0, `iter ${i}: finish=stop included tool calls`);
      assert(taskState.listedSrc, `iter ${i}: stopped before listing src/`);
      assert(
        REQUIRED_FILES.every((path) => taskState.readFiles.has(path)),
        `iter ${i}: stopped before reading every source file`,
      );
      const evidence = result.content.toLowerCase();
      assert(evidence.trim().length > 0, `iter ${i}: terminal answer was empty`);
      for (const path of REQUIRED_FILES) {
        assert(evidence.includes(path.split("/").at(-1)), `iter ${i}: terminal answer omitted ${path}`);
      }
      assert(/\b42\b/.test(evidence), `iter ${i}: terminal answer omitted main's return value`);
      completed = true;
      break;
    }

    assert.equal(
      result.finishReason,
      "tool_calls",
      `iter ${i}: expected tool_calls or a verified terminal stop`,
    );
    assert(result.toolCalls.length > 0, `iter ${i}: finish=tool_calls without tool call deltas`);
    assert.equal(result.toolCalls.length, 1, `iter ${i}: expected one tool call at a time`);

    const parsedCalls = result.toolCalls.map((tc) => {
      assert(tc.id.length > 0, `iter ${i}: tool call missing id`);
      assert(tc.name.length > 0, `iter ${i}: tool call missing name`);
      let args;
      try {
        args = JSON.parse(tc.arguments);
      } catch (error) {
        throw new Error(`iter ${i}: tool args are not valid JSON: ${tc.arguments}`, {
          cause: error,
        });
      }
      assert(args && typeof args === "object" && !Array.isArray(args), `iter ${i}: tool args must be an object`);
      return { ...tc, args };
    });

    for (const tc of parsedCalls) {
      if (tc.name === "list_files") {
        assert.equal(typeof tc.args.dir, "string", `iter ${i}: list_files.dir must be a string`);
        const dir = tc.args.dir.replaceAll("\\", "/").replace(/\/+$/, "");
        assert.equal(dir, "src", `iter ${i}: expected list_files for src/`);
        taskState.listedSrc = true;
      } else if (tc.name === "read_file") {
        assert(taskState.listedSrc, `iter ${i}: read_file called before list_files`);
        assert.equal(typeof tc.args.path, "string", `iter ${i}: read_file.path must be a string`);
        const path = tc.args.path.replaceAll("\\", "/").replace(/^\.\/+/, "");
        assert(REQUIRED_FILES.includes(path), `iter ${i}: unexpected read_file path ${path}`);
        taskState.readFiles.add(path);
      } else {
        assert.fail(`iter ${i}: unexpected tool ${tc.name}`);
      }
    }

    messages.push({
      role: "assistant",
      content: result.content || null,
      tool_calls: parsedCalls.map((tc) => ({
        id: tc.id,
        type: "function",
        function: { name: tc.name, arguments: tc.arguments },
      })),
    });
    for (const tc of parsedCalls) {
      messages.push({
        role: "tool",
        tool_call_id: tc.id,
        content: fakeToolResult(tc.name, tc.args),
      });
    }
  }
  assert(completed, `model did not finish the verified task within ${ITERATIONS} iterations`);
} catch (error) {
  failures++;
  console.error(`MINI-RALPH ERROR: ${formatFailure(error)}`);
}

console.log(failures === 0 ? "MINI-RALPH PASS" : `MINI-RALPH FAIL (${failures} failures)`);
process.exitCode = failures === 0 ? 0 : 1;
