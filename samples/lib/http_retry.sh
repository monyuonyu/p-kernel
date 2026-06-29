# ---------------------------------------------------------------------------
# samples/lib/http_retry.sh — a SOURCED, crown-neutral curl de-flake helper.
#
# THE LOAD-BEARING GUARD: retry ONLY a literal transient (curl HTTP code "000"
# OR curl's exit ∈ {7,28,52,56} = connection-refused / operation-timeout /
# connection-reset / send-recv-error). Bounded (<= 6 attempts, ~1s backoff,
# --max-time 8) and FAIL-CLOSED: after the bound it returns the LAST (000)
# result so the caller's assertion STILL fails — a genuine hang keeps its teeth.
#
# A DEFINITIVE answer is returned IMMEDIATELY on the FIRST try and is NEVER
# retried: any real HTTP code (200/403/409/500/…), i.e. curl exit 0 with a
# non-000 code, comes straight back. The de-flake absorbs ONLY no-response /
# alive-but-deaf (a node briefly unresponsive while it runs a DMN sleep-
# consolidation — by design: the mind is thinking). It NEVER masks a wrong
# value, a missing tag, or a real status code.
#
# WHY: on ubuntu-latest the UMP x86_64 ark-profile + i18n certs intermittently
# see curl code 000 because the p-kernel node is ALIVE but momentarily deaf
# while consolidating. That is transient, not a bug — so we retry the 000, and
# ONLY the 000.
#
# Sourced (not exec'd). POSIX-bash. Deps = curl + sleep only (no new deps).
# ---------------------------------------------------------------------------

HTTP_RETRY_MAX="${HTTP_RETRY_MAX:-6}"            # bounded attempts (<= 6)
HTTP_RETRY_BACKOFF="${HTTP_RETRY_BACKOFF:-1}"    # ~1s between attempts
HTTP_RETRY_MAXTIME="${HTTP_RETRY_MAXTIME:-8}"    # per-attempt --max-time (sec)

# curl000 <curl args...> — a drop-in for an inline `curl ...` call. It passes
# every argument straight through (preserving the EXACT stdout shape the call
# site expects: a bare `%{http_code}`, a `body\n%{http_code}`, a JSON body, …),
# and enforces a per-attempt --max-time (appended last, so it always bounds the
# call even if a site forgot one). Retries ONLY a transient; returns the first
# DEFINITIVE result immediately; FAILS CLOSED at the bound.
curl000() {
    local out rc code attempt=0 transient
    while :; do
        attempt=$((attempt + 1))
        out="$(curl "$@" --max-time "$HTTP_RETRY_MAXTIME")"
        rc=$?
        # The HTTP code (when present) is the LAST whitespace token of the
        # last non-empty line of the -w output. Absent (-w not used) -> empty.
        code="$(printf '%s' "$out" | tr -d '\r' | awk 'NF{c=$NF} END{print c}')"
        transient=0
        case "$rc" in
            7|28|52|56) transient=1 ;;                 # connection / timeout / reset / send-recv
            0) [ "$code" = "000" ] && transient=1 ;;   # exit 0 but alive-but-deaf (HTTP 000)
        esac
        # DEFINITIVE (transient=0) -> return NOW, never retried. Or the bound is
        # reached -> FAIL CLOSED: hand back the last (000/transient) result with
        # its rc so the caller's assertion evaluates it as-is and FAILS.
        if [ "$transient" -eq 0 ] || [ "$attempt" -ge "$HTTP_RETRY_MAX" ]; then
            printf '%s' "$out"
            return "$rc"
        fi
        sleep "$HTTP_RETRY_BACKOFF"
    done
}

# wait_http <port> [path] — a boot/readiness gate. Polls GET on the port until
# it answers with a non-000 HTTP code (the node's HTTP server is up and not
# deaf), bounded ~25s. Returns 0 the instant it answers, nonzero if it never
# does (the caller then fails + cleans up). Default path = /manifesto (cheap;
# every galaxy/ark node serves it).
wait_http() {  # <port> [path]
    local port="$1" path="${2:-/manifesto}" i=0 code
    while [ "$i" -lt 25 ]; do
        code="$(curl -s -o /dev/null -w '%{http_code}' --max-time "$HTTP_RETRY_MAXTIME" \
                "127.0.0.1:${port}${path}" 2>/dev/null)"
        [ -n "$code" ] && [ "$code" != "000" ] && return 0
        sleep 1
        i=$((i + 1))
    done
    return 1
}
