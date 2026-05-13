# README - r9700 AI Gateway

A production-ready monorepo for running and managing local AI workloads on single-GPU machines.
The gateway provides smart GPU scheduling, mode switching (AI/Gaming/Maintenance), and a React dashboard.

## Project Structure

```
R9700-AI-Gateway/
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
│   ├── backend-ollama/    # Ollama client and types
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
cd R9700-AI-Gateway

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
- `server.proxyPort`: Ollama-compatible gateway port (default: 11434)
- `database.path`: SQLite database location
- `ollama.baseUrl`: Your Ollama backend URL

## Environment Variables

Copy `.env.example` to `.env` and set:

```bash
OLLAMA_HOST=127.0.0.1:11435
OLLAMA_MAX_QUEUE=512
OLLAMA_MAX_LOADED_MODELS=1
R9700_GATEWAY_ADMIN_KEY=change-me-to-a-random-secret
R9700_LOG_LEVEL=info
R9700_DB_PATH=./data/gateway.sqlite
R9700_LOG_DIR=./data/logs
```

## Features

- **GPU Scheduling**: Queues and schedules GPU-heavy jobs, preventing conflicts
- **Mode Switching**: AI / Gaming / Maintenance modes for flexible control
- **Ollama Proxy**: Full Ollama API compatibility (chat, generate, embeddings)
- **OpenAI Compatibility**: Proxy OpenAI API endpoints to Ollama
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
- `/models` - Ollama models
- `/services` - Service health
- `/modes` - Mode switching
- `/metrics` - System metrics

See `docs/api/` for detailed API documentation.
