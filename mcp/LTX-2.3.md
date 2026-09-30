# LTX-2.3 video setup

InferDeck uses its bundled native video runtime for text-to-video generation. The
request runs inside the gateway and uses model admission, progress, cancellation,
and media history.

Obtain matching LTX-2.3 diffusion weights, Gemma 3 text encoder, embedding
connectors, video VAE, and audio VAE. The [upstream setup](https://github.com/leejet/stable-diffusion.cpp/blob/master/docs/ltx2.md)
lists compatible artifacts. Review the [model terms](https://huggingface.co/Lightricks/LTX-2.3)
before distributing weights. Downloads are separate from the code build.

Merge [the example model entry](ltx-2.3-profile.example.yml) into your gateway
configuration and replace its artifact paths. Its VRAM value is an admission
estimate, not proof that every dimensions and frame-count combination fits. Start
with small dimensions and a short clip.

The API accepts `POST /api/inferdeck/v1/media/video/generations` with JSON:

```json
{
  "model": "ltx-2.3",
  "prompt": "A red balloon floating above a quiet field",
  "negative_prompt": "",
  "width": 512,
  "height": 320,
  "frames": 33,
  "fps": 24,
  "steps": 20,
  "seed": 42,
  "guidance_scale": 6
}
```

The verified Windows/AMD placement uses `diffusion=vulkan0,te=cpu`: diffusion
stays on Vulkan while the Gemma text encoder runs on CPU. This avoids a large
text-encoder Vulkan allocation, at the cost of additional host RAM and CPU work.
A real 256x256, 17-frame clip at 8 fps was generated through the dashboard and verified with browser video and stereo-audio decoding, saved-preview reload, and MCP retrieval. Begin with a small clip before increasing resolution or duration.

The call waits for generation and returns video bytes. The job identifier is
returned in `X-InferDeck-Job-Id`; saved outputs use the existing media job API.
MCP exposes generation, cancellation, and output retrieval when media access is
enabled. Configure client timeouts to accommodate generation.

Windows builds return browser-playable MP4 with H.264 video and AAC audio.
Previously saved AVI files remain downloadable. Non-Windows builds use AVI.
No external encoding process is required.

The Video dashboard is a full section with Generate, Model Settings, Model Store,
Usage, and Health & alerts pages. Completed MP4 jobs can appear in the saved-video
preview after reload; AVI jobs remain download-only. The dashboard and MCP use
the same saved media-job records.

The initial profile bounds frames to 9 through 121 in increments of 8, dimensions to multiples of 32, with each dimension at least 64, up to 1280 by 720, and sampling to 50 steps. Encoded output is
limited to 25 MiB. Reduce the clip size if that limit is exceeded. Saved job access
and cancellation require operator credentials; generation uses the inference API
key.

Image-to-video, video editing, and performance tuning are not part of this
endpoint. Actual LTX inference requires the complete model artifact set; protocol
fixtures and encoder tests alone do not establish model quality or Windows/AMD
performance.
