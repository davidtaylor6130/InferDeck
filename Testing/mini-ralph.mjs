const BASE = process.env.GATEWAY_URL ?? "http://127.0.0.1:11435";
const MODEL = process.env.GATEWAY_MODEL ?? "qwen2.5-coder-3b";
const ITERATIONS = Number(process.env.ITERATIONS ?? 5);

const tools = [
  {
    type: "function",
    function: {
      name: "read_file",
      description: "Read a file from the workspace and return its contents",
      parameters: {
        type: "object",
        properties: { path: { type: "string", description: "workspace-relative path" } },
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
  if (name === "list_files") return JSON.stringify(["src/main.c", "src/util.c", "README.md"]);
  if (name === "read_file") return "int main(void) { return 42; }\n";
  return `unknown tool ${name}`;
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
      "Find out what source files exist under src/ and then read each one. Use tools one step at a time.",
  },
];

async function streamOnce(iter) {
  const res = await fetch(`${BASE}/v1/chat/completions`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ model: MODEL, messages, tools, stream: true }),
  });
  if (!res.ok) throw new Error(`HTTP ${res.status}: ${await res.text()}`);

  let content = "";
  let reasoning = "";
  const toolCalls = [];
  let finishReason = null;
  let chunks = 0;
  let buf = "";
  const decoder = new TextDecoder();
  for await (const part of res.body) {
    buf += decoder.decode(part, { stream: true });
    let nl;
    while ((nl = buf.indexOf("\n\n")) >= 0) {
      const event = buf.slice(0, nl);
      buf = buf.slice(nl + 2);
      for (const line of event.split("\n")) {
        if (!line.startsWith("data: ")) continue;
        const payload = line.slice(6).trim();
        if (payload === "[DONE]") continue;
        const json = JSON.parse(payload);
        if (json.error) throw new Error(`stream error: ${JSON.stringify(json.error)}`);
        const choice = json.choices?.[0];
        if (!choice) continue;
        chunks++;
        const delta = choice.delta ?? {};
        if (delta.content) content += delta.content;
        if (delta.reasoning_content) reasoning += delta.reasoning_content;
        for (const tc of delta.tool_calls ?? []) {
          const idx = tc.index ?? 0;
          toolCalls[idx] ??= { id: "", name: "", arguments: "" };
          if (tc.id) toolCalls[idx].id = tc.id;
          if (tc.function?.name) toolCalls[idx].name = tc.function.name;
          if (tc.function?.arguments) toolCalls[idx].arguments += tc.function.arguments;
        }
        if (choice.finish_reason) finishReason = choice.finish_reason;
      }
    }
  }
  return { content, reasoning, toolCalls, finishReason, chunks };
}

let failures = 0;
for (let i = 1; i <= ITERATIONS; i++) {
  const t0 = Date.now();
  const r = await streamOnce(i);
  const secs = ((Date.now() - t0) / 1000).toFixed(1);
  const summary = r.toolCalls
    .map((tc) => `${tc.name}(${tc.arguments.slice(0, 80)})`)
    .join(", ");
  console.log(
    `iter ${i}: finish=${r.finishReason} chunks=${r.chunks} tool_calls=[${summary}] content=${JSON.stringify(r.content.slice(0, 120))} (${secs}s)`,
  );

  if (r.finishReason === "stop" && r.toolCalls.length === 0 && i > 2) {
    console.log(`iter ${i}: model finished the task (finish=stop), ending loop`);
    break;
  }
  if (r.finishReason !== "tool_calls" || r.toolCalls.length === 0) {
    console.log(`iter ${i}: FAIL - expected tool_calls finish, got ${r.finishReason}`);
    failures++;
    break;
  }
  if (r.finishReason === "tool_calls" && r.toolCalls.length === 0) {
    console.log(`iter ${i}: FAIL - finish=tool_calls but no streamed tool_call deltas`);
    failures++;
    break;
  }
  for (const tc of r.toolCalls) {
    try {
      JSON.parse(tc.arguments || "{}");
    } catch (e) {
      console.log(`iter ${i}: FAIL - tool args not valid JSON: ${tc.arguments}`);
      failures++;
    }
    if (!tc.name) {
      console.log(`iter ${i}: FAIL - tool call missing name`);
      failures++;
    }
  }
  if (failures) break;

  messages.push({
    role: "assistant",
    content: r.content || null,
    tool_calls: r.toolCalls.map((tc, idx) => ({
      id: tc.id || `call_${i}_${idx}`,
      type: "function",
      function: { name: tc.name, arguments: tc.arguments || "{}" },
    })),
  });
  for (const [idx, tc] of r.toolCalls.entries()) {
    messages.push({
      role: "tool",
      tool_call_id: tc.id || `call_${i}_${idx}`,
      content: fakeToolResult(tc.name, tc.arguments),
    });
  }
}

console.log(failures === 0 ? "MINI-RALPH PASS" : `MINI-RALPH FAIL (${failures} failures)`);
process.exit(failures === 0 ? 0 : 1);
