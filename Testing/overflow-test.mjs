const BASE = process.env.GATEWAY_URL ?? "http://127.0.0.1:11434";
const MODEL = process.env.GATEWAY_MODEL ?? "qwen2.5-coder-3b";

const hugeText = "alpha bravo charlie delta echo foxtrot golf hotel ".repeat(5000);
const body = (stream) => ({
  model: MODEL,
  stream,
  messages: [
    { role: "system", content: "You are a helpful assistant." },
    { role: "user", content: hugeText + "\nSummarize the above." },
  ],
});

const res = await fetch(`${BASE}/v1/chat/completions`, {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify(body(false)),
});
console.log("non-stream status:", res.status);
console.log("non-stream body:", (await res.text()).slice(0, 400));

const res2 = await fetch(`${BASE}/v1/chat/completions`, {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify(body(true)),
});
console.log("stream status:", res2.status);
const reader = res2.body.getReader();
const decoder = new TextDecoder();
let acc = "";
while (acc.length < 2000) {
  const { value, done } = await reader.read();
  if (done) break;
  acc += decoder.decode(value, { stream: true });
}
console.log("stream first events:", acc.slice(0, 600));
