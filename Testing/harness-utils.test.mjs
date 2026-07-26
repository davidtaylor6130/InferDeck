import assert from "node:assert/strict";
import test from "node:test";

import { SseDecoder } from "./harness-utils.mjs";

const bytes = (text) => new TextEncoder().encode(text);

test("SseDecoder handles split LF and CRLF frames", () => {
  const decoder = new SseDecoder();
  assert.deepEqual(decoder.push(bytes('data: {"a":1}\r')), []);
  assert.deepEqual(decoder.push(bytes('\n\r\ndata: {"b":')), ['{"a":1}']);
  assert.deepEqual(decoder.push(bytes("2}\n\n")), ['{"b":2}']);
  assert.deepEqual(decoder.finish(), []);
});

test("SseDecoder flushes multiline final residue", () => {
  const decoder = new SseDecoder();
  assert.deepEqual(
    decoder.push(bytes(": heartbeat\ndata: first\ndata: second")),
    [],
  );
  assert.deepEqual(decoder.finish(), ["first\nsecond"]);
});
