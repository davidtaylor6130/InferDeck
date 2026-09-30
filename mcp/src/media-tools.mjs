import * as z from 'zod/v4';

const MAX_JOBS = 100;
const MAX_OUTPUT_BYTES = 25 * 1024 * 1024;

const toolText = (value, extraContent = []) => ({
  content: [{ type: 'text', text: JSON.stringify(value, null, 2) }, ...extraContent],
  structuredContent: value,
});

const imageInput = z.object({
  prompt: z.string().min(1).max(32000),
  model: z.string().min(1).max(256).optional(),
  n: z.number().int().min(1).max(10).optional(),
  size: z.string().regex(/^\d{3,4}x\d{3,4}$/).optional(),
}).strict();

const musicInput = z.object({
  model: z.string().min(1).max(256),
  prompt: z.string().min(1).max(4096),
  lyrics: z.string().max(32768).optional(),
  duration: z.number().finite().min(10).max(600).optional(),
  seed: z.number().int().min(-1).max(4294967295).optional(),
  steps: z.number().int().min(0).max(100).optional(),
  guidance_scale: z.number().finite().min(0).max(50).optional(),
}).strict();

const speechInput = z.object({
  model: z.string().min(1).max(256),
  input: z.string().min(1).max(4096),
  voice: z.union([z.string().min(1).max(256), z.object({ id: z.string().min(1).max(256) }).strict()]),
  instructions: z.string().max(4096).optional(),
  response_format: z.enum(['mp3', 'opus', 'aac', 'flac', 'wav', 'pcm']).optional(),
  speed: z.number().finite().min(0.25).max(4).optional(),
}).strict();

const jobsInput = z.object({
}).strict();

const outputInput = z.object({
  job_id: z.number().int().nonnegative().max(Number.MAX_SAFE_INTEGER),
  output_index: z.number().int().nonnegative().max(1000),
}).strict();

const jobIdInput = z.object({
  job_id: z.number().int().nonnegative().max(Number.MAX_SAFE_INTEGER),
}).strict();

function responseBody(response) {
  if (response?.body !== undefined) return response.body;
  return response;
}

function responseHeaders(response) {
  return response?.headers ?? {};
}

function header(response, name) {
  const headers = responseHeaders(response);
  if (typeof headers.get === 'function') return headers.get(name) ?? headers.get(name.toLowerCase());
  return headers[name] ?? headers[name.toLowerCase()] ?? headers[name.toUpperCase()];
}

function bytesToBase64(value) {
  if (typeof value === 'string') return Buffer.from(value, 'binary').toString('base64');
  if (value instanceof Uint8Array || Buffer.isBuffer(value)) return Buffer.from(value).toString('base64');
  throw new Error('InferDeck returned no binary media output');
}

function imageBlocks(imageData) {
  const blocks = [];
  let totalBytes = 0;
  for (const item of imageData) {
    if (typeof item?.b64_json !== 'string' || !item.b64_json) {
      throw new Error('InferDeck returned an image without b64_json data');
    }
    const bytes = Buffer.from(item.b64_json, 'base64').byteLength;
    if (!Number.isSafeInteger(bytes) || totalBytes > MAX_OUTPUT_BYTES - bytes) {
      throw new Error('InferDeck image outputs exceed the MCP 25 MiB retrieval limit');
    }
    totalBytes += bytes;
    blocks.push({ type: 'image', data: item.b64_json, mimeType: 'image/png' });
  }
  return blocks;
}

function binaryResponse(response) {
  const body = responseBody(response);
  const contentType = response?.contentType ?? response?.mimeType ?? header(response, 'content-type');
  if (!contentType?.startsWith('image/') && !contentType?.startsWith('audio/') && !contentType?.startsWith('video/')) {
    throw new Error('InferDeck returned an unsupported media type');
  }
  const bytes = typeof body === 'string' ? Buffer.byteLength(body, 'binary') : body?.byteLength;
  if (!Number.isSafeInteger(bytes) || bytes < 1 || bytes > MAX_OUTPUT_BYTES) {
    throw new Error('InferDeck media output exceeds the MCP retrieval limit');
  }
  return { contentType, data: bytesToBase64(body), bytes };
}

function hasLiveVideoCapability(client) {
  return client.videoGenerationAvailable === true
    || client.mediaCapabilities?.video_generation === true
    || client.mediaCapabilities?.videoGeneration === true
    || client.capabilities?.video_generation === true
    || client.capabilities?.videoGeneration === true;
}

const videoInput = z.object({
  model: z.string().min(1).max(256),
  prompt: z.string().min(1).max(4096),
  negative_prompt: z.string().max(4096).optional(),
  width: z.number().int().min(32).max(1280).refine((value) => value % 32 === 0).default(512),
  height: z.number().int().min(32).max(720).refine((value) => value % 32 === 0).default(320),
  frames: z.number().int().min(9).max(121).refine((value) => (value - 1) % 8 === 0).default(33),
  fps: z.number().int().min(1).max(60).default(24),
  steps: z.number().int().min(1).max(50).default(20),
  seed: z.number().int().min(-1).max(4294967295).optional(),
  guidance_scale: z.number().finite().min(0).max(20).default(6),
}).strict();

/** Register operator-scoped media tools. The orchestrator must explicitly opt in. */
export function registerMediaTools(server, client) {
  if (client?.mediaOperatorOptIn !== true) return false;
  if (!client || typeof client.request !== 'function') {
    throw new Error('Media tools require an orchestrator client.request(path, options) implementation');
  }

  server.registerTool('generate_image', {
    description: 'Synchronously generate an image through the operator-authorized InferDeck gateway.',
    inputSchema: imageInput,
  }, async (raw, ctx) => {
    const input = imageInput.parse(raw);
    const response = await client.request('/api/inferdeck/v1/media/images/generations', {
      method: 'POST', body: input, signal: ctx?.signal,
    });
    const payload = responseBody(response);
    const imageData = Array.isArray(payload?.data) ? payload.data : [];
    const jobId = header(response, 'x-inferdeck-job-id');
    const outputRefs = jobId
      ? imageData.map((_, index) => ({ job_id: Number(jobId), output_index: index,
        path: `/api/inferdeck/v1/media/jobs/${jobId}/outputs/${index}` }))
      : [];
    const images = imageBlocks(imageData);
    return toolText({ job_id: jobId ? Number(jobId) : null, output_count: imageData.length,
      output_refs: outputRefs,
      output_refs_best_effort: true,
      note: outputRefs.length === imageData.length
        ? 'Generation is synchronous. Saved output references are best effort; attached image data is authoritative.'
        : 'Generation is synchronous. Saved output references are unavailable; attached image data is authoritative.' }, images);
  });

  server.registerTool('generate_music', {
    description: 'Synchronously generate music/audio through the operator-authorized InferDeck gateway.',
    inputSchema: musicInput,
  }, async (raw, ctx) => {
    const input = musicInput.parse(raw);
    const response = await client.request('/api/inferdeck/v1/media/audio/generations', {
      method: 'POST', body: input, signal: ctx?.signal,
    });
    const media = binaryResponse(response);
    return toolText({ job_id: header(response, 'x-inferdeck-job-id') ?? null,
      content_type: media.contentType, bytes: media.bytes,
      note: 'Generation is synchronous; the returned audio is attached as MCP audio content.' },
    [{ type: 'audio', data: media.data, mimeType: media.contentType }]);
  });

  server.registerTool('synthesize_speech', {
    description: 'Synchronously synthesize speech through InferDeck /v1/audio/speech.',
    inputSchema: speechInput,
  }, async (raw, ctx) => {
    const input = speechInput.parse(raw);
    const response = await client.request('/v1/audio/speech', {
      method: 'POST', body: { ...input, stream_format: 'audio' }, signal: ctx?.signal,
    });
    const media = binaryResponse(response);
    return toolText({ content_type: media.contentType, bytes: media.bytes,
      note: 'TTS is synchronous and bounded by the orchestrator request timeout.' },
    [{ type: 'audio', data: media.data, mimeType: media.contentType }]);
  });

  {
    server.registerTool('generate_video', {
      description: 'Synchronously generate video through InferDeck LTX-2.3. This tool is advertised only when the live client capability probe explicitly reports video generation.',
      inputSchema: videoInput,
    }, async (raw, ctx) => {
      const input = videoInput.parse(raw);
      if (!hasLiveVideoCapability(client)) {
        throw new Error('InferDeck video generation is not currently available');
      }
      const response = await client.request('/api/inferdeck/v1/video/generations', {
        method: 'POST', body: input, signal: ctx?.signal,
      });
      const body = responseBody(response);
      if (body && typeof body === 'object' && !Buffer.isBuffer(body) && !(body instanceof Uint8Array)) {
        return toolText({ ...body, job_id: header(response, 'x-inferdeck-job-id') ?? body.job_id ?? null,
          note: 'Generation is synchronous. Use get_media_output for a stored playable output when available.' });
      }
      const media = binaryResponse(response);
      const jobId = header(response, 'x-inferdeck-job-id');
      const uri = jobId
        ? `inferdeck://media/jobs/${jobId}/outputs/0`
        : 'inferdeck://media/generations/latest';
      return toolText({ job_id: jobId ? Number(jobId) : null,
        content_type: media.contentType, bytes: media.bytes,
        note: 'Video generation is synchronous. The playable output is embedded below.' },
      [{ type: 'resource', resource: { uri, mimeType: media.contentType, blob: media.data } }]);
    });
  }

  server.registerTool('list_media_jobs', {
    description: 'List up to 100 global InferDeck media jobs. Results are operator-authorized and not per-key filtered.',
    inputSchema: jobsInput,
  }, async (raw, ctx) => {
    jobsInput.parse(raw);
    const payload = responseBody(await client.request('/api/inferdeck/v1/media/jobs', { method: 'GET', signal: ctx?.signal }));
    const jobs = Array.isArray(payload?.jobs) ? payload.jobs.slice(0, MAX_JOBS) : [];
    return toolText({ jobs, truncated: Array.isArray(payload?.jobs) && payload.jobs.length > MAX_JOBS });
  });

  server.registerTool('get_media_output', {
    description: 'Retrieve one bounded stored image/audio/video output as MCP media content.',
    inputSchema: outputInput,
  }, async (raw, ctx) => {
    const input = outputInput.parse(raw);
    const response = await client.request(
      `/api/inferdeck/v1/media/jobs/${input.job_id}/outputs/${input.output_index}`,
      { method: 'GET', signal: ctx?.signal },
    );
    const media = binaryResponse(response);
    const content = media.contentType.startsWith('image/')
      ? { type: 'image', data: media.data, mimeType: media.contentType }
      : media.contentType.startsWith('audio/')
        ? { type: 'audio', data: media.data, mimeType: media.contentType }
        : { type: 'resource', resource: { uri: `inferdeck://media/jobs/${input.job_id}/outputs/${input.output_index}`, mimeType: media.contentType, blob: media.data } };
    return toolText({ job_id: input.job_id, output_index: input.output_index,
      content_type: media.contentType, bytes: media.bytes }, [content]);
  });

  server.registerTool('cancel_media_job', {
    description: 'Cancel a running global InferDeck media job using operator authorization.',
    inputSchema: jobIdInput,
  }, async (raw, ctx) => {
    const input = jobIdInput.parse(raw);
    const response = await client.request(`/api/inferdeck/v1/media/jobs/${input.job_id}/cancel`, { method: 'POST', signal: ctx?.signal });
    return toolText({ ...responseBody(response), job_id: input.job_id });
  });

  return true;
}
