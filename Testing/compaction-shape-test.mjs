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

const res = await fetch(`${BASE}/v1/chat/completions`, {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ model: MODEL, stream: false, max_tokens: 128, messages }),
});
console.log("no-tools compaction shape status:", res.status);
console.log("body:", (await res.text()).slice(0, 500));

const res2 = await fetch(`${BASE}/v1/chat/completions`, {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify({ model: MODEL, stream: true, max_tokens: 128, messages }),
});
console.log("stream status:", res2.status);
const reader = res2.body.getReader();
const decoder = new TextDecoder();
let acc = "";
while (acc.length < 1200) {
  const { value, done } = await reader.read();
  if (done) break;
  acc += decoder.decode(value, { stream: true });
}
console.log("stream first events:", acc.slice(0, 700));
