#!/bin/bash
# tests/ci/link_only_cc.sh — a CC for tests/ci/shell_links.sh (BA-SHELL).
#
# Runs the real compiler ($LINK_ONLY_REAL_CC, default cc) with the same
# arguments. When the call LINKS (has -o and none of -c -S -E), it writes one
# line to $LINK_ONLY_LOG ("LINK ok <out>" or "LINK FAIL <out>") and, on success,
# replaces the program with a stub that exits 0 at once, so the test script
# that called us does not run its (minutes long) cert. Compile-only and
# preprocess calls pass through untouched.
real="${LINK_ONLY_REAL_CC:-cc}"
out=""; link=1; prev=""
for a in "$@"; do
    case "$a" in
        -c|-S|-E) link=0 ;;
    esac
    [ "$prev" = "-o" ] && out="$a"
    prev="$a"
done
"$real" "$@"
rc=$?
if [ "$link" = 1 ] && [ -n "$out" ]; then
    if [ "$rc" = 0 ]; then
        echo "LINK ok $out" >> "${LINK_ONLY_LOG:-/dev/null}"
        printf '#!/bin/sh\nexit 0\n' > "$out" && chmod +x "$out"
    else
        echo "LINK FAIL $out" >> "${LINK_ONLY_LOG:-/dev/null}"
    fi
fi
exit "$rc"
