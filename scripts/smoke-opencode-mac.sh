#!/usr/bin/env bash
set -u

EXPECTED_BASE_URL="${EXPECTED_BASE_URL:-http://ai.homelab.com:11434/v1}"
DASHBOARD_BASE_URL="${DASHBOARD_BASE_URL:-http://ai.homelab.com:8080}"
MODEL="${MODEL:-inferdeck/qwen3.6-35b-a3b:latest}"
MAX_OPENCODE_SECONDS="${MAX_OPENCODE_SECONDS:-240}"
FAILED=0

write_check() {
  printf '%s: %s\n' "$1" "$2"
}

fail_check() {
  write_check "$1" "$2"
  FAILED=1
}

openwebui_control() {
  local label="$1"
  local root_base="${EXPECTED_BASE_URL%/v1}"
  local body
  body="$(python3 - "$label" <<'PY'
import json
import sys
label = sys.argv[1]
print(json.dumps({
    "model": "qwen3.6-35b-a3b",
    "stream": False,
    "messages": [{"role": "user", "content": f"Reply with exactly: webui {label} ok"}],
    "options": {"num_predict": 16},
}))
PY
)"
  if curl -fsS --max-time 180 -A "Open WebUI" -H "Content-Type: application/json" --data "$body" "$root_base/api/chat" >/dev/null; then
    write_check "Open WebUI /api/chat control ${label}" "ok"
    return 0
  fi
  fail_check "Open WebUI /api/chat control ${label}" "failed"
  return 1
}

dashboard_backend_check() {
  local label="$1"
  local status_json
  status_json="$(curl -fsS --max-time 15 "$DASHBOARD_BASE_URL/api/status" 2>/dev/null || true)"
  local summary
  summary="$(printf '%s' "$status_json" | python3 -c 'import json, sys
try:
    data = json.load(sys.stdin)
except Exception as exc:
    print("parse_failed=" + str(exc))
    raise SystemExit(2)
queue = data.get("queue", {})
services = data.get("services", [])
llama_services = [svc for svc in services if svc.get("id") == "llama-server" or svc.get("kind") == "llama_cpp"]
print("queue.running=" + str(queue.get("running")))
print("queue.queued=" + str(queue.get("queued")))
print("llama.services=" + str(len(llama_services)))
for svc in llama_services:
    print("llama.status=" + str(svc.get("status")))
    print("llama.pid=" + str(svc.get("pid")))
if queue.get("running") != 0:
    raise SystemExit(3)
if queue.get("queued") not in (0, None):
    raise SystemExit(4)
if len(llama_services) != 1:
    raise SystemExit(5)
if llama_services[0].get("status") != "running":
    raise SystemExit(6)
if not llama_services[0].get("pid"):
    raise SystemExit(7)
')"
  local status_code=$?
  printf 'dashboard backend check %s:\n%s\n' "$label" "$summary"
  if [[ "$status_code" -ne 0 ]]; then
    fail_check "dashboard backend check ${label}" "failed"
    return 1
  fi
  return 0
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v python3 >/dev/null 2>&1; then
  fail_check "python3" "not found"
fi
if ! command -v curl >/dev/null 2>&1; then
  fail_check "curl" "not found"
fi
if ! command -v opencode >/dev/null 2>&1; then
  fail_check "opencode" "not found"
fi
if [[ "$FAILED" -ne 0 ]]; then
  exit 1
fi

write_check "repo path" "$(pwd)"

if ! EXPECTED_BASE_URL="$EXPECTED_BASE_URL" SKIP_GENERATION=0 "$script_dir/diagnose-opencode-inferdeck.sh"; then
  fail_check "diagnostics" "failed"
fi

before_json="$(curl -fsS --max-time 15 "$DASHBOARD_BASE_URL/api/status" 2>/dev/null || true)"
before_job="$(printf '%s' "$before_json" | python3 -c 'import json,sys
try:
    data=json.load(sys.stdin)
    job=data.get("observability",{}).get("lastOpenCodeRequest")
    print((job or {}).get("id",""))
except Exception:
    print("")
')"
write_check "last OpenCode before" "${before_job:-none}"
openwebui_control "before"
dashboard_backend_check "before"

prompt="Inspect this repo/site after reading local files and make a concise loading-speed plan. Do not edit files."
opencode_output="$(mktemp "${TMPDIR:-/tmp}/inferdeck-opencode.XXXXXX.log")"
opencode run -m "$MODEL" "$prompt" >"$opencode_output" 2>&1 &
opencode_pid=$!
write_check "opencode pid" "$opencode_pid"

new_job_seen=0
new_job_seen_at=0
run_status="running"
for ((i=1; i<=MAX_OPENCODE_SECONDS; i++)); do
  poll_json="$(curl -fsS --max-time 5 "$DASHBOARD_BASE_URL/api/status" 2>/dev/null || true)"
  poll_job="$(printf '%s' "$poll_json" | python3 -c 'import json,sys
try:
    data=json.load(sys.stdin)
    job=(data.get("observability",{}).get("lastOpenCodeRequest") or {})
    print(job.get("id",""))
except Exception:
    print("")
')"
  if [[ -n "$poll_job" && "$poll_job" != "$before_job" && "$new_job_seen" -eq 0 ]]; then
    new_job_seen=1
    new_job_seen_at=$i
    write_check "new OpenCode job seen" "after ${i}s: $poll_job"
    openwebui_control "during"
  fi
  if ! kill -0 "$opencode_pid" 2>/dev/null; then
    wait "$opencode_pid"
    opencode_exit=$?
    run_status="exited:$opencode_exit"
    break
  fi
  sleep 1
done

if kill -0 "$opencode_pid" 2>/dev/null; then
  run_status="timeout"
  kill "$opencode_pid" 2>/dev/null || true
  wait "$opencode_pid" 2>/dev/null || true
fi

write_check "opencode run status" "$run_status"
tail -n 40 "$opencode_output" || true
if [[ "$run_status" == "timeout" ]]; then
  if [[ "$new_job_seen" -eq 0 ]]; then
    fail_check "stall classification" "no request sent to InferDeck within ${MAX_OPENCODE_SECONDS}s"
  else
    fail_check "stall classification" "OpenCode did not finish after InferDeck accepted a request"
  fi
elif [[ "$run_status" != "exited:0" ]]; then
  fail_check "stall classification" "OpenCode local error; inspect $opencode_output"
elif [[ "$new_job_seen" -eq 0 ]]; then
  fail_check "stall classification" "OpenCode exited without sending a new InferDeck request"
else
  write_check "stall classification" "request reached InferDeck and OpenCode exited"
fi

if [[ "$new_job_seen" -eq 1 && "$new_job_seen_at" -gt 2 ]]; then
  fail_check "OpenCode job visibility" "job appeared after ${new_job_seen_at}s, expected within 2s"
fi

after_json="$(curl -fsS --max-time 15 "$DASHBOARD_BASE_URL/api/status" 2>/dev/null || true)"
status_summary="$(printf '%s' "$after_json" | python3 -c 'import json, sys
before = sys.argv[1]
try:
    data = json.load(sys.stdin)
except Exception as exc:
    print(f"parse_failed={exc}")
    raise SystemExit(2)
obs = data.get("observability", {})
queue = data.get("queue", {})
job = obs.get("lastOpenCodeRequest") or {}
print("id=" + str(job.get("id", "")))
print("status=" + str(job.get("status", "")))
print("phase=" + str(job.get("phase", "")))
print("client=" + str(job.get("client", "")))
print("createdAt=" + str(job.get("createdAt", "")))
print("responseMode=" + str(job.get("responseMode", "")))
print("sseChunks=" + str(job.get("sseChunks", "")))
print("heartbeatChunks=" + str(job.get("heartbeatChunks", "")))
print("running=" + str(queue.get("running")))
if not job:
    raise SystemExit(3)
if before and job.get("id") == before:
    raise SystemExit(4)
if job.get("client") != "OpenCode":
    raise SystemExit(5)
if job.get("status") not in ("succeeded", "failed"):
    raise SystemExit(6)
if queue.get("running") != 0:
    raise SystemExit(7)' "$before_job"
)"
status_code=$?
printf '%s\n' "$status_summary"
if [[ "$status_code" -ne 0 ]]; then
  fail_check "dashboard OpenCode evidence" "failed"
fi

openwebui_control "after"
dashboard_backend_check "after"

if [[ "$FAILED" -ne 0 ]]; then
  exit 1
fi
printf 'Mac/OpenCode InferDeck smoke passed.\n'
