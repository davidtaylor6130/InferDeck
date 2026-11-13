# Model bundles

A model bundle is a model whose files live in more than one Hugging Face
repository. Qwen-Image-2.1 is the first one: its diffusion weights, VAE and
text encoder come from three different repos. The model store installs a
bundle from a recipe in `data/model-bundles.json`. Adding a bundle is a JSON
edit; no C++ change is needed.

## Recipe format

```json
{
  "bundles": [
    {
      "id": "qwen-image-2.1-q8",
      "name": "Qwen-Image-2.1 Q8_0",
      "family": "qwen-image-2.1",
      "runtime": "stable_diffusion_cpp",
      "modality": "image",
      "capabilities": ["image_generation", "image_editing"],
      "license": "Qwen Research License (not open source)",
      "licenseUrl": "https://huggingface.co/Qwen/Qwen-Image-2.1/blob/main/LICENSE",
      "vramRequiredMb": 20000,
      "artifacts": { "guidance_scale": "6.0", "backend": "vulkan" },
      "components": [
        {
          "key": "diffusion_model",
          "repo": "leejet/Qwen-Image-2.1-GGUF",
          "file": "qwen_image_2.1-Q8_0.gguf",
          "revision": "cc11433936a06e9765f7c0c0b1f0436cfd2b9856",
          "required": true
        }
      ]
    }
  ]
}
```

| Field | Rule |
|---|---|
| `id` | Unique. Letters, digits, `-`, `_`, `.`. Used for the install folder. |
| `name`, `family` | Optional. Default to `id`. `family` is written to the registry entry. |
| `runtime` | A runtime the store knows: `llama_cpp`, `stable_diffusion_cpp`, `ace_step_cpp`, `whisper_cpp`, `sherpa_onnx`, `ltx_video_cpp`. |
| `modality` | Required. |
| `capabilities` | Optional. Defaults to the runtime's usual capability. |
| `license` | Required. Shown in discovery. `licenseUrl` is optional. |
| `vramRequiredMb` | Optional. Without it the registry gets the total download size in MiB. |
| `artifacts` | Optional string settings copied into the registry `artifacts` map, such as `guidance_scale`. |
| `components[].key` | The registry artifact key for the file. `lower_snake_case`, unique, and not also used in `artifacts`. |
| `components[].repo` | `owner/name`. |
| `components[].file` | Path inside the repo. Must be relative, use `/`, contain no `..`, and have an extension the runtime accepts. Two components cannot share a path. |
| `components[].revision` | Optional commit or branch. Pin a commit so every install gets the same bytes. Defaults to `main`. |
| `components[].required` | Optional, default `true`. An optional component that is missing upstream is skipped. A missing required one fails the install. At least one component must be required. |

An invalid recipe makes the whole file invalid, and the error names the
recipe and component. The file is read on each request, so edits apply
without a restart.

## API

- `GET /api/inferdeck/v1/model-store/bundles` returns `{"bundles": [...]}` with
  each recipe, its license and its components. It returns an empty list when
  the file is absent and 500 `model_bundles_invalid` when it does not parse.
- `POST /api/inferdeck/v1/model-store/downloads` with
  `{"bundle": "qwen-image-2.1-q8", "modelName": "qwen-image-2.1-q8"}` queues
  the install. Progress, cancel and resume work like any other store download.

## Install behaviour

The store reads each repo's file listing once, takes the size and SHA-256 of
every component, and queues one job. Files download into
`<store root>/<runtime>/<bundle id>/<modelName>/`, keeping each file's repo
path (the VAE lands in `vae/`). Each file is size and checksum checked before
the folder is moved into place. The registry entry then gets one artifact per
component key plus the recipe's `artifacts` settings. Both survive a restart
through the store manifest.

The gateway reads `data/model-bundles.json` relative to its working
directory, like `data/pricing.json`. A deploy must copy it to
`C:\InferDeck\data\`.

## Shipped bundles

| Id | Diffusion | Text encoder | Download |
|---|---|---|---|
| `qwen-image-2.1-q8` | Q8_0 | Qwen3-VL-8B Q8_0 | 18.2 GB |
| `qwen-image-2.1-q4` | Q4_K | Qwen3-VL-8B Q4_K_M | 11.1 GB |

Both include the BF16 VAE and the F16 vision projector, which editing needs.
See `docs/qwen-image-2.1.md` for measurements. The Q4 VRAM need was not
measured, so it falls back to the download size.
