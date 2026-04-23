#!/usr/bin/env bash
# run-qemu-3node-virtio.sh — launch three QEMU VMs connected via mcast hub,
# each with a VirtIO PCI NIC. For validating FPT-179 Phase 4 end-to-end.
#
# Usage:  run-qemu-3node-virtio.sh [duration_seconds]
#
# Default duration is 90s. Serial logs land in /tmp/node{0,1,2}-virtio.log.
# Trap Ctrl-C to kill all three QEMUs cleanly.
set -u

DURATION="${1:-90}"
REPO=/home/iamfo470/phd/camkes-vm-examples
CPU_FLAGS="qemu64,+rdrand,+fsgsbase,+xsave,+xsaveopt"
MCAST="230.0.0.100:12340"

for N in 0 1 2; do
    BD="$REPO/build-qemu-node${N}-virtio"
    if [ ! -f "$BD/images/kernel-x86_64-pc99" ]; then
        echo "ERROR: kernel not found for node${N} at $BD" >&2
        exit 1
    fi
done

PIDS=()
cleanup() {
    for P in "${PIDS[@]}"; do
        kill -TERM "$P" 2>/dev/null || true
    done
    sleep 1
    for P in "${PIDS[@]}"; do
        kill -KILL "$P" 2>/dev/null || true
    done
}
trap cleanup EXIT INT TERM

for N in 0 1 2; do
    BD="$REPO/build-qemu-node${N}-virtio"
    LOG="/tmp/node${N}-virtio.log"
    MAC="52:54:00:12:34:0${N}"
    echo "[hub] starting node${N} -> $LOG (mac=$MAC)"
    qemu-system-x86_64 \
        -machine q35 -cpu "$CPU_FLAGS" \
        -m 512 -nographic -serial "file:$LOG" \
        -no-reboot \
        -device virtio-net-pci,disable-legacy=on,disable-modern=off,netdev=net0,mac="$MAC" \
        -netdev "socket,id=net0,mcast=$MCAST,localaddr=127.0.0.1" \
        -object "filter-dump,id=fd${N},netdev=net0,file=/tmp/node${N}-virtio.pcap" \
        -kernel "$BD/images/kernel-x86_64-pc99" \
        -initrd "$BD/images/capdl-loader-image-x86_64-pc99" \
        >/dev/null 2>&1 &
    PIDS+=($!)
done

echo "[hub] all three nodes launched, PIDs: ${PIDS[*]}"
echo "[hub] running for ${DURATION}s..."
sleep "$DURATION"

echo "[hub] terminating..."
cleanup
trap - EXIT

echo "[hub] done. Logs: /tmp/node0-virtio.log /tmp/node1-virtio.log /tmp/node2-virtio.log"
