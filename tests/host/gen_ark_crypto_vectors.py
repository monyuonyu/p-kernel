#!/usr/bin/env python3
"""gen_vectors.py <rfc8439.txt> -> C header on stdout (tests/host/ark_crypto_vectors.h).
Each vector is found by its label line (1-based line number checked against the label text),
parsed from the RFC's own layout (a colon list, possibly wrapped mid-byte, or an offset dump),
and its length asserted. An auditor reruns this and diffs the header."""
import re, sys

L = open(sys.argv[1], encoding="ascii", errors="replace").read().split("\n")

def colon(n, label, size):
    line = L[n - 1]
    assert label in line, (n, label, line)
    s = line.split(label, 1)[1]
    i = n
    while True:                     # a wrapped list: the next lines until a blank one
        t = L[i].strip()
        if not t or not re.fullmatch(r"[0-9a-f:.()]+", t):
            break
        s += t; i += 1
    s = re.sub(r"[\s().=]", "", s).lstrip(":")
    out = bytes(int(x, 16) for x in s.split(":") if x)
    assert len(out) == size, (label, len(out), size)
    return out

DUMP = re.compile(r"^\s*(\d{3})  (.*)$")
def dump(n, label, size):
    assert L[n - 1].strip().startswith(label), (n, label, L[n - 1])
    out, i, want = bytearray(), n, 0
    while i < len(L):
        m = DUMP.match(L[i])
        if not m:
            if L[i].strip() == "" and out and len(out) < size:   # a page break inside a dump
                i += 1; continue
            if out:
                break
            i += 1; continue
        if L[i].strip().startswith(("Nir & Langley", "RFC 8439")):
            i += 1; continue
        assert int(m.group(1)) == want, (label, m.group(1), want)
        toks = m.group(2)[:47].split()
        assert all(re.fullmatch(r"[0-9a-f]{2}", t) for t in toks), (label, toks)
        out += bytes(int(t, 16) for t in toks)
        want += 16
        i += 1
        if len(out) >= size:
            break
    assert len(out) == size, (label, len(out), size)
    return bytes(out)

V = [
    ("CC_KEY",    colon(652, "Key = ", 32)),
    ("CC_NONCE",  colon(655, "Nonce = ", 12)),
    ("CC_PT",     dump(679, "Plaintext Sunscreen:", 114)),
    ("CC_CT",     dump(745, "Ciphertext Sunscreen:", 114)),
    ("P1305_KEY", colon(908, "Key Material: ", 32)),
    ("P1305_MSG", dump(922, "Message to be Authenticated:", 34)),
    ("P1305_TAG", colon(973, "Tag: ", 16)),
    ("KG_KEY",    dump(1038, "Key:", 32)),
    ("KG_NONCE",  dump(1042, "Nonce:", 12)),
    ("KG_OUT",    dump(1057, "Output bytes:", 32)),
    ("AE_PT",     dump(1263, "Plaintext:", 114)),
    ("AE_AAD",    dump(1273, "AAD:", 12)),
    ("AE_KEY",    dump(1276, "Key:", 32)),
    ("AE_IV",     dump(1280, "IV:", 8)),
    ("AE_FIXED",  dump(1283, "32-bit fixed-common part:", 4)),
    ("AE_OTK",    dump(1307, "Poly1305 Key:", 32)),
    ("AE_CT",     dump(1324, "Ciphertext:", 114)),
    ("AE_TAG",    colon(1366, "Tag:", 16)),
]

# Appendix A.3: the Poly1305 vectors #1-#11 (#5-#11 are the reduction edge cases),
# from "A.3." to "A.4.". #1-#4 are offset dumps, #5-#11 rows of upper-case hex.
def a3():
    s = next(i for i, l in enumerate(L) if l.startswith("A.3.  Poly1305"))
    e = next(i for i, l in enumerate(L) if l.startswith("A.4.  Poly1305"))
    out, cur, lab, k = [], None, None, 0
    ROW = re.compile(r"^\s*((?:[0-9A-F]{2} ){15}[0-9A-F]{2})\s*$")
    for i in range(s, e):
        l = L[i]
        m = re.match(r"^\s*Test Vector #(\d+)", l)
        if m:
            k += 1
            assert int(m.group(1)) == k, (i, l)
            cur = {}; out.append(cur); lab = None; continue
        if cur is None:
            continue
        t = l.strip()
        if t in ("One-time Poly1305 Key:", "R:", "S:"):
            lab = {"One-time Poly1305 Key:": "key", "R:": "r", "S:": "s"}[t]; cur.setdefault(lab, b""); continue
        if t in ("Text to MAC:", "data:"):
            lab = "msg"; cur.setdefault(lab, b""); continue
        if t in ("Tag:", "tag:"):
            lab = "tag"; cur.setdefault(lab, b""); continue
        m = DUMP.match(l)
        if m and lab and not t.startswith(("Nir & Langley", "RFC 8439")):
            assert int(m.group(1)) == len(cur[lab]), (i, lab, m.group(1), len(cur[lab]))
            toks = m.group(2)[:47].split()
            assert all(re.fullmatch(r"[0-9a-f]{2}", x) for x in toks), (i, toks)
            cur[lab] += bytes(int(x, 16) for x in toks); continue
        m = ROW.match(l)
        if m and lab:
            cur[lab] += bytes(int(x, 16) for x in m.group(1).split()); continue
    assert len(out) == 11, len(out)
    res = []
    for n, v in enumerate(out, 1):
        key = v["key"] if "key" in v else v["r"] + v["s"]
        assert len(key) == 32 and len(v["tag"]) == 16 and len(v["msg"]) > 0, (n, len(key), len(v["tag"]))
        res.append((n, key, v["msg"], v["tag"]))
    return res

A3 = a3()
assert [len(m) for _, _, m, _ in A3] == [64, 375, 375, 127, 16, 16, 48, 48, 16, 64, 48], \
    [len(m) for _, _, m, _ in A3]
for n, key, msg, tag in A3:
    V += [("A3_KEY_%d" % n, key), ("A3_MSG_%d" % n, msg), ("A3_TAG_%d" % n, tag)]

print("/* tests/host/ark_crypto_vectors.h: RFC 8439 test vectors, generated from the RFC text")
print(" * by tests/host/gen_ark_crypto_vectors.py <rfc8439.txt> (sections 2.4.2, 2.5.2, 2.6.2,")
print(" * 2.8.2 and appendix A.3 #1-#11). Do not edit: rerun and diff. */")
for name, b in V:
    print("static const unsigned char %s[%d] = {" % (name, len(b)))
    for k in range(0, len(b), 12):
        print("    " + ", ".join("0x%02x" % x for x in b[k:k + 12]) + ",")
    print("};")
print("static const struct { const unsigned char *key, *msg, *tag; unsigned len; } A3[%d] = {" % len(A3))
for n, _, msg, _ in A3:
    print("    { A3_KEY_%d, A3_MSG_%d, A3_TAG_%d, %d }," % (n, n, n, len(msg)))
print("};")
