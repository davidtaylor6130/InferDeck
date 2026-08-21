# Release reproducibility

`VERSION` is the authoritative product version. CMake reads it before
`project()`, native binaries receive it through `INFERDECK_VERSION`, the
dashboard reads it during the Vite build, and release metadata reads the same
file. Package metadata must match it at a release commit.

The Windows release environment is pinned to:

- GitHub's `windows-2025` runner image;
- Vulkan SDK `1.4.309.0`, downloaded from LunarG by a versioned URL and checked
  against SHA-256
  `48b132169b64fe65cdb0f20970195335a65354e73f1ea5373032c2a8bbad4297`;
- vcpkg baseline `77df67cfff9c12ccfdb52284e07c87c75092f723`;
- llama.cpp submodule `adb55e5148dc93bcdca7212a2d1df3ccc422959a`;
- Vulkan-Headers submodule `015e25c3c91b70eb1a754d36fb14c4ba6ad9b0b9`;
- sherpa-onnx `v1.13.2` Windows shared runtime archive, SHA-256
  `e1f9b7e6b17aec5a56ea1180ffd915a42eef86c7abf7a0749ddc530e0e0831e4`,
  plus its pinned C API header, SHA-256
  `437b1279047877167d8fadc74a60d47f3df514d703fdac1c1b6851da9bc2fdb4`;
- immutable commit SHAs for every GitHub Action.

`VULKAN_SDK` or `INFERDECK_VULKAN_SDK_ROOT` must identify the exact pinned SDK
directory. Configuration fails when the directory name or required headers,
import library, or shader compiler do not match. There is no developer-machine
fallback.

Update dependencies in a dedicated pull request. Change the vcpkg baseline and
submodule gitlinks explicitly, run the complete clean verification matrix, and
record upstream revisions and compatibility results in the pull request. Do not
track a branch, `latest`, or an unverified installer.

The release workflow emits `DEPENDENCIES.json`, `SBOM.spdx.json`,
`release-manifest.json`, and `SHA256SUMS.txt`. The dependency manifest records
locked-input hashes, exact installed vcpkg versions, and submodule commits. The
SPDX 2.3 document records the packaged dependency graph. The release manifest
hashes every packaged file, and `SHA256SUMS.txt` hashes the final archive.

Builds are expected to differ in PE timestamps and debug metadata until the
native toolchain supports deterministic linking for the complete vendored
graph. Compare release manifests and explain every differing hash. Functional
equivalence requires identical version, source commit, dependency manifest,
submodule revisions, dashboard assets, tests, and API contract results.

Signing is conditional on release credentials. When no signing certificate is
available, the workflow publishes checksummed but explicitly unsigned
artifacts. A signed release must verify the executable signature before
publication and attach a detached signature for the release manifest.
