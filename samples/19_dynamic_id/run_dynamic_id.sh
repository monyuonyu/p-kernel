#!/usr/bin/env bash
#
# run_dynamic_id.sh — build the relay and verify dynamic node-id leasing.
#
# Exercises only the relay's lease behaviour (G6 first step). The kernel
# (net_relay.c) client side is a follow-up wave; here a Python pseudo-
# client speaks the v2 wire directly.
#
# PASS prints "[dynamic-id] PASS"; any failed assertion exits non-zero.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
relay_dir="$here/../../relay"

echo "== building relay =="
make -C "$relay_dir" relay >/dev/null
echo "[OK] relay built"

echo "== running dynamic-id lease test =="
exec python3 "$here/dynamic_id_test.py"
