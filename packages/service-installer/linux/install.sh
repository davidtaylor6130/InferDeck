#!/bin/bash
set -e

cd "$(dirname "$0")/.."  # Go to repo root

echo "Installing r9700 AI Gateway as a systemd service..."

SERVICE_DIR="/etc/systemd/system"
WORKING_DIR="$(pwd)"

cat > "${SERVICE_DIR}/r9700-ai-gateway.service" << EOF
[Unit]
Description=r9700 AI Gateway
After=network.target

[Service]
Type=simple
User=$(whoami)
WorkingDirectory=${WORKING_DIR}
ExecStart=node apps/gateway-service/dist/main.js
Restart=on-failure
RestartSec=10
RestartMaxAttempts=5
TimeoutStartSec=60
TimeoutStopSec=15
Environment=NODE_ENV=production
EnvironmentFile=${WORKING_DIR}/apps/gateway-service/.env

[Install]
WantedBy=multi-user.target
EOF

echo "Reloading systemd..."
sudo systemctl daemon-reload

echo "Starting service..."
sudo systemctl start r9700-ai-gateway

echo "Enabling on boot..."
sudo systemctl enable r9700-ai-gateway

echo "Service installed and started!"
echo "Status: sudo systemctl status r9700-ai-gateway"
echo "Logs: journalctl -u r9700-ai-gateway -f"
