#!/bin/bash
# build.sh â€” Build script for InferDeck gateway service
# Usage: ./scripts/build.sh [debug|release]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_ROOT}/build"
VCPKG_ROOT="${PROJECT_ROOT}/vcpkg"
CONFIGURATION="${1:-Release}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# Check prerequisites
check_prerequisites() {
    if ! command -v cmake &> /dev/null; then
        log_error "cmake not found. Install cmake 3.27+ first."
        exit 1
    fi

    if ! command -v cmake --version &> /dev/null 2>&1; then
        CMAKE_VERSION=$(cmake --version | head -1 | awk '{print $3}')
        CMAKE_MAJOR=$(echo $CMAKE_VERSION | cut -d. -f1)
        CMAKE_MINOR=$(echo $CMAKE_VERSION | cut -d. -f2)
        if [ "$CMAKE_MAJOR" -lt 3 ] || ([ "$CMAKE_MAJOR" -eq 3 ] && [ "$CMAKE_MINOR" -lt 27 ]); then
            log_error "cmake version too old. Need 3.27+, got $CMAKE_VERSION"
            exit 1
        fi
    fi
}

# Clone vcpkg if not present
setup_vcpkg() {
    if [ ! -d "$VCPKG_ROOT" ]; then
        log_info "Cloning vcpkg..."
        git clone https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
    fi
}

# Install dependencies
install_deps() {
    log_info "Installing dependencies via vcpkg..."
    cd "$VCPKG_ROOT"
    ./bootstrap-vcpkg.sh 2>/dev/null
    ./vcpkg install --triplet=x64-windows
    cd "$PROJECT_ROOT"
}

# Generate TLS certs
generate_certs() {
    local cert_dir="${BUILD_DIR}/certs"
    mkdir -p "$cert_dir"

    if [ -f "${cert_dir}/server.crt" ] && [ -f "${cert_dir}/server.key" ]; then
        log_warn "Certs already exist, skipping"
        return
    fi

    log_info "Generating self-signed TLS certs..."
    openssl req -x509 -newkey rsa:4096 \
        -keyout "${cert_dir}/server.key" \
        -out "${cert_dir}/server.crt" \
        -days 365 -nodes \
        -subj "/C=US/ST=State/L=City/O=InferDeck/CN=localhost" 2>/dev/null
    log_info "Certs generated in ${cert_dir}/"
}

# Configure with CMake
configure() {
    log_info "Configuring CMake (${CONFIGURATION})..."
    cd "$BUILD_DIR"
    cmake \
        -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
        -DVCPKG_TARGET_TRIPLET=x64-windows \
        -DCMAKE_BUILD_TYPE="${CONFIGURATION}" \
        ..
    cd "$PROJECT_ROOT"
}

# Build
build() {
    log_info "Building (${CONFIGURATION})..."
    cd "$BUILD_DIR"
    cmake --build . --config "${CONFIGURATION}" -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    cd "$PROJECT_ROOT"
}

# Run tests
run_tests() {
    log_info "Running tests..."
    cd "$BUILD_DIR"
    ctest -C "${CONFIGURATION}" --output-on-failure
    cd "$PROJECT_ROOT"
}

# Package for Windows
package() {
    log_info "Packaging for Windows..."
    local pkg_dir="${BUILD_DIR}/inferdeck-${CONFIGURATION}"
    mkdir -p "$pkg_dir"

    # Copy executable
    cp "${BUILD_DIR}/bin/Release/inferdeck-gateway.exe" "$pkg_dir/" 2>/dev/null || true
    # Copy certs
    cp -r "${BUILD_DIR}/certs" "$pkg_dir/" 2>/dev/null || true
    # Copy config
    mkdir -p "${pkg_dir}/config"
    cp "${PROJECT_ROOT}/config/gateway.yml.example" "${pkg_dir}/config/" 2>/dev/null || true
    # Copy README
    cp "${PROJECT_ROOT}/README.md" "$pkg_dir/" 2>/dev/null || true

    log_info "Packaged files in ${pkg_dir}/"
}

# Main
main() {
    log_info "Building InferDeck gateway service (${CONFIGURATION})"

    check_prerequisites
    setup_vcpkg
    install_deps
    generate_certs
    configure
    build
    run_tests
    package

    log_info "Build complete! Output: ${BUILD_DIR}/"
}

main "$@"
