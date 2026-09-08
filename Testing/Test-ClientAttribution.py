#!/usr/bin/env python3
"""Isolated real-HTTP owner attribution probe. Never targets the production service."""
import argparse, json, os, secrets, shutil, socket, subprocess, sys, tempfile, time, urllib.error, urllib.request
from pathlib import Path

def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0)); return s.getsockname()[1]

def call(base, path, method="GET", body=None, token=None, request_id=None, timeout=30):
    headers = {"Content-Type": "application/json"}
    if token: headers["Authorization"] = "Bearer " + token
    if request_id: headers["X-Request-Id"] = request_id
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(base + path, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read().decode()) if r.readable() else {}
    except urllib.error.HTTPError as e:
        raw = e.read().decode(errors="replace")
        try: payload = json.loads(raw)
        except json.JSONDecodeError: payload = {"raw": raw}
        return e.code, payload

def wait_ready(base, proc, token):
    for _ in range(120):
        if proc.poll() is not None: raise RuntimeError("gateway exited during startup")
        try:
            status, _ = call(base, "/api/inferdeck/v1/health", token=token, timeout=1)
            if status == 200:
                if os.name == "nt":
                    port = int(base.rsplit(":", 1)[1])
                    command = f"(Get-NetTCPConnection -LocalPort {port} -State Listen -ErrorAction Stop).OwningProcess"
                    result = subprocess.run(["powershell", "-NoProfile", "-Command", command], capture_output=True, text=True, timeout=10, check=True)
                    owners = {int(value) for value in result.stdout.split()}
                    if owners != {proc.pid}: raise RuntimeError("listener is not the isolated gateway child")
                return
        except Exception: pass
        time.sleep(.25)
    raise RuntimeError("gateway listener did not become ready")

def request_row(rows, request_id):
    matches = [r for r in rows if r.get("request_id") == request_id]
    if len(matches) != 1: raise AssertionError(f"expected one row for {request_id}, got {len(matches)}")
    return matches[0]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gateway-exe", default="build/bin/Release/inferdeck-gateway.exe")
    ap.add_argument("--model-path", default=r"C:\InferDeck\models\Qwen\Qwen2.5-0.5B-Instruct-GGUF\qwen2.5-0.5b-instruct-q4_k_m.gguf")
    ap.add_argument("--keep-artifacts", action="store_true")
    args = ap.parse_args()
    exe = Path(args.gateway_exe).resolve(); model = Path(args.model_path).resolve()
    if not exe.is_file(): raise FileNotFoundError(exe)
    if not model.is_file(): raise FileNotFoundError(model)
    work = Path(tempfile.mkdtemp(prefix="inferdeck-client-attribution-")); proc = None
    port = free_port(); control = secrets.token_hex(32); base = f"http://127.0.0.1:{port}"
    try:
        cfg = work / "gateway.yml"
        mp = str(model).replace("\\", "/")
        text = f'''schema_version: 1
server:
  host: "127.0.0.1"
  port: {port}
logging:
  level: "info"
  file: "{str(work / "gateway.log").replace(chr(92), "/")}"
auth:
  required: false
  token: ""
  api_keys_db: "{str(work / "api-keys.db").replace(chr(92), "/")}"
control:
  allow_remote: false
  allow_data_plane_token: false
  token: "{control}"
cors:
  origins: ["*"]
state:
  file: "{str(work / "state.json").replace(chr(92), "/")}"
default_model: ""
observability:
  stats_db: "{str(work / "stats.db").replace(chr(92), "/")}"
  adlx_helper: ""
  telemetry_poll_ms: 1000
gateway:
  auto_swap: true
  n_batch: 128
  n_ubatch: 128
  use_mmap: true
  use_mlock: false
  n_gpu_layers: 0
  flash_attn: "off"
  kv_offload: false
  op_offload: false
  cache_type_k: "f32"
  cache_type_v: "f32"
model_aliases:
  - name: "alias-a"
    target: "target-a"
  - name: "alias-b"
    target: "target-b"
model_registry:
  - name: "target-a"
    family: "qwen2.5"
    gguf_path: "{mp}"
    n_slots: 1
    context_size: 2048
    n_gpu_layers: 0
    has_vision: false
  - name: "target-b"
    family: "qwen2.5"
    gguf_path: "{str(work / "missing.gguf").replace(chr(92), "/")}"
    n_slots: 1
    context_size: 2048
    n_gpu_layers: 0
    has_vision: false
'''
        cfg.write_text(text, encoding="utf-8")
        env = os.environ.copy(); env["GGML_VK_VISIBLE_DEVICES"] = " "
        proc = subprocess.Popen([str(exe), "-c", str(cfg)], cwd=str(work), env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_ready(base, proc, control)
        status, ka = call(base, "/api/inferdeck/v1/api-keys", "POST", {"name":"client-a"}, control)
        if status != 201: raise AssertionError(f"key A creation failed: {status}")
        status, kb = call(base, "/api/inferdeck/v1/api-keys", "POST", {"name":"client-b"}, control)
        if status != 201: raise AssertionError(f"key B creation failed: {status}")
        if not ka.get("id") or not ka.get("key") or not kb.get("id") or not kb.get("key"): raise AssertionError("key response missing id/key")
        success_id, failure_id = "client-attribution-success", "client-attribution-failure"
        status, _ = call(base, "/v1/chat/completions", "POST", {"model":"alias-a","messages":[{"role":"user","content":"Reply OK"}],"max_tokens":4}, ka["key"], success_id)
        if status != 200: raise AssertionError(f"success request failed: {status}")
        status, _ = call(base, "/v1/chat/completions", "POST", {"model":"alias-b","messages":[{"role":"user","content":"Reply OK"}],"max_tokens":4}, kb["key"], failure_id)
        if status not in (400, 404, 500, 503): raise AssertionError(f"failure request returned unexpected status {status}")
        _, hist = call(base, "/api/inferdeck/v1/stats/history?limit=100", token=control)
        ok = request_row(hist["requests"], success_id)
        bad = request_row(hist["requests"], failure_id)
        for row, key, alias, resolved, rid in ((ok,ka,"alias-a","target-a",success_id),(bad,kb,"alias-b","target-b",failure_id)):
            assert row["request_id"] == rid and row["api_key_id"] == key["id"] and row["api_key_name"] == key["name"]
            assert row["model"] == alias and row["resolved_model"] == resolved
        swaps = [s for s in hist["swaps"] if s.get("requested_model") == "alias-a"]
        if len(swaps) != 1: raise AssertionError(f"expected attributed auto-swap, got {len(swaps)}")
        swap = swaps[0]; assert swap["request_id"] == success_id and swap["api_key_id"] == ka["id"] and swap["api_key_name"] == ka["name"]
        proc.terminate(); proc.wait(15)
        proc = subprocess.Popen([str(exe), "-c", str(cfg)], cwd=str(work), env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        wait_ready(base, proc, control)
        _, after = call(base, "/api/inferdeck/v1/stats/history?limit=100", token=control)
        for before_row in (ok, bad):
            restored = request_row(after["requests"], before_row["request_id"])
            for field in ("request_id", "api_key_id", "api_key_name", "model", "resolved_model"):
                if restored[field] != before_row[field]: raise AssertionError(f"restart changed {field}")
        restored_swaps = [row for row in after["swaps"] if row.get("request_id") == success_id]
        if len(restored_swaps) != 1: raise AssertionError("restart lost attributed swap")
        for field in ("api_key_id", "api_key_name", "requested_model"):
            if restored_swaps[0][field] != swap[field]: raise AssertionError(f"restart changed swap {field}")
        visible = json.dumps(after) + (work / "gateway.log").read_text(encoding="utf-8", errors="replace")
        if any(key["key"] in visible for key in (ka, kb)): raise AssertionError("raw API key leaked into history or logs")
        status, _ = call(base, "/api/inferdeck/v1/swap/status", token=control)
        if status != 200: raise AssertionError("swap status failed after restart")
        print(json.dumps({"port":port,"request_ids":[success_id,failure_id],"key_ids":[ka["id"],kb["id"]],"swap_request_id":swap["request_id"],"restart_history":True}))
    finally:
        if proc and proc.poll() is None: proc.kill(); proc.wait(10)
        if not args.keep_artifacts:
            resolved = work.resolve()
            if resolved.parent != Path(tempfile.gettempdir()).resolve() or not resolved.name.startswith("inferdeck-client-attribution-"):
                raise RuntimeError("refusing cleanup outside the owned temporary directory")
            shutil.rmtree(resolved)

if __name__ == "__main__":
    try: main()
    except Exception as e:
        print(f"FAILED: {e}", file=sys.stderr); sys.exit(1)