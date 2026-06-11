const BASE = process.env.GATEWAY_URL ?? "http://127.0.0.1:11434";
const MODEL = process.env.GATEWAY_MODEL ?? "qwen3.6-35b-a3b";

const filler = "The quick brown fox jumps over the lazy dog. ".repeat(400);
const base = [
  { role: "system", content: "You are concise." },
  { role: "user", content: filler + "\nSay OK." },
];

async function ask(messages, label) {
  const t0 = Date.now();
  const res = await fetch(`${BASE}/v1/chat/completions`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ model: MODEL, stream: false, max_tokens: 16, messages }),
  });
  const body = await res.json();
  const u = body.usage ?? {};
  console.log(`${label}: status=${res.status} prompt=${u.prompt_tokens} cached=${u.prompt_tokens_details?.cached_tokens} wall=${Date.now() - t0}ms`);
  return body.choices?.[0]?.message?.content ?? "";
}

const reply = await ask(base, "turn 1");
const followup = [
  ...base,
  { role: "assistant", content: reply || "OK" },
  { role: "user", content: "Now say DONE." },
];
await ask(followup, "turn 2");
