#!/bin/bash
# tests/host/run_ark_crypto.sh — ark-entry.md 3b, stage 1 (C2 and C4 for RFC 8439):
# arch/common/ark_crypto.c against the RFC 8439 vectors of 2.4.2, 2.5.2, 2.6.2 and
# 2.8.2, appendix A.3 #1-#11 and A.5 (tests/host/ark_crypto_vectors.h, generated from the
# RFC text by gen_ark_crypto_vectors.py), plus the
# tamper test (a bit of the tag, AAD or ciphertext: -1, all zero). Plain is built
# with ASan and UBSan.
# "nc": -DARKC_NC_NO_PAD must redden [arkc-aead-seal], [arkc-aead-open] and [arkc-aead-a5] only,
# -DARKC_NC_TAG_NOCHECK must redden [arkc-open-tamper] only.
# Output: "[ark-crypto] ALL PASS" (exit 0) or RED (1); nc prints
# "[ark-crypto] NC RED as expected" (exit 0) or NC FAIL (1). ~3 s.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
MODE="${1:-plain}"
W="$(mktemp -d /tmp/arkc.XXXXXX)"
trap 'rm -rf "$W"' EXIT
CC="${CC:-cc}"
CF="-std=c11 -O1 -Wall -Wextra -Werror -I $ROOT/arch/common/include -I $HERE"
SRC="$HERE/ark_crypto_test.c $ROOT/arch/common/ark_crypto.c"

if [ "$MODE" != nc ]; then
    $CC $CF -fsanitize=address,undefined -fno-sanitize-recover=all $SRC -o "$W/plain" \
        || { echo "[ark-crypto] build FAILED"; exit 1; }
    "$W/plain" | tee "$W/plain.log"
    if grep -q 'SUMMARY: 0 FAIL' "$W/plain.log" && [ "$(grep -c '  PASS  ' "$W/plain.log")" = 10 ]; then
        echo "[ark-crypto] ALL PASS"; exit 0
    fi
    echo "[ark-crypto] RED"; exit 1
fi

bad=0
nc() {  # macro, the FAIL lines expected (sorted ids, space separated)
    $CC $CF -D"$1" $SRC -o "$W/$1" || { echo "[ark-crypto] nc build $1 FAILED"; bad=1; return; }
    "$W/$1" > "$W/$1.log"
    got="$(grep '  FAIL  ' "$W/$1.log" | sed 's/.*FAIL  \(\[[a-z0-9-]*\]\).*/\1/' | sort -u | tr '\n' ' ')"
    if [ "$got" = "$2" ]; then echo "[ark-crypto] nc $1 red on: $got"; else
        echo "[ark-crypto] nc $1 WRONG: red on '$got', want '$2'"; bad=1; fi
}
nc ARKC_NC_NO_PAD "[arkc-aead-a5] [arkc-aead-open] [arkc-aead-seal] "
nc ARKC_NC_TAG_NOCHECK "[arkc-open-tamper] "
if [ "$bad" = 0 ]; then echo "[ark-crypto] NC RED as expected"; exit 0; fi
echo "[ark-crypto] NC FAIL"; exit 1
