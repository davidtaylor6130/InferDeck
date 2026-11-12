# Qwen-Image-2.1

InferDeck runs Qwen-Image-2.1 in-process through the `stable_diffusion_cpp`
runtime. It supports text-to-image generation and editing from up to four
reference images.

Status: the runtime builds and the request path is covered by unit tests.
Generation on the R9700 has not been measured yet; see
[Validation](#validation).

## License

The Qwen-Image-2.1 weights are published under the Qwen Research License
(`license: other`, `qwen-research` on Hugging Face), not an open-source
license. Read
[the license](https://huggingface.co/Qwen/Qwen-Image-2.1/blob/main/LICENSE)
before any use beyond research. The Qwen3-VL-8B text encoder is Apache-2.0.
InferDeck does not redistribute either.

## Runtime pin

`libs/third_party/stable-diffusion.cpp` is pinned to
`f8890b9db34a96d5c516382914971f4b4b92d402` (`master-924-f8890b9`, 2026-09-27).
This is the newest revision that builds against the ggml bundled with the
pinned llama.cpp (`73a43d1f`). Later revisions call `ggml_prec_set_acc`, which
that ggml does not have.

stable-diffusion.cpp is built with `SD_USE_UPSTREAM_GGML=ON` so it shares
llama.cpp's ggml. Consequences:

- INT8 `convrot` safetensors are not supported. Use GGUF or BF16 weights.
- FP8 weights are converted to F16 at load time.
- TAEQI2.1 previews and the `qwen-image-2.1-turbo` sigma preset arrived
  upstream after this pin and are not available.

Moving past this pin needs a llama.cpp update or a separate ggml for the image
runtime. Both are larger changes than this integration.

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
    vram_required_mb: 20000   # starting estimate; replace with the measured value
    artifacts:
      diffusion_model: "F:/InferDeck/models/image/qwen-image-2.1/qwen_image_2.1-Q8_0.gguf"
      vae: "F:/InferDeck/models/image/qwen-image-2.1/qwen_image_2.1_vae_bf16.safetensors"
      llm: "F:/InferDeck/models/image/qwen-image-2.1/Qwen3VL-8B-Instruct-Q8_0.gguf"
      llm_vision: "F:/InferDeck/models/image/qwen-image-2.1/mmproj-Qwen3VL-8B-Instruct-F16.gguf"
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
| `backend` | `vulkan` (default) or `cpu`. |
| `max_vram` | Optional GiB budget for managed weights. |

Declare `image_editing` only on models that accept reference images.

## API

Text to image works on every image route. Use a width and height divisible
by 64 and a guidance scale near 6.

Editing is an InferDeck extension on
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
  at most 8192x8192 pixels. The whole request body is limited to 16 MiB.
- Order matters when the prompt refers to "the first image".
- The response is the same as generation: `data[].b64_json` PNG images.
- The model must declare `image_editing`, otherwise the request fails with
  `unsupported_image_model`.

Strict `POST /v1/images/generations` rejects `reference_images` with
`unsupported_parameter`. The strict OpenAI surface is unchanged.

## Validation

Not yet run on hardware. To complete issue #189, record on the R9700:

1. Device, driver, and Vulkan SDK version.
2. Quantisation of each artifact.
3. Peak VRAM for one 1024x1024 generation and one single-reference edit.
4. Wall time and step count for both.
5. That an existing SD 1.5 or SDXL model still generates on the new pin.

Then replace `vram_required_mb` above with the measured peak plus margin.
