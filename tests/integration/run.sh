#!/usr/bin/env bash
# InferDeck integration test runner
# Usage: tests/integration/run.sh unit|integration|e2e|all
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${INFERDECK_BUILD_DIR:-$ROOT_DIR/build}"

if ! command -v ctest >/dev/null 2>&1; then
    for p in "/mnt/c/Program Files/CMake/bin/ctest.exe" \
             "/c/Program Files/CMake/bin/ctest.exe" \
             "/c/ProgramData/chocolatey/bin/ctest.exe" \
             "/usr/bin/ctest" \
             "/usr/local/bin/ctest"; do
        if [ -x "$p" ]; then
            CTEST_DIR="$(dirname "$p")"
            export PATH="$CTEST_DIR:$PATH"
            break
        fi
    done
    if ! command -v ctest >/dev/null 2>&1 && command -v where.exe >/dev/null 2>&1; then
        CTEST_PATH_RAW="$(where.exe ctest 2>/dev/null | tr -d '\r' | head -n 1)"
        if [ -n "$CTEST_PATH_RAW" ]; then
            CTEST_DIR_UNIX="$(cygpath -u "$(dirname "$CTEST_PATH_RAW")" 2>/dev/null || echo "")"
            if [ -n "$CTEST_DIR_UNIX" ] && [ -d "$CTEST_DIR_UNIX" ]; then
                export PATH="$CTEST_DIR_UNIX:$PATH"
            fi
        fi
    fi
fi

if ! command -v ctest >/dev/null 2>&1; then
    echo "ERROR: ctest not found in PATH"
    echo "Install CMake or add its bin directory to PATH"
    exit 1
fi

if [ ! -d "$BUILD_DIR" ]; then
    echo "ERROR: build directory not found: $BUILD_DIR"
    echo "Configure with: cmake -S $ROOT_DIR -B $BUILD_DIR"
    exit 1
fi

run_unit() {
    echo "==> Running unit tests..."
    ctest --test-dir "$BUILD_DIR" -C Debug -L unit --output-on-failure
}

run_integration() {
    echo "==> Running integration tests..."
    ctest --test-dir "$BUILD_DIR" -C Debug -L integration --output-on-failure
}

run_e2e() {
    echo "==> Running e2e tests (requires test GGUF)..."
    if [ -z "$INFERDECK_TEST_MODEL" ]; then
        DEFAULT_MODEL="C:/Inferdeck/models/Qwen/Qwen2.5-Coder-3B-Instruct-GGUF/qwen2.5-coder-3b-instruct-q4_k_m.gguf"
        if [ -f "$DEFAULT_MODEL" ]; then
            export INFERDECK_TEST_MODEL="$DEFAULT_MODEL"
            echo "Using default test model: $INFERDECK_TEST_MODEL"
        else
            echo "ERROR: e2e tests require INFERDECK_TEST_MODEL env var or"
            echo "  $DEFAULT_MODEL to exist"
            exit 1
        fi
    fi
    ctest --test-dir "$BUILD_DIR" -C Debug -L e2e --output-on-failure
}

run_all() {
    run_unit
    run_integration
    run_e2e
}

case "${1:-integration}" in
    unit)
        run_unit
        ;;
    integration)
        run_integration
        ;;
    e2e)
        run_e2e
        ;;
    all)
        run_all
        ;;
    *)
        echo "Usage: $0 {unit|integration|e2e|all}"
        exit 1
        ;;
esac
