#!/bin/bash
#
# sel4-serial-logger.sh -- TCP log receiver for seL4 serial output
#
# Listens on a TCP port, writes incoming serial data to a log file
# with size-based rotation. Designed to receive from Xen hvm_serial=tcp:
#
# Usage: ./sel4-serial-logger.sh [port] [log_dir]
#
# Dependencies: socat
#
# SPDX-License-Identifier: BSD-2-Clause
#

set -euo pipefail

PORT="${1:-9514}"
LOG_DIR="${2:-/var/log/sel4}"
LOG_FILE="$LOG_DIR/serial.log"
MAX_SIZE=$((10 * 1024 * 1024))  # 10 MB per file
MAX_FILES=5                      # keep 5 rotated files

mkdir -p "$LOG_DIR"

rotate_log() {
    local size
    size=$(stat -c%s "$LOG_FILE" 2>/dev/null || echo 0)
    if [ "$size" -ge "$MAX_SIZE" ]; then
        # Rotate: serial.log.4 -> deleted, .3 -> .4, ... .0 -> .1, serial.log -> .0
        for i in $(seq $((MAX_FILES - 1)) -1 1); do
            [ -f "$LOG_FILE.$((i - 1))" ] && mv "$LOG_FILE.$((i - 1))" "$LOG_FILE.$i"
        done
        mv "$LOG_FILE" "$LOG_FILE.0"
        echo "[$(date -Iseconds)] Log rotated" > "$LOG_FILE"
    fi
}

echo "sel4-serial-logger: listening on port $PORT, logging to $LOG_FILE"
echo "Configure seL4 VM:  xe vm-param-set uuid=\$VM_UUID platform:hvm_serial=tcp:<this_ip>:$PORT"

while true; do
    # socat: accept one TCP connection, pipe to stdout
    # When connection drops, loop restarts (handles VM reboot)
    socat TCP-LISTEN:"$PORT",reuseaddr - 2>/dev/null | while IFS= read -r line; do
        echo "[$(date -Iseconds)] $line" >> "$LOG_FILE"
        rotate_log
    done
    echo "[$(date -Iseconds)] Connection closed, waiting for reconnect..." >> "$LOG_FILE"
    sleep 1
done
