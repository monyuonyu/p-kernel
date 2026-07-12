#!/bin/bash
# ---------------------------------------------------------------------------
# run_consent_dom.sh — G3 cert: the ark-profile consent card must survive
#                       the first-run onboarding's DOM move (#ark ->
#                       #intromount), i.e. the commit-90feb043 regression.
#
# fable5's CI-coverage audit flagged this as gap G3: galaxy/consent PIXELS
# had zero automated coverage (only the fable5 HTTP/JSON data-plane was
# gated). This drives a real headless Chromium against
# arch/common/web/galaxy.html at a phone viewport (411 CSS px), invokes the
# SAME functions the real first-run onboarding uses, and asserts the
# consent card is actually styled (not fallen back to UA defaults) and does
# not overflow the viewport. See consent_dom_cert.py's header for the full
# mechanism + why each assertion is load-bearing for this exact bug.
#
# Requires: python3 + the `playwright` package with the chromium browser
# downloaded. If not already present this script installs both itself
# (network required) -- in CI, prefer doing that as its own cacheable step
# and let this script just run the cert; see the draft ci.yml job in this
# cert's landing report.
#
# Usage:
#   tests/web/run_consent_dom.sh [path-to-galaxy.html]
#     (defaults to the repo's committed arch/common/web/galaxy.html)
#
# Exit 0 = green (card healthy). Exit nonzero = red (the bug is back).
# ---------------------------------------------------------------------------
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
GALAXY_HTML="${1:-$REPO_ROOT/arch/common/web/galaxy.html}"

PYTHON="${PYTHON:-python3}"

if ! "$PYTHON" -c "import playwright" >/dev/null 2>&1; then
  echo "[run_consent_dom] playwright (python) not found -- installing ..." >&2
  "$PYTHON" -m pip install --quiet --break-system-packages playwright \
    || "$PYTHON" -m pip install --quiet playwright
  "$PYTHON" -m playwright install --with-deps chromium
fi

exec "$PYTHON" "$HERE/consent_dom_cert.py" "$GALAXY_HTML"
