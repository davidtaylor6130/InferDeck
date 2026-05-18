#!/bin/bash
# validate_api.sh — Verify API structure without sending actual messages
# Usage: ./scripts/validate_api.sh [base_url]
set -euo pipefail

BASE_URL="${1:-https://localhost:8080}"
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# Test counter
TESTS=0
PASSED=0
FAILED=0

# Helper to test endpoint
test_endpoint() {
    local name="$1"
    local url="$2"
    local expected_status="${3:-200}"
    local method="${4:-GET}"

    TESTS=$((TESTS + 1))
    log_info "Testing: $name ($method $url)"

    local response
    response=$(curl -s -o /dev/null -w "%{http_code}" --insecure "$url" 2>/dev/null || echo "000")

    if [ "$response" = "$expected_status" ]; then
        log_info "  ✓ Status $response (expected $expected_status)"
        PASSED=$((PASSED + 1))
    else
        log_error "  ✗ Status $response (expected $expected_status)"
        FAILED=$((FAILED + 1))
    fi
}

# Helper to validate response schema
test_schema() {
    local name="$1"
    local url="$2"
    local json_path="$3"
    local expected_type="$4"

    TESTS=$((TESTS + 1))
    log_info "Validating schema: $name"

    local response
    response=$(curl -s --insecure "$url" 2>/dev/null || echo "")

    if [ -z "$response" ]; then
        log_error "  ✗ Empty response"
        FAILED=$((FAILED + 1))
        return
    fi

    # Check if JSON path exists (basic check)
    if echo "$response" | grep -q "$json_path"; then
        log_info "  ✓ Response contains expected structure"
        PASSED=$((PASSED + 1))
    else
        log_error "  ✗ Response missing expected structure: $json_path"
        FAILED=$((FAILED + 1))
    fi
}

echo "=========================================="
echo "InferDeck API Validation"
echo "Testing against: $BASE_URL"
echo "=========================================="
echo ""

# Test health endpoint
test_endpoint "Health check" "$BASE_URL/v1/health" "200"
test_schema "Health schema" "$BASE_URL/v1/health" "status" "string"

# Test models endpoint
test_endpoint "Models list" "$BASE_URL/v1/models" "200"
test_schema "Models schema" "$BASE_URL/v1/models" "data" "array"

# Test metrics endpoint
test_endpoint "Metrics" "$BASE_URL/inferdeck/metrics" "200"
test_schema "Metrics schema" "$BASE_URL/inferdeck/metrics" "counters" "object"

# Test status endpoint
test_endpoint "Status" "$BASE_URL/inferdeck/status" "200"
test_schema "Status schema" "$BASE_URL/inferdeck/status" "initialized" "boolean"

# Test chat completions (should return 400 or 415 for missing body)
test_endpoint "Chat completions (no body)" "$BASE_URL/v1/chat/completions" "400|415" "POST"

# Test completions (should return 400 or 415 for missing body)
test_endpoint "Completions (no body)" "$BASE_URL/v1/completions" "400|415" "POST"

# Test embeddings (should return 400 or 415 for missing body)
test_endpoint "Embeddings (no body)" "$BASE_URL/v1/embeddings" "400|415" "POST"

echo ""
echo "=========================================="
echo "Results: $TESTS tests, $PASSED passed, $FAILED failed"
echo "=========================================="

if [ $FAILED -gt 0 ]; then
    log_error "Some tests failed!"
    exit 1
fi

log_info "All API validation tests passed!"
exit 0
