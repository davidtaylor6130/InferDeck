# r9700 AI Gateway - Changelog

## [1.0.0] - 2025-05-10

### Added
- Full React + Vite + Tailwind dashboard with live data
- Dashboard tabs: Dashboard, Jobs, Models, Services, Modes
- Complete job management UI with table, status badges, and cancel action
- Model management UI with pull, load, and delete operations
- Service health cards with real-time status indicators
- Mode switcher with three modes: AI, Gaming, Maintenance
- SSE events route for real-time queue updates
- API documentation at docs/api/overview.md
- Gateway-core build configuration (tsc output to dist/)
- Proper TypeScript builds for all packages
- .gitkeep files for data/logs and data/job-artifacts directories
- Hardware metrics type definitions in shared package

### Fixed
- events.routes.ts: Fixed undefined `ctx` — now retrieves queueStore from app context
- jobs.repo.ts: Fixed broken SQL — replaced all string interpolation with positional `?` parameters, eliminating SQL injection vulnerability
- jobs.repo.ts: Fixed unclosed template literal in listStmt (now removed, using inline queries instead)
- jobs.repo.ts: Fixed `:daysDays` parameter typo in cleanupOldStmt
- jobs.repo.ts: Fixed reprioritize() and delete() endpoints to use parameterized queries
- models.routes.ts: Fixed /models/load to use `/api/show` instead of `/api/pull`
- models.routes.ts: Fixed /models/unload to use `/api/pull` with `keep_alive: "0"` instead of `/api/generate`
- PolicyEngine.ts: Fixed `linked` to `locked` in canExecuteGpuJob return value
- apiTypes.ts: Removed `"noop_test"` and `| string` from JobType union — now fully typed
- jobTypes.ts: Removed `| string` from ClientOrigin union — now fully typed
- PolicyEngine.ts: Fixed `_mode` private field with proper getter/setter for mode property

### Changed
- `.env.example`: Fixed `Ollama_HOST` to `OLLAMA_HOST` (consistent casing)
- schema.ts: Changed `publicBaseUrl` default from `http://ai.homelab.com:8721` to `http://localhost:8721`
- schema.ts: Changed `executable` default from Windows path to `ollama` (cross-platform via PATH)
- schema.ts: Changed `hardware.provider` default from `"null"` to `"none"`
- gateway.example.yaml: Applied same defaults as schema.ts
- App.tsx: Removed unused `react-router-dom` imports (Link, Routes, Route, useLocation)
- dashboard package.json: Removed unused `react-router-dom` dependency
- gateway-service package.json: Removed 9 unused dependencies (@anthropic-ai/sdk, @fastify/cookie, @fastify/rate-limit, @fastify/swagger, @fastify/swagger-ui, http-proxy-middleware, winston, winston-daily-rotate-file, dotenv)
- Deleted dead files: containers/.gitkeep, hooks/.gitkeep, pages/JobsList.tsx, pages/Dashboard.tsx
- LoadConfig: Added R9700_API_KEY_ENV environment override support
- Dashboard UI: Added footer, better error states, empty states, and cross-browser compatibility
- CHANGELOG: Updated from `[Unreleased]` to versioned `[1.0.0]`

### Removed
- Dead placeholder components from App.tsx (JobsList, ModelsCard, ServicesCard, ModesCard stubs)
- Dead dashboard pages (JobsList.tsx, Dashboard.tsx)
- Unused Fastify dependencies from gateway-service package
