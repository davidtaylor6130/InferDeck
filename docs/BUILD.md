# InferDeck Gateway — Build Guide

## Prerequisites

- **CMake**: 3.27 or later
- **C++ Compiler**: MSVC (Windows) or Clang (cross-compile)
- **Git**: For vcpkg cloning
- **OpenSSL**: For self-signed cert generation
- **Internet connection**: For vcpkg dependency downloads

## Quick Start

```bash
# One-command build (Release)
./scripts/build.sh Release

# Debug build
./scripts/build.sh Debug

# Run the gateway
./build/inferdeck-gateway.exe

# With custom config
./build/inferdeck-gateway.exe -c config/gateway.yml
```

## Manual Build

### 1. Clone and bootstrap vcpkg

```bash
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh
```

### 2. Install dependencies

```bash
./vcpkg install \
  vulkan-headers:x64-windows \
  vulkan-loader:x64-windows \
  cpp-httplib:x64-windows \
  spdlog:x64-windows \
  catch2:x64-windows \
  nlohmann-json:x64-windows \
  fmt:x64-windows \
  yaml-cpp:x64-windows \
  google-benchmark:x64-windows
```

### 3. Configure and build

```bash
mkdir -p build && cd build
cmake \
  -DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-windows \
  -DCMAKE_BUILD_TYPE=Release \
  ..
cmake --build . --config Release -j
```

### 4. Run tests

```bash
cd build
ctest --config Release --output-on-failure
```

### 5. Create installer

```bash
cd scripts
./package_installer.sh 0.1.0
# Output: build/inferdeck-gateway-0.1.0-windows-x64.exe
```

## Configuration

Copy and edit the example config:

```bash
cp config/gateway.example.yml config/gateway.yml
```

Key settings in `config/gateway.yml`:

- `model.path`: Path to your GGUF file
- `gpu.device_id`: GPU index (-1 for auto-detect)
- `server.tls.enabled`: Enable/disable HTTPS
- `logging.level`: trace, debug, info, warn, error, fatal

## TLS Certificates

Self-signed certificates are generated during the build process via `scripts/build.sh`.

To regenerate manually:

```bash
mkdir -p certs
openssl req -x509 -newkey rsa:4096 \
  -keyout certs/server.key \
  -out certs/server.crt \
  -days 365 -nodes \
  -subj "/C=US/ST=State/L=City/O=InferDeck/CN=localhost"
```

## Troubleshooting

### Vulkan not found on Windows

1. Install Vulkan SDK from [vulkan.lunarg.com](https://vulkan.lunarg.com/)
2. Ensure `VULKAN_SDK` environment variable points to installation
3. Verify GPU drivers support Vulkan 1.3+

### llama.cpp compilation errors

1. Ensure your GPU supports Vulkan (AMD RX 5000+ or NVIDIA GTX 1060+)
2. Check Vulkan SDK version is ≥ 1.3.0
3. Update GPU drivers

### TLS handshake failures

1. Verify certs exist in `certs/` directory
2. Check `config/gateway.yml` tls.cert_file and tls.key_file paths
3. For production, replace self-signed certs with CA-issued certificates

### Model loading failures

1. Verify GGUF file path in `config/gateway.yml`
2. Ensure GGUF file is not corrupted
3. Check VRAM availability matches model size

## Cross-Compilation (macOS → Windows)

```bash
# Install mingw-w64
brew install mingw-w64

# Configure for Windows
mkdir -p build-windows && cd build-windows
cmake \
  -DCMAKE_TOOLCHAIN_FILE=../vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-windows-static \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
  -DCMAKE_BUILD_TYPE=Release \
  ..

cmake --build . --config Release -j$(sysctl -n hw.ncpu)
```

## Benchmark

Run Google Benchmarks after building:

```bash
# Build benchmarks
cmake --build . --config Release --target benchmarks

# Run
./build/benchmarks
```

## Code Generation

Generate Doxygen documentation:

```bash
doxygen docs/Doxyfile
# Output: docs/doxygen/html/, docs/doxygen/xml/
```
