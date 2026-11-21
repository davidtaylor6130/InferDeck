# Qwen-Image-2.1

InferDeck runs Qwen-Image-2.1 in-process through the `stable_diffusion_cpp`
runtime. It supports text-to-image generation and editing from up to four
reference images.

Status: validated on the R9700 on 2026-11-12. See [Validation](#validation)
for the measurements and two known limits.

## License

The Qwen-Image-2.1 weights are not open source. They are published under the
Qwen Research License Agreement (release date 2026-09-20; `license: other`,
`qwen-research` on Hugging Face). Per
[the license](https://huggingface.co/Qwen/Qwen-Image-2.1/blob/main/LICENSE):

- Use is for non-commercial research or evaluation use only (the agreement
  defines "Non-Commercial" as research or evaluation purposes only). Commercial
  use needs a separate licence from the licensor (Hangzhou Tongyi Laboratory
  Technology Co., Ltd.).
- Redistribution must carry the agreement, notices on modified files, and the
  attribution "Qwen is licensed under the Qwen RESEARCH LICENSE AGREEMENT,
  Copyright (c) 2026 Hangzhou Tongyi Laboratory Technology Co., Ltd. All Rights
  Reserved."
- Using the weights or their outputs to create, train, fine-tune or improve a
  distributed AI model requires prominently displaying "Built with Qwen" or
  "Improved using Qwen". "Qwen" may not be the primary name of a derivative.
- Starting IP litigation against the licensor over the Materials terminates
  the licence.

The Qwen3-VL-8B text encoder is Apache-2.0. InferDeck does not redistribute
either. Treat converted GGUF files (`leejet/Qwen-Image-2.1-GGUF`) as covered by
the same agreement.

## Runtime pin

`libs/third_party/stable-diffusion.cpp` is pinned to `01c463a`
(`master-1104-5dcfa79-1-g01c463a`, 2026-11-15). It builds against the ggml in
the runtime-upgrade llama.cpp and includes Qwen-Image-2.1 generation,
multi-reference editing, the `qwen_image_2_1_prefix_cache_type` model argument
and the `qwen-image-2.1-turbo` sigma preset. The limits recorded for the earlier
`f8890b9` pin (no turbo preset, no TAEQI2.1 previews, ggml mismatch with newer
revisions) no longer apply to the pin itself; InferDeck does not yet expose the
turbo preset or previews.

stable-diffusion.cpp is built with `SD_USE_UPSTREAM_GGML=ON` so it shares
llama.cpp's ggml, and the whole build uses `GGML_MAX_NAME=160` to match it; a
mismatch corrupts tensor lookups and aborts on the first generation. INT8
`convrot` safetensors have CPU and Vulkan kernels upstream at this pin but are
not tested in InferDeck; use GGUF or BF16 weights, which are the validated formats. FP8 weights
are converted to F16 at load time. The Validation section below was measured on
`f8890b9` and must be repeated on `01c463a`.

## Weights

| Artifact key | File | Source | Size |
|---|---|---|---|
| `diffusion_model` | `qwen_image_2.1-Q8_0.gguf` | `leejet/Qwen-Image-2.1-GGUF` | 7.16 GiB |
| `vae` | `vae/qwen_image_2.1_vae_bf16.safetensors` | `Comfy-Org/Qwen-Image-2.1` | 0.63 GiB |
| `llm` | `Qwen3VL-8B-Instruct-Q8_0.gguf` | `Qwen/Qwen3-VL-8B-Instruct-GGUF` | 8.11 GiB |
| `llm_vision` | `mmproj-Qwen3VL-8B-Instruct-F16.gguf` | `Qwen/Qwen3-VL-8B-Instruct-GGUF` | 1.08 GiB |

`llm_vision` is only needed for editing. Smaller options: `qwen_image_2.1-Q4_K.gguf`
(3.91 GiB) and `Qwen3VL-8B-Instruct-Q4_K_M.gguf` (4.68 GiB). The earlier Qwen
Image and Wan VAE files do not work with this model.

## Configuration

```yaml
model_registry:
  - name: qwen-image-2.1-q8
    family: qwen-image-2.1
    runtime: stable_diffusion_cpp
    modality: image
    capabilities: [image_generation, image_editing]
    n_slots: 1
    min_slots: 1
    vram_required_mb: 30000
    artifacts:
      diffusion_model: "E:/InferDeck/models/leejet/Qwen-Image-2.1-GGUF/qwen_image_2.1-Q8_0.gguf"
      vae: "E:/InferDeck/models/Comfy-Org/Qwen-Image-2.1/vae/qwen_image_2.1_vae_bf16.safetensors"
      llm: "E:/InferDeck/models/Qwen/Qwen3-VL-8B-Instruct-GGUF/Qwen3VL-8B-Instruct-Q8_0.gguf"
      llm_vision: "E:/InferDeck/models/Qwen/Qwen3-VL-8B-Instruct-GGUF/mmproj-Qwen3VL-8B-Instruct-F16.gguf"
      guidance_scale: "6.0"
      backend: vulkan
```

Image artifact keys for `stable_diffusion_cpp`:

| Key | Meaning |
|---|---|
| `model` | Single-file checkpoint (SD 1.5, SDXL). |
| `diffusion_model` | Standalone diffusion weights. Use instead of `model` for split models. |
| `vae`, `clip_l`, `clip_g`, `t5xxl` | Component weights for models that need them. |
| `llm` | LLM text encoder. |
| `llm_vision` | Vision projector for the LLM text encoder. Required for editing with GGUF encoders. |
| `model_args` | Passed to stable-diffusion.cpp unchanged, for example `qwen_image_2_1_prefix_cache_type=q8_0`. |
| `steps` | Sampling steps when the request sets none. Default 20. |
| `guidance_scale` | Guidance when the request sets none. Default 7.0. Use 6.0 for Qwen-Image-2.1. |
| `backend` | `vulkan` (default) or `cpu`. |
| `max_vram` | Optional GiB budget for managed weights. |

Declare `image_editing` only on models that accept reference images. It gates
reference images on `/v1/images/edits` and the JSON `reference_images` field;
a model without it is rejected with `unsupported_image_model`. Plain generation
needs only `image_generation`.

## API

Text to image works on every image route. Use a width and height divisible
by 64. Strict routes cannot send steps or guidance, so set the model's
`guidance_scale` artifact to 6.0; at the generic default of 7.0 the output
shows a woven texture.

Editing has two entry points. `POST /v1/images/edits` is the OpenAI-shaped
multipart route: send `image` or `image[]` files (one to four, at most 12 MiB
each), plus `model`, `prompt`, and optionally `size` and `n`. The format is
detected from the file bytes (PNG, JPEG or BMP); the declared content type is
ignored. Each image must be at most 8192 pixels per side and 16 megapixels. The
total multipart size limit is 16 MiB (413 `request_too_large` above it), chunked
bodies are refused, and `mask` or `input_fidelity: high` return
`unsupported_capability`. The same
route exists at `/api/inferdeck/v1/media/images/edits`. Both share the
generation path's validation, model capability check and output.

```sh
curl -H "Expect:" http://127.0.0.1:11434/v1/images/edits \
  -F model=qwen-image-2.1-q8 \
  -F "prompt=change the sign text to InferDeck" \
  -F size=1024x1024 \
  -F "image[]=@input.png;type=image/png"
```

The JSON form is an InferDeck extension on
`POST /api/inferdeck/v1/media/images/generations` and
`POST /compat/openai-derivative/v1/images/generations`. Add `reference_images`:

```json
{
  "model": "qwen-image-2.1-q8",
  "prompt": "change the sign text to 'InferDeck'",
  "size": "1024x1024",
  "reference_images": ["data:image/png;base64,iVBORw0KGgo..."]
}
```

- One to four entries, each a base64 `data:image/...` URL (PNG, JPEG, or BMP),
  at most 8192 pixels per side and 16 megapixels. The whole request body is
  limited to 16 MiB.
- Order matters when the prompt refers to "the first image".
- The response is the same as generation: `data[].b64_json` PNG images.
- The model must declare `image_editing`, otherwise the request fails with
  `unsupported_image_model`.

With curl, add `-H "Expect:"`. curl sends `Expect: 100-continue` for large
bodies and the gateway answers 417.

Strict `POST /v1/images/generations` rejects `reference_images` with
`unsupported_parameter`. The strict OpenAI surface is unchanged.

## Validation

Run on 2026-11-12 against a test gateway on port 11499, beside the live
service, which held 8.2 GB of VRAM throughout.

| Item | Value |
|---|---|
| GPU | AMD Radeon AI PRO R9700, 32 GB, driver 32.0.23002.1006 |
| Vulkan SDK | 1.4.309.0 |
| Weights | diffusion Q8_0, text encoder Q8_0, vision projector F16, VAE BF16 |
| Settings | 1024x1024, 20 steps, Vulkan, 256 px VAE tiling |

| Request | Wall time | Peak VRAM above the live baseline |
|---|---|---|
| SD 1.5, 512x512 (regression check) | 13.4 s including load | 2.8 GB |
| Qwen text to image, guidance 7.0 | 131.6 s including a 28 s load | 16.2 GB |
| Qwen text to image, guidance 6.0 | 104.2 s | not comparable, see below |
| Qwen edit, one 1024x1024 reference, guidance 6.0 | 257.3 s | 18.4 GB |

VRAM is the system-wide DXGI figure sampled every 0.5 s, so it is an upper
bound for the model. Outputs were checked by eye: text rendering was correct
in all Qwen images, and the edit changed the sign text and background as asked.

Known limits:

- **Edits run without the prefix cache when VRAM is tight.** With 8.2 GB held
  by the live service, stable-diffusion.cpp could not fit the cache, logged
  `insufficient memory for prefix caching`, and retried without it. The edit
  still completed, in about twice the time of a generation. `model_args: "qwen_image_2_1_prefix_cache_type=q8_0"` halves the
  cache and may avoid the retry; it was not measured.
- **Edit quality is below generation quality.** The edited image kept the
  subject but showed a visible woven texture at guidance 6.0, and lost the
  subject's face at 7.0. Not investigated further.

An untiled VAE decode at 1024x1024 ran out of VRAM in this setup;
stable-diffusion.cpp fell back to 256 px tiling on its own.
