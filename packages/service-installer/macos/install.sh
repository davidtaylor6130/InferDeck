#!/bin/bash
set -e

cd "$(dirname "$0")/.."  # Go to repo root

echo "Installing r9700 AI Gateway as a launchd service..."

WORKING_DIR="$(pwd)"
USER_NAME=$(whoami)
SERVICES_DIR="${HOME}/Library/LaunchAgents"

mkdir -p "${SERVICES_DIR}"

cat > "${SERVICES_DIR}/com.r9700.ai-gateway.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.r9700.ai-gateway</string>
    <key>ProgramArguments</key>
    <array>
        <string>node</string>
        <string>apps/gateway-service/dist/main.js</string>
    </array>
    <key>WorkingDirectory</key>
    <string>${WORKING_DIR}</string>
    <key>EnvironmentVariables</key>
    <dict>
        <key>NODE_ENV</key>
        <string>production</string>
    </dict>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardErrorPath</key>
    <string>${WORKING_DIR}/data/logs/launchd-err.log</string>
    <key>StandardOutPath</key>
    <string>${WORKING_DIR}/data/logs/launchd-out.log</string>
</dict>
</plist>
EOF

echo "Loading service..."
launchctl load "${SERVICES_DIR}/com.r9700.ai-gateway.plist"

echo "Service installed and started!"
echo "Status: launchctl list com.r9700.ai-gateway"
echo "Logs: tail -f ${WORKING_DIR}/data/logs/launchd-out.log"
