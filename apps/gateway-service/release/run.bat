@echo off
echo Starting InferDeck Gateway...
echo.
echo Dashboard available at: http://ai.homelab.com:8721
echo Gateway API available at: http://ai.homelab.com:11434
echo.
inferdeck-gateway.exe --config config/gateway.local.yaml
pause
