#!/usr/bin/env bash
set -u

EXPECTED_BASE_URL="${EXPECTED_BASE_URL:-http://ai.homelab.com:11434/v1}"
FALLBACK_BASE_URL="${FALLBACK_BASE_URL:-http://127.0.0.1:11434/v1}"
MODEL="${MODEL:-qwen3.6-35b-a3b}"
SKIP_GENERATION="${SKIP_GENERATION:-0}"
FAILED=0

write_check() {
  printf '%s: %s\n' "$1" "$2"
}

json_value() {
  local file="$1"
  local expr="$2"
  python3 - "$file" "$expr" <<'PY'
import json
import sys

path, expr = sys.argv[1], sys.argv[2]
try:
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
except Exception:
    sys.exit(0)

def first_scalar(value, keys):
    if isinstance(value, dict):
        for key in keys:
            if key in value and isinstance(value[key], (str, int, float, bool)):
                return str(value[key])
        for item in value.values():
            found = first_scalar(item, keys)
            if found:
                return found
    elif isinstance(value, list):
        for item in value:
            found = first_scalar(item, keys)
            if found:
                return found
    return ""

if expr == "model":
    print(data.get("model", "") if isinstance(data, dict) else "")
elif expr == "provider_id":
    direct = first_scalar(data, ["providerID", "providerId"])
    providers = data.get("provider", {}) if isinstance(data, dict) else {}
    if not direct and isinstance(providers, dict) and len(providers) == 1:
        direct = next(iter(providers.keys()))
    print(direct)
elif expr == "base_url":
    model = data.get("model", "") if isinstance(data, dict) else ""
    provider_id = first_scalar(data, ["providerID", "providerId"])
    if not provider_id and "/" in model:
        provider_id = model.split("/", 1)[0]
    providers = data.get("provider", {}) if isinstance(data, dict) else {}
    if not provider_id and isinstance(providers, dict) and len(providers) == 1:
        provider_id = next(iter(providers.keys()))
    provider_cfg = providers.get(provider_id, {}) if isinstance(providers, dict) else {}
    options = provider_cfg.get("options", {}) if isinstance(provider_cfg, dict) else {}
    base = ""
    if isinstance(options, dict):
        base = options.get("baseURL") or options.get("baseUrl") or options.get("base_url") or ""
    print(base or first_scalar(data, ["baseURL", "baseUrl", "base_url"]))
PY
}

assistant_preview() {
  python3 -c 'import json, sys
try:
    data = json.load(sys.stdin)
    choice = data.get("choices", [{}])[0]
    message = choice.get("message", {})
    content = message.get("content") or ""
    reasoning = message.get("reasoning_content") or ""
    finish = choice.get("finish_reason")
    if content:
        print(f"content={content}; finish={finish}")
    elif reasoning:
        print(f"reasoning={reasoning}; finish={finish}")
    else:
        print(f"empty assistant text; finish={finish}")
except Exception as exc:
    print(f"parse failed: {exc}")'
}

ollama_preview() {
  python3 -c 'import json, sys
try:
    data = json.load(sys.stdin)
    message = data.get("message", {})
    content = message.get("content") or ""
    thinking = message.get("thinking") or data.get("thinking") or ""
    if content:
        print(f"content={content}")
    elif thinking:
        print(f"thinking={thinking}")
    else:
        print("empty assistant text")
except Exception as exc:
    print(f"parse failed: {exc}")'
}

tcp_check() {
  local host="$1"
  local port="$2"
  python3 - "$host" "$port" <<'PY'
import socket
import sys
host, port = sys.argv[1], int(sys.argv[2])
try:
    with socket.create_connection((host, port), timeout=5):
        print("ok")
except Exception as exc:
    print(f"failed: {exc}")
    sys.exit(1)
PY
}

retry_cmd() {
  local attempts="$1"
  local delay="$2"
  shift 2
  local i
  for ((i=1; i<=attempts; i++)); do
    if "$@"; then
      return 0
    fi
    sleep "$delay"
  done
  return 1
}

CONFIG_CANDIDATES=(
  "$HOME/.config/opencode/opencode.json"
  "$HOME/.config/opencode/config.json"
  "$HOME/Library/Application Support/opencode/opencode.json"
  "$HOME/Library/Application Support/opencode/config.json"
  "$(pwd)/opencode.json"
)

CONFIG_PATH=""
for candidate in "${CONFIG_CANDIDATES[@]}"; do
  if [[ -f "$candidate" ]]; then
    CONFIG_PATH="$candidate"
    break
  fi
done

SELECTED_MODEL=""
PROVIDER=""
BASE_URL=""
if [[ -n "$CONFIG_PATH" ]]; then
  SELECTED_MODEL="$(json_value "$CONFIG_PATH" model)"
  PROVIDER="$(json_value "$CONFIG_PATH" provider_id)"
  if [[ -z "$PROVIDER" && "$SELECTED_MODEL" == */* ]]; then
    PROVIDER="${SELECTED_MODEL%%/*}"
  fi
  BASE_URL="$(json_value "$CONFIG_PATH" base_url)"
fi
trimmed_base="${BASE_URL%/}"

write_check "config path" "${CONFIG_PATH:-not found}"
if command -v opencode >/dev/null 2>&1; then
  write_check "opencode version" "$(opencode --version 2>/dev/null || printf 'failed')"
else
  write_check "opencode version" "not found"
fi
write_check "selected provider" "${PROVIDER:-not found}"
write_check "selected model" "${SELECTED_MODEL:-not found}"
write_check "baseURL" "${BASE_URL:-not found}"
write_check "normalized baseURL" "${trimmed_base:-not found}"

if [[ -z "$CONFIG_PATH" ]]; then
  printf 'FAIL: OpenCode config file was not found on this machine; cannot prove OpenCode targets InferDeck.\n' >&2
  FAILED=1
fi

if command -v opencode >/dev/null 2>&1; then
  PATHS_OUTPUT="$(opencode debug paths 2>/dev/null || true)"
  LOG_PATH="$(printf '%s\n' "$PATHS_OUTPUT" | awk '$1=="log"{print substr($0, index($0,$2))}')"
  DATA_PATH="$(printf '%s\n' "$PATHS_OUTPUT" | awk '$1=="data"{print substr($0, index($0,$2))}')"
  TMP_PATH="$(printf '%s\n' "$PATHS_OUTPUT" | awk '$1=="tmp"{print substr($0, index($0,$2))}')"
  write_check "opencode data path" "${DATA_PATH:-unknown}"
  write_check "opencode log path" "${LOG_PATH:-unknown}"
  write_check "opencode tmp path" "${TMP_PATH:-unknown}"
  if [[ -n "${LOG_PATH:-}" && -d "$LOG_PATH" ]]; then
    latest_log="$(find "$LOG_PATH" -type f -name '*.log' -print0 2>/dev/null | xargs -0 ls -t 2>/dev/null | head -n 1 || true)"
    write_check "opencode latest log" "${latest_log:-none}"
    if [[ -n "$latest_log" ]]; then
      latest_errors="$(grep -Ei 'ERROR|WARN|SQLITE|busy|failed|tool|read' "$latest_log" | tail -n 12 || true)"
      if [[ -n "$latest_errors" ]]; then
        printf 'opencode latest relevant log lines:\n%s\n' "$latest_errors"
      else
        write_check "opencode latest relevant log lines" "none"
      fi
    fi
  fi
fi

if [[ -z "$SELECTED_MODEL" ]]; then
  write_check "selected model warning" "no top-level model set; OpenCode may use CLI/session/default model"
fi

if [[ -n "$SELECTED_MODEL" && "$SELECTED_MODEL" == */* && -n "$PROVIDER" && "${SELECTED_MODEL%%/*}" != "$PROVIDER" ]]; then
  printf 'FAIL: OpenCode selected model provider prefix (%s) does not match selected provider (%s).\n' "${SELECTED_MODEL%%/*}" "$PROVIDER" >&2
  FAILED=1
fi

if [[ "$trimmed_base" != "${EXPECTED_BASE_URL%/}" && "$trimmed_base" != "${FALLBACK_BASE_URL%/}" ]]; then
  printf 'FAIL: OpenCode provider/baseURL is not targeting InferDeck. Expected %s or %s.\n' "$EXPECTED_BASE_URL" "$FALLBACK_BASE_URL" >&2
  FAILED=1
fi

if DNS_IP="$(python3 - <<'PY'
import socket
try:
    print(", ".join(sorted({info[4][0] for info in socket.getaddrinfo("ai.homelab.com", 11434)})))
except Exception as exc:
    print(f"failed: {exc}")
    raise SystemExit(1)
PY
)"; then
  write_check "ai.homelab.com DNS/IP" "$DNS_IP"
else
  write_check "ai.homelab.com DNS/IP" "$DNS_IP"
  FAILED=1
fi

if TCP_RESULT="$(retry_cmd 3 2 tcp_check ai.homelab.com 11434)"; then
  write_check "TCP ai.homelab.com:11434" "$TCP_RESULT"
else
  write_check "TCP ai.homelab.com:11434" "$TCP_RESULT"
  FAILED=1
fi

API_BASE="${trimmed_base:-${EXPECTED_BASE_URL%/}}"
ROOT_BASE="${API_BASE%/v1}"

if MODELS_JSON="$(curl -fsS --retry 2 --retry-delay 2 --retry-connrefused --max-time 15 "$API_BASE/models")"; then
  MODEL_IDS="$(printf '%s' "$MODELS_JSON" | python3 -c 'import json, sys; data = json.load(sys.stdin); print(", ".join(item.get("id", "") for item in data.get("data", [])[:5]))')"
  write_check "/v1/models result" "$MODEL_IDS"
else
  write_check "/v1/models result" "FAILED"
  FAILED=1
fi

if [[ "$SKIP_GENERATION" != "1" ]]; then
  CHAT_BODY="$(python3 - "$MODEL" <<'PY'
import json
import sys
print(json.dumps({
    "model": sys.argv[1],
    "stream": False,
    "max_tokens": 16,
    "messages": [{"role": "user", "content": "Reply with exactly: inferdeck opencode diag ok"}],
}))
PY
)"
  if CHAT_JSON="$(curl -fsS --retry 1 --retry-delay 2 --retry-connrefused --max-time 180 -H "Content-Type: application/json" --data "$CHAT_BODY" "$API_BASE/chat/completions")"; then
    write_check "/v1/chat/completions tiny result" "$(printf '%s' "$CHAT_JSON" | assistant_preview)"
  else
    write_check "/v1/chat/completions tiny result" "FAILED"
    FAILED=1
  fi

  OLLAMA_BODY="$(python3 - "$MODEL" <<'PY'
import json
import sys
print(json.dumps({
    "model": sys.argv[1],
    "stream": False,
    "messages": [{"role": "user", "content": "Reply with exactly: inferdeck webui diag ok"}],
    "options": {"num_predict": 16},
}))
PY
)"
  if OLLAMA_JSON="$(curl -fsS --retry 1 --retry-delay 2 --retry-connrefused --max-time 180 -H "Content-Type: application/json" --data "$OLLAMA_BODY" "$ROOT_BASE/api/chat")"; then
    write_check "/api/chat Open WebUI control result" "$(printf '%s' "$OLLAMA_JSON" | ollama_preview)"
  else
    write_check "/api/chat Open WebUI control result" "FAILED"
    FAILED=1
  fi
fi

if [[ "$FAILED" -ne 0 ]]; then
  exit 1
fi
printf 'OpenCode InferDeck diagnostics passed.\n'
