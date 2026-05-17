# Contributing to InferDeck

Thank you for your interest in InferDeck! This guide covers the basics of getting started.

## Development Setup

```bash
# Install dependencies
pnpm install

# Run type checking
pnpm typecheck

# Run tests
pnpm test

# Start gateway service (dev mode)
pnpm dev:gateway

# Start dashboard (dev mode)
pnpm dev:dashboard
```

## Project Structure

- `apps/gateway-service/` - Backend Fastify service
- `apps/dashboard/` - React frontend (Vite + Tailwind)
- `packages/gateway-core/` - Scheduler, resource lock manager, queue store
- `packages/backend-llama/` - llama.cpp client
- `packages/shared/` - Shared types and utilities
- `config/` - Configuration files
- `tests/` - Test suites

## Branch Naming

- `feature/description` - New features
- `fix/description` - Bug fixes
- `docs/description` - Documentation changes

## Commit Messages

Use conventional commits:

```
feat: add GPU thermal throttling support
fix: resolve queue deadlock on mode switch
docs: update architecture diagram
```

## Before Submitting

1. Run `pnpm typecheck` - no errors
2. Run `pnpm test` - all tests pass
3. Run `pnpm lint` - no warnings
4. Link any related issues in the PR body

## Reporting Issues

Use the provided issue template when creating a new issue. Include:

- Steps to reproduce
- Expected behavior
- Actual behavior
- Environment (OS, GPU, llama.cpp version)
