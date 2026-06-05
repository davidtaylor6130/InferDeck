#!/usr/bin/env bash
# InferDeck parity runner (bash shim around the PowerShell version).
#
# This is a thin wrapper. On Windows, prefer running `run.ps1` directly.
# On Linux/macOS, install PowerShell Core: https://aka.ms/powershell
# and it will be invoked via `pwsh`.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if ! command -v pwsh >/dev/null 2>&1; then
  echo "pwsh is required. Install PowerShell Core: https://aka.ms/powershell" >&2
  exit 2
fi

exec pwsh -NoProfile -File "$SCRIPT_DIR/run.ps1" "$@"
