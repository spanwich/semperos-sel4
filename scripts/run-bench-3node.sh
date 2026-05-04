#!/usr/bin/env bash
# run-bench-3node.sh — single 3-node QEMU boot of the spanning bench, with
# guaranteed log capture + JSON conversion. Intended to be called once per
# boot from a multi-boot sweep loop.
#
# Usage:
#   run-bench-3node.sh <config_tag> <run_number>
#
#   config_tag: short label that goes into the log/JSON filename
#               (e.g. "new" for current branch HEAD, "old" for the
#               pre-FPT-183 baseline). Does NOT change what is built —
#               you must build the right tree into
#               build-qemu-node{0,1,2}-virtio/ before invoking.
#   run_number: integer, used for filename uniqueness within a config.
#
# Outputs (under results/):
#   log_<config>_run<N>_<TS>.log
#   exp_unified_<config>_3node_virtio_run<N>_<TS>.json
#
# Why: the 2026-05-01 5+5 sweep saved logs for only 2 of 10 boots because
# the orchestrator did not consistently tee compose output to a file. With
# logs missing, [BENCH-FAIL]/[BENCH-PHASE] markers are unreachable and we
# can't diagnose runs like 'spanning_chain_revoke n_valid=2/5'. This
# wrapper makes the log a hard precondition of each boot.
set -euo pipefail

if [ $# -ne 2 ]; then
    echo "Usage: $0 <config_tag> <run_number>" >&2
    exit 2
fi

CONFIG="$1"
RUN_NUM="$2"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESULTS_DIR="$REPO_ROOT/results"
COMPOSE_FILE="$REPO_ROOT/docker/docker-compose-3node.yml"
PARSER="$REPO_ROOT/scripts/parse_bench_log.py"

[ -f "$COMPOSE_FILE" ] || { echo "ERROR: compose file not found: $COMPOSE_FILE" >&2; exit 1; }
[ -f "$PARSER" ] || { echo "ERROR: parser not found: $PARSER" >&2; exit 1; }
mkdir -p "$RESULTS_DIR"

TS="$(date -u +%Y%m%d_%H%M%S)"
LOG="$RESULTS_DIR/log_${CONFIG}_run${RUN_NUM}_${TS}.log"
JSON="$RESULTS_DIR/exp_unified_${CONFIG}_3node_virtio_run${RUN_NUM}_${TS}.json"

echo "[run-bench-3node] config=$CONFIG run=$RUN_NUM ts=$TS"
echo "[run-bench-3node] log  -> $LOG"
echo "[run-bench-3node] json -> $JSON"

# Make sure no stale containers from a prior boot are still around.
( cd "$REPO_ROOT/docker" && docker compose -f docker-compose-3node.yml down --remove-orphans >/dev/null 2>&1 ) || true

# Boot. --abort-on-container-exit makes the whole stack stop when node-a
# (which prints the bench output) exits, so the log file naturally ends
# at the right place.
( cd "$REPO_ROOT/docker" && \
  docker compose -f docker-compose-3node.yml up --abort-on-container-exit ) \
  > "$LOG" 2>&1 || COMPOSE_RC=$?
COMPOSE_RC="${COMPOSE_RC:-0}"

# Always try to clean up containers so a stuck boot does not block the next.
( cd "$REPO_ROOT/docker" && docker compose -f docker-compose-3node.yml down --remove-orphans >/dev/null 2>&1 ) || true

if [ ! -s "$LOG" ]; then
    echo "[run-bench-3node] ERROR: log file empty (compose_rc=$COMPOSE_RC)" >&2
    exit 1
fi

echo "[run-bench-3node] compose_rc=$COMPOSE_RC log_size=$(wc -c <"$LOG") bytes"

# Convert log -> JSON. Parser is tolerant of partial logs.
python3 "$PARSER" "$LOG" "$CONFIG" "$RUN_NUM" "$JSON"

echo "[run-bench-3node] done."
echo "$JSON"
