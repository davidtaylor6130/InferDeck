InferDeck Gateway - Windows Release
====================================

Run: inferdeck-gateway.exe --config config/gateway.local.yaml

Dashboard: http://ai.homelab.com:8721
Gateway API: http://ai.homelab.com:11434
OpenAI API: http://ai.homelab.com:11434/v1
Real Ollama backend: http://127.0.0.1:11435

====================================
OpenCode Setup
====================================

Copy opencode.json to your OpenCode config folder:

  Copy: opencode.json
  To:  %USERPROFILE%\.config\opencode\opencode.json

Then restart OpenCode and run:
  opencode /models

Select "homelab" - it'll use any model you have in Ollama!
