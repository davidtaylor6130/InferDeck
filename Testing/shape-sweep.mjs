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

for (const [name, messages] of Object.entries(shapes)) {
  const res = await fetch(`${BASE}/v1/chat/completions`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ model: MODEL, stream: false, max_tokens: 8, messages }),
  }).catch(e => ({ status: "fetch-error", text: async () => String(e) }));
  const text = (await res.text()).slice(0, 220);
  console.log(`${name}: ${res.status}${res.status >= 400 ? " | " + text : ""}`);
}
