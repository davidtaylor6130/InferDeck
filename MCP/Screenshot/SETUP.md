# MCP Screenshot Analyzer — Mac Setup

## 1. Copy the script to your Mac

The repo lives in your workspace. Copy (or git clone) the whole `MCP/` folder.

```
/path/to/your/workspace/
└── MCP/
    └── Screenshot/
        ├── screenshot_analyzer.py   ← the server
        ├── Captures/                ← screenshots saved here (auto-created)
        └── SETUP.md                 ← this file
```

## 2. Install dependencies

```bash
pip install fastmcp playwright httpx Pillow
playwright install chromium
```

`Pillow` is optional but strongly recommended — it enables JPEG output (smaller files) and auto-resizing of oversized screenshots.

## 3. Test the tool standalone

Before hooking into OpenCode, verify the full pipeline works:

```bash
python3 /path/to/MCP/Screenshot/screenshot_analyzer.py \
  --test https://example.com "What color is the background?"
```

This runs through all 4 steps:
1. Pings gateway — confirms Windows PC is reachable and model exists
2. Takes Playwright screenshot — confirms browser works
3. Saves + resizes — confirms disk access
4. Sends to InferDeck — confirms image analysis works

If this passes, OpenCode integration will work.

## 4. Add to Mac opencode.json

Edit `~/.config/opencode/opencode.json`, add to `mcpServers`:

```json
"screenshot-analyzer": {
  "type": "local",
  "command": ["python3", "/absolute/path/to/MCP/Screenshot/screenshot_analyzer.py"],
  "enabled": true
}
```

Make sure to use the **absolute path** to `screenshot_analyzer.py` on your Mac.

## 5. Restart OpenCode

Then ask: *"Take a screenshot of https://example.com and describe what you see"*

## Environment variables (optional)

| Variable | Default | Description |
|---|---|---|
| `INFERDECK_URL` | `http://192.168.0.168:11434/api/chat` | Gateway endpoint |
| `INFERDECK_MODEL` | `qwen3.6-35b-a3b` | Model name on gateway |
| `INFERDECK_TIMEOUT` | `120` | Gateway timeout in seconds |

Example override:

```json
"screenshot-analyzer": {
  "type": "local",
  "command": ["python3", "/path/to/screenshot_analyzer.py"],
  "env": {
    "INFERDECK_TIMEOUT": "300"
  },
  "enabled": true
}
```

## Troubleshooting

### "Cannot reach InferDeck gateway"
- Mac needs LAN access to `192.168.0.168:11434`
- Test with: `curl http://192.168.0.168:11434/api/tags` from the Mac

### "Playwright timed out"
- Some sites block headless browsers
- The URL might be incorrect (needs `https://` prefix)

### Gateway returns gibberish
- Run the test mode first to isolate whether it's the MCP pipeline or the gateway
