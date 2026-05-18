# InferDeck

A production-ready monorepo for running and managing local AI workloads on single-GPU machines.
The gateway provides smart GPU scheduling, mode switching (AI/Gaming/Maintenance), and a React dashboard.

## C++ Gateway (v2.0)

The gateway service has been converted from Node/TypeScript to C++ 23 with:

- **Direct llama.cpp integration** — No external backend required
- **Vulkan GPU acceleration** — AMD-first, auto-detects mixed precision from GGUF
- **Strict OpenAI API compatibility** — `/v1/chat/completions`, `/v1/completions`, `/v1/models`, `/v1/embeddings`
- **SSE streaming** — OpenAI-compatible chunked output
- **Self-contained .exe** — No Node.js runtime needed

### Quick Start (C++ Gateway)

```bash
# Build
./scripts/build.sh Release

# Run
./build/inferdeck-gateway.exe

# With custom config
./build/inferdeck-gateway.exe -c config/gateway.yml
```

### C++ Gateway API

The C++ gateway exposes OpenAI-compatible endpoints:

- `POST /v1/chat/completions` — Chat completions (streaming + non-streaming)
- `POST /v1/completions` — Text completions (streaming + non-streaming)
- `GET /v1/models` — List available models
- `POST /v1/embeddings` — Create embeddings
- `GET /v1/health` — Health check
- `GET /inferdeck/metrics` — Inference metrics
- `GET /inferdeck/status` — Engine status

See `docs/api_reference.md` for full API documentation.

### Build & Deploy

See `docs/BUILD.md` for build instructions and `docs/DEPLOY.md` for deployment.

## Project Structure

```
InferDeck/
├── apps/
│   ├── gateway-service/   # Backend gateway service (Fastify, SQLite)
│   └── dashboard/         # React + Vite + Tailwind dashboard
├── config/                # Configuration files
│   ├── gateway.example.yaml
│   └── gateway.local.yaml
├── data/                  # Runtime data (SQLite DB, logs, job artifacts)
├── docs/                  # Architecture and API documentation
├── packages/
│   ├── gateway-core/      # Scheduler, resource lock manager
│   ├── backend-llama/     # llama.cpp client and types
│   ├── service-installer/ # Windows/Linux/macOS service installers
│   └── shared/            # Shared types and utilities
├── tests/                 # Test suites
├── pnpm-workspace.yaml    # PNPM workspace config
├── tsconfig.base.json     # Base TypeScript config
└── package.json           # Root workspaces definition
```

## Quick Start

```bash
# 1. Clone the repo
git clone <repo-url>
cd InferDeck

# 2. Install dependencies
pnpm install

# 3. Configure
cp config/gateway.example.yaml config/gateway.local.yaml
# Edit config/gateway.local.yaml with your settings

# 4. Start the gateway service
pnpm dev:gateway

# 5. Start the dashboard
pnpm dev:dashboard
```

## Configuration

Edit `config/gateway.local.yaml` to configure:

- `server.dashboardPort`: Dashboard UI port (default: 8721)
- `server.proxyPort`: llama.cpp-compatible gateway port (default: 11434)
- `database.path`: SQLite database location
- `backend.baseUrl`: Your llama.cpp backend URL

## Environment Variables

Copy `.env.example` to `.env` and set:

```bash
LLAMA_HOST=127.0.0.1:11435
LLAMA_MAX_QUEUE=512
LLAMA_MAX_LOADED_MODELS=1
R9700_GATEWAY_ADMIN_KEY=change-me-to-a-random-secret
R9700_LOG_LEVEL=info
R9700_DB_PATH=./data/gateway.sqlite
R9700_LOG_DIR=./data/logs
```

## Features

- **GPU Scheduling**: Queues and schedules GPU-heavy jobs, preventing conflicts
- **Mode Switching**: AI / Gaming / Maintenance modes for flexible control
- **llama.cpp Proxy**: Full llama.cpp API compatibility (chat, generate, embeddings)
- **OpenAI Compatibility**: Proxy OpenAI API endpoints to llama.cpp
- **React Dashboard**: Real-time monitoring and control
- **Job Queue**: Priority-based job management with lifecycle tracking
- **Health Monitoring**: Comprehensive health checks for all services
- **Service Installer**: Windows (WinSW), Linux (systemd), macOS (launchd) support

## Development

```bash
# Build all packages
pnpm build

# Run tests
pnpm test

# Type check
pnpm typecheck

# Lint
pnpm lint
```

## API Endpoints

- `/health` - Health check
- `/status` - Gateway status
- `/queue` - Job queue status
- `/jobs` - Job management
- `/models` - llama.cpp models
- `/services` - Service health
- `/modes` - Mode switching
- `/metrics` - System metrics

See `docs/api/` for detailed API documentation.
