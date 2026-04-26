#!/usr/bin/env bash
# GAP-006 / GAP-013 / ITEM-018: true cross-process restart test.
#
# Runs the write phase of tests/restart_test.cpp in one process, then
# invokes the verify phase in a SEPARATE process that re-opens the same
# PMDK pool file. The write phase intentionally skips its destructor
# (via _Exit) so the verify phase has to recover undrained PTFO state
# through the pool's recovery scan.
#
# Requires `make PMDK=1`. Uses a tmpfs path under /dev/shm per
# DECISION-014; tmpfs is not a real persistence simulator but validates
# the logical recovery contract.
#
# Exit 0 on success; non-zero if any manifest expectation mismatched.

set -euo pipefail

cd "$(dirname "$0")/.."

BIN="bin/restart_test"
if [[ ! -x "$BIN" ]]; then
  echo "run_restart_test.sh: $BIN not built. Run 'make PMDK=1' first." >&2
  exit 2
fi

POOL="/dev/shm/f3tree_restart_$$.pool"
MANIFEST="/tmp/f3tree_restart_manifest_$$.txt"

cleanup() {
  rm -f "$POOL" "$MANIFEST"
}
trap cleanup EXIT

echo "== restart_test: write phase (pmdk-crash) =="
"$BIN" --phase write --manifest "$MANIFEST" --pool "$POOL" --pmdk-crash

echo "== restart_test: verify phase (fresh process) =="
"$BIN" --phase verify --manifest "$MANIFEST" --pool "$POOL"

echo "== restart_test: re-verify (idempotence) =="
"$BIN" --phase verify --manifest "$MANIFEST" --pool "$POOL"

echo "OK"
