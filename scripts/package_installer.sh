#!/bin/bash
# package_installer.sh — Create self-extracting Windows installer
# Usage: ./scripts/package_installer.sh [version]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_ROOT}/build"
VERSION="${1:-0.1.0}"
INSTALLER_NAME="inferdeck-gateway-${VERSION}-windows-x64.exe"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# Check if build exists
if [ ! -d "${BUILD_DIR}/inferdeck-Release" ]; then
    log_error "Build not found. Run ./scripts/build.sh first."
    exit 1
fi

log_info "Creating installer: ${INSTALLER_NAME}"

# Create staging directory
STAGING_DIR="${BUILD_DIR}/staging"
rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR"

# Copy packaged files
cp -r "${BUILD_DIR}/inferdeck-Release/"* "$STAGING_DIR/"

# Create self-extracting archive with PowerShell
PS1_SCRIPT="${BUILD_DIR}/create_installer.ps1"
cat > "$PS1_SCRIPT" << 'PSEOF'
# Self-extracting installer script for InferDeck Gateway
$ErrorActionPreference = "Stop"
$PSScriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$InstallDir = Join-Path $env:LOCALAPPDATA "InferDeck"

Write-Host "Installing InferDeck Gateway to $InstallDir..."

# Create install directory
if (!(Test-Path $InstallDir)) {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
}

# Extract files
$Files = Get-ChildItem -Path $PSScriptRoot -Recurse -File
foreach ($File in $Files) {
    $RelativePath = $File.FullName.Substring($PSScriptRoot.Length + 1)
    $DestPath = Join-Path $InstallDir $RelativePath
    $DestDir = Split-Path $DestPath -Parent
    if (!(Test-Path $DestDir)) {
        New-Item -ItemType Directory -Force -Path $DestDir | Out-Null
    }
    Copy-Item $File.FullName $DestPath -Force
}

# Create desktop shortcut
$WshShell = New-Object -ComObject WScript.Shell
$Shortcut = $WshShell.CreateShortcut("$env:DESKTOP\InferDeck Gateway.lnk")
$Shortcut.TargetPath = Join-Path $InstallDir "inferdeck-gateway.exe"
$Shortcut.Description = "InferDeck Gateway - Local LLM Server"
$Shortcut.Save()

# Create start menu entry
$StartMenu = [environment]::getfolderpath("startmenu")
$InferDeckDir = Join-Path $StartMenu "InferDeck"
if (!(Test-Path $InferDeckDir)) {
    New-Item -ItemType Directory -Force -Path $InferDeckDir | Out-Null
}
$Shortcut = $WshShell.CreateShortcut("$InferDeckDir\InferDeck Gateway.lnk")
$Shortcut.TargetPath = Join-Path $InstallDir "inferdeck-gateway.exe"
$Shortcut.Description = "InferDeck Gateway - Local LLM Server"
$Shortcut.Save()

Write-Host "Installation complete!"
Write-Host "Run inferdeck-gateway.exe to start the server."
PSEOF

# Create the self-extracting installer
# Combine PowerShell script + payload
PAYLOAD="${BUILD_DIR}/inferdeck-payload.zip"
cd "$STAGING_DIR"
zip -r "$PAYLOAD" . 2>/dev/null || 7z a "$PAYLOAD" . 2>/dev/null || tar -czf "$PAYLOAD" .
cd "$PROJECT_ROOT"

# Create final installer (concatenate PS script + payload)
# Windows can execute this as a self-extracting archive
cp "$PS1_SCRIPT" "${BUILD_DIR}/${INSTALLER_NAME}.ps1"
cat "${BUILD_DIR}/${INSTALLER_NAME}.ps1" "$PAYLOAD" > "${BUILD_DIR}/${INSTALLER_NAME}"

# Clean up
rm -f "$PAYLOAD" "$PS1_SCRIPT"
rm -rf "$STAGING_DIR"

log_info "Installer created: ${BUILD_DIR}/${INSTALLER_NAME}"
log_info "Run with: ${INSTALLER_NAME}.ps1 -ExecutionPolicy Bypass"
