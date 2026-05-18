# InferDeck Gateway — Deployment Guide

## Windows Deployment

### Option 1: Self-Extracting Installer (Recommended)

Run the generated `.exe` installer:

```powershell
# Execute the installer
.\inferdeck-gateway-0.1.0-windows-x64.exe

# This will:
# 1. Extract to LOCALAPPDATA\InferDeck
# 2. Create desktop shortcut
# 3. Create start menu entry
# 4. Copy certs, config, README
```

### Option 2: Manual Extraction

```powershell
# Extract the installer manually
Expand-Archive -Path inferdeck-gateway-0.1.0-windows-x64.exe -DestinationPath C:\InferDeck

# Navigate to install directory
cd C:\InferDeck

# Start the gateway
.\inferdeck-gateway.exe
```

## Configuration for Production

1. Copy the example config:

```powershell
copy config\gateway.example.yml config\gateway.yml
```

2. Edit `config\gateway.yml`:

```yaml
server:
  host: "0.0.0.0"    # Bind to all interfaces
  port: 8080
  tls:
    enabled: true
    cert_file: "certs/server.crt"
    key_file: "certs/server.key"

model:
  path: "models/your-model.gguf"
  precision: "auto"

gpu:
  device_id: 0

queue:
  worker_threads: 4
  max_queue_size: 100

logging:
  level: "info"
  file: "logs/gateway.log"
```

3. Place your GGUF model file in `models/` directory.

## Starting the Gateway

```powershell
# Default (uses config/gateway.yml)
.\inferdeck-gateway.exe

# Custom config
.\inferdeck-gateway.exe -c config\gateway.yml

# Help
.\inferdeck-gateway.exe -h
```

## Stopping the Gateway

Press `Ctrl+C` to gracefully shut down. The server will:
1. Finish processing queued requests
2. Unload the model from GPU
3. Close the HTTP listener
4. Exit cleanly

## Verifying the Service

```powershell
# Check health
curl -k https://localhost:8080/v1/health

# List models
curl -k https://localhost:8080/v1/models

# Test chat
curl -k https://localhost:8080/v1/chat/completions `
  -H "Content-Type: application/json" `
  -d "{\"messages\":[{\"role\":\"user\",\"content\":\"Hello\"}]}"

# Check metrics
curl -k https://localhost:8080/inferdeck/metrics
```

## Firewall Configuration

If the gateway is not accessible from other machines, add a Windows Firewall rule:

```powershell
# Allow inbound traffic on port 8080
New-NetFirewallRule `
  -DisplayName "InferDeck Gateway" `
  -Direction Inbound `
  -Protocol TCP `
  -LocalPort 8080 `
  -Action Allow
```

## Logging

Logs are written to `logs/gateway.log` (configured in gateway.yml).

Log levels: `trace`, `debug`, `info`, `warn`, `error`, `fatal`.

## Troubleshooting

### Gateway won't start

1. Check config: `.\inferdeck-gateway.exe -h`
2. Check logs: `cat logs/gateway.log`
3. Verify model path exists
4. Verify TLS certs exist in `certs/`

### "Vulkan not found" error

1. Install Vulkan SDK from [vulkan.lunarg.com](https://vulkan.lunarg.com/)
2. Update GPU drivers
3. Verify GPU supports Vulkan 1.3+

### "Model failed to load" error

1. Verify GGUF file is valid (not corrupted)
2. Check VRAM availability
3. Verify model path in config is correct
4. Check GPU compatibility

### TLS handshake failures

1. Replace self-signed certs with CA-issued certificates
2. Add self-signed cert to system trust store
3. Verify `cert_file` and `key_file` paths in config

### High GPU memory usage

1. Adjust `gpu.mem_alloc_percent` in config (default: 95)
2. Lower `context_size` if needed
3. Use a smaller quantized model

## Uninstallation

1. Delete `LOCALAPPDATA\InferDeck`
2. Remove desktop/start menu shortcuts
3. Delete `logs/` if present

## Monitoring

### Health Check

```bash
curl -k https://localhost:8080/v1/health
```

### Metrics

```bash
curl -k https://localhost:8080/inferdeck/metrics
```

### Status

```bash
curl -k https://localhost:8080/inferdeck/status
```

### Log Monitoring (PowerShell)

```powershell
Get-Content logs\gateway.log -Wait -Tail 50
```
