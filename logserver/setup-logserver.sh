#!/bin/bash
#
# setup-logserver.sh -- Install sel4-serial-logger on a Linux VM
#
# Run this on the log server VM (any Debian/Ubuntu/Alpine).
#
# SPDX-License-Identifier: BSD-2-Clause
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Install socat if missing
if ! command -v socat &>/dev/null; then
    echo "Installing socat..."
    if command -v apt-get &>/dev/null; then
        sudo apt-get update && sudo apt-get install -y socat
    elif command -v apk &>/dev/null; then
        sudo apk add socat
    elif command -v yum &>/dev/null; then
        sudo yum install -y socat
    else
        echo "ERROR: Install socat manually" >&2
        exit 1
    fi
fi

# Install logger script
sudo mkdir -p /opt/sel4-logger /var/log/sel4
sudo cp "$SCRIPT_DIR/sel4-serial-logger.sh" /opt/sel4-logger/
sudo chmod +x /opt/sel4-logger/sel4-serial-logger.sh

# Install systemd service
sudo cp "$SCRIPT_DIR/sel4-serial-logger.service" /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now sel4-serial-logger

echo ""
echo "Log server running on port 9514"
echo "Logs: /var/log/sel4/serial.log"
echo ""
echo "Configure seL4 VM on XCP-ng host:"
echo "  xe vm-shutdown uuid=\$VM_UUID"
LOG_IP=$(hostname -I | awk '{print $1}')
echo "  xe vm-param-set uuid=\$VM_UUID platform:hvm_serial=tcp:${LOG_IP}:9514"
echo "  xe vm-start uuid=\$VM_UUID"
echo ""
echo "Read logs remotely:"
echo "  ssh $(whoami)@${LOG_IP} cat /var/log/sel4/serial.log"
