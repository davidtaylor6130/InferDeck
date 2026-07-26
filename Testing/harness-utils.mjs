import assert from "node:assert/strict";

const DEFAULT_TIMEOUT_MS = 120_000;

function boundedInteger(value, fallback, minimum, maximum, label) {
  const parsed = value === undefined ? fallback : Number(value);
  if (!Number.isInteger(parsed) || parsed < minimum || parsed > maximum) {
    throw new Error(`${label} must be an integer from ${minimum} to ${maximum}`);
  }
  return parsed;
}

export function requestTimeoutMs() {
  return boundedInteger(
    process.env.REQUEST_TIMEOUT_MS,
    DEFAULT_TIMEOUT_MS,
    1_000,
    600_000,
    "REQUEST_TIMEOUT_MS",
  );
}

export function boundedEnvInteger(name, fallback, minimum, maximum) {
  return boundedInteger(process.env[name], fallback, minimum, maximum, name);
}

export function createDeadline(label, timeoutMs = requestTimeoutMs()) {
  const controller = new AbortController();
  const timer = setTimeout(
    () => controller.abort(new Error(`${label} exceeded ${timeoutMs}ms`)),
    timeoutMs,
  );
  timer.unref?.();
  return {
    signal: controller.signal,
    clear: () => clearTimeout(timer),
  };
}

export function formatFailure(error) {
  if (error instanceof Error) {
    if (error.name === "AbortError" && error.cause instanceof Error) {
      return error.cause.message;
    }
    return error.stack ?? error.message;
  }
  return String(error);
}

export async function readJsonResponse(response, label) {
  const text = await response.text();
  try {
    return JSON.parse(text);
  } catch (error) {
    throw new Error(`${label} returned invalid JSON: ${text.slice(0, 300)}`, {
      cause: error,
    });
  }
}

export function assertErrorResponse(body, expectedCode, label) {
  assert(body && typeof body === "object" && !Array.isArray(body), `${label}: body must be an object`);
  assert(body.error && typeof body.error === "object", `${label}: missing error object`);
  assert.equal(typeof body.error.message, "string", `${label}: error.message must be a string`);
  assert(body.error.message.length > 0, `${label}: error.message must not be empty`);
  assert.equal(body.error.code, expectedCode, `${label}: unexpected error code`);
}

export function assertCompletionResponse(body, label) {
  assert(body && typeof body === "object" && !Array.isArray(body), `${label}: body must be an object`);
  assert.equal(body.object, "chat.completion", `${label}: unexpected object type`);
  assert.equal(typeof body.id, "string", `${label}: id must be a string`);
  assert(Array.isArray(body.choices) && body.choices.length > 0, `${label}: choices must not be empty`);
  const choice = body.choices[0];
  assert(choice && typeof choice === "object", `${label}: first choice must be an object`);
  assert(choice.message && typeof choice.message === "object", `${label}: missing message`);
  assert.equal(choice.message.role, "assistant", `${label}: unexpected message role`);
  assert(
    typeof choice.message.content === "string" ||
      typeof choice.message.reasoning_content === "string" ||
      Array.isArray(choice.message.tool_calls),
    `${label}: assistant message contains no output`,
  );
  assert.equal(typeof choice.finish_reason, "string", `${label}: finish_reason must be a string`);
  assert(body.usage && typeof body.usage === "object", `${label}: missing usage`);
  for (const key of ["prompt_tokens", "completion_tokens", "total_tokens"]) {
    assert(Number.isInteger(body.usage[key]) && body.usage[key] >= 0, `${label}: invalid usage.${key}`);
  }
  assert.equal(
    body.usage.total_tokens,
    body.usage.prompt_tokens + body.usage.completion_tokens,
    `${label}: total_tokens invariant failed`,
  );
  return choice;
}

export class SseDecoder {
  #buffer = "";
  #decoder = new TextDecoder();

  push(chunk) {
    this.#buffer += this.#decoder.decode(chunk, { stream: true });
    return this.#drain(false);
  }

  finish() {
    this.#buffer += this.#decoder.decode();
    return this.#drain(true);
  }

  #drain(final) {
    const events = [];
    let match;
    while ((match = /\r?\n\r?\n/.exec(this.#buffer)) !== null) {
      const raw = this.#buffer.slice(0, match.index);
      this.#buffer = this.#buffer.slice(match.index + match[0].length);
      const event = parseEvent(raw);
      if (event !== null) events.push(event);
    }
    if (final && this.#buffer.length > 0) {
      const event = parseEvent(this.#buffer);
      this.#buffer = "";
      if (event !== null) events.push(event);
    }
    return events;
  }
}

function parseEvent(raw) {
  const data = [];
  for (const line of raw.split(/\r?\n/)) {
    if (line === "" || line.startsWith(":")) continue;
    if (line === "data") data.push("");
    else if (line.startsWith("data:")) data.push(line.slice(5).replace(/^ /, ""));
  }
  return data.length === 0 ? null : data.join("\n");
}

export async function readSseResponse(response, label) {
  const contentType = response.headers.get("content-type") ?? "";
  assert.match(contentType, /^text\/event-stream(?:;|$)/i, `${label}: expected text/event-stream`);
  assert(response.body, `${label}: response has no body`);

  const reader = response.body.getReader();
  const decoder = new SseDecoder();
  const payloads = [];
  try {
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      for (const payload of decoder.push(value)) payloads.push(payload);
    }
    for (const payload of decoder.finish()) payloads.push(payload);
  } finally {
    try {
      await reader.cancel();
    } catch {
    }
    reader.releaseLock();
  }

  let doneCount = 0;
  const events = [];
  for (const payload of payloads) {
    if (payload === "[DONE]") {
      doneCount++;
      continue;
    }
    try {
      events.push(JSON.parse(payload));
    } catch (error) {
      throw new Error(`${label}: invalid SSE JSON: ${payload.slice(0, 300)}`, {
        cause: error,
      });
    }
  }
  assert.equal(doneCount, 1, `${label}: expected exactly one [DONE] event`);
  return events;
}

export function assertChatChunk(event, label) {
  assert(event && typeof event === "object" && !Array.isArray(event), `${label}: event must be an object`);
  assert.equal(event.object, "chat.completion.chunk", `${label}: unexpected event object`);
  assert(Array.isArray(event.choices), `${label}: choices must be an array`);
  if (event.choices.length === 0) return null;
  const choice = event.choices[0];
  assert(choice && typeof choice === "object", `${label}: choice must be an object`);
  assert(choice.delta && typeof choice.delta === "object", `${label}: delta must be an object`);
  assert(
    choice.finish_reason === null || typeof choice.finish_reason === "string",
    `${label}: invalid finish_reason`,
  );
  return choice;
}
