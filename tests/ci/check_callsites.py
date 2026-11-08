#!/usr/bin/env python3
"""tests/ci/check_callsites.py [--root DIR] [--selftest]

Pins a few call sites that the in-process certs do not reach (docs/architecture/
test-holes.md H5, H12, part of H2). The certs (`nodes ipw`, `nodes mhwide`) test
the helper functions (pk_node_ip_like, mh_have_frame, mh_have_rx_sender, ...);
whether the sender and the receiver actually call them was only read by eye,
and the audits' breaks there stayed green.

Each rule names a function body in a source file and the text it must and must
not contain. --selftest applies each rule's break (the audits' own break,
written into the text in memory) and wants that rule, and only that rule, red;
then it wants the real tree green.

What it is not: a semantic check. A rewrite that keeps the call but ignores its
result passes; renaming a helper needs the rule renamed with it (the rule then
goes red, it does not go quiet).

Exit 0 = every rule holds (and with --selftest every break is caught).
"""
import os
import re
import sys

# (id, file, function, must contain (regex), must not contain (regex), hole, break (old, new))
RULES = [
    ("snf-ip", "arch/common/supernode.c", "snf_node_ip",
     [r"pk_node_ip_like\s*\("], [r"<<\s*24"], "H12",
     ("return (UW)pk_node_ip_like(n, (unsigned int)net_my_ip);",
      "return ((UW)(n + 1) << 24) | (net_my_ip & 0x00FFFFFFUL);")),
    ("ss6-ip", "arch/common/ss6_live.c", "sl_node_ip",
     [r"pk_node_ip_like\s*\("], [r"<<\s*24"], "H12",
     ("return (UW)pk_node_ip_like(n, (unsigned int)net_my_ip);",
      "return ((UW)(n + 1) << 24) | (net_my_ip & 0x00FFFFFFUL);")),
    ("mh-send", "arch/common/r3_incontext.c", "mh_publish",
     [r"mh_have_frame\s*\(", r"kdds_pub\s*\(\s*mh_pub\s*,\s*&mh_tx_w\s*,\s*fn\s*\)"],
     [r"kdds_pub\s*\([^;]*sizeof"], "H5 (B1)",
     ("(void)kdds_pub(mh_pub, &mh_tx_w, fn);", "(void)kdds_pub(mh_pub, &mh_tx, (W)sizeof mh_tx);")),
    ("mh-recv-sender", "arch/common/r3_incontext.c", "mh_poll",
     [r"mh_have_rx_sender\s*\(\s*&mh_rx_w\s*,\s*r\s*\)", r"if\s*\(\s*!\s*mh_sender_takes\s*\(\s*o\s*\)\s*\)"],
     [r"DNODE_MAX"], "H5 (B2)",
     ("if (!mh_sender_takes(o)) return;", "if (o >= DNODE_MAX) return;")),
    ("mh-recv-origin", "arch/common/r3_incontext.c", "mh_poll",
     [r"mh_have_rx_origin\s*\(\s*&mh_rx_w\s*,\s*r\s*,\s*i\s*\)", r"mh_beats\s*\("],
     [r"\.e\[i\]\.origin"], "H5",
     ("pk_node_t o_in = mh_have_rx_origin(&mh_rx_w, r, i);", "pk_node_t o_in = mh_rx.e[i].origin;")),
    ("mh-busy", "arch/common/r3_incontext.c", "mh_poll",
     [r"MC_PUSH_BUSY\s*\("], [r"mc_push_pkt\.key\s*=="], "H2 (part: the busy test goes through MC_PUSH_BUSY)",
     ("INT busy  = MC_PUSH_BUSY((UB)k, s_cur, o_cur);",
      "INT busy  = mc_push_sends_left > 0 && mc_push_pkt.key == (UB)k;")),
]


def body(text, func):
    """the text of the first definition of func (from its name to the matching brace), or None"""
    m = re.search(r"^[A-Za-z_][\w \t\*]*\b%s\s*\([^;{]*\)\s*\{" % re.escape(func), text, re.M)
    if not m:
        return None
    i, depth = m.end() - 1, 0
    while i < len(text):
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return text[m.start():i + 1]
        i += 1
    return None


def check(texts):
    """texts: {file: text} -> [(rule id, ok, why)]"""
    out = []
    for rid, f, func, must, mustnot, hole, _ in RULES:
        b = body(texts[f], func)
        if b is None:
            out.append((rid, False, "%s: %s() not found (renamed? move the rule with it)" % (f, func)))
            continue
        miss = [p for p in must if not re.search(p, b)]
        bad = [p for p in mustnot if re.search(p, b)]
        why = "; ".join(["missing /%s/" % p for p in miss] + ["has /%s/" % p for p in bad])
        out.append((rid, not miss and not bad, "%s %s() [%s]%s" % (f, func, hole, (": " + why) if why else "")))
    return out


def load(root):
    return {f: open(os.path.join(root, f), encoding="utf-8", errors="replace").read()
            for f in sorted({r[1] for r in RULES})}


def main(argv):
    root, selftest, i = os.getcwd(), False, 1
    while i < len(argv):
        if argv[i] == "--root":
            root = argv[i + 1]; i += 2
        elif argv[i] == "--selftest":
            selftest = True; i += 1
        else:
            print(__doc__); return 2
    texts = load(root)
    bad = 0
    if selftest:
        for rid, f, func, _, _, hole, (old, new) in RULES:
            if texts[f].count(old) != 1:
                print("[callsites-self] WRONG %s: the break's text is not in %s once (the source moved: "
                      "rewrite the break)" % (rid, f))
                bad += 1
                continue
            t = dict(texts)
            t[f] = t[f].replace(old, new)
            res = {r[0]: r[1] for r in check(t)}
            red = sorted(k for k, ok in res.items() if not ok)
            ok = red == [rid]
            print("[callsites-self] %s break %s -> red %s" % ("ok   " if ok else "WRONG", rid, red))
            bad += not ok
    for rid, ok, why in check(texts):
        print("[callsites] %s %s %s" % ("ok  " if ok else "RED ", rid, why))
        bad += not ok
    print("[result] %s callsites%s" % ("PASS" if bad == 0 else "FAIL", " (selftest)" if selftest else ""))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
