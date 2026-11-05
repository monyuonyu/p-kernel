#!/usr/bin/env python3
"""tests/ci/check_vmodel.py — ROADMAP 0-7, first slice: keep the design-doc
<-> cert table in docs/architecture/V-MODEL.md honest by machine.

RED when:
  missing-row   a design doc under 10-requirements/ 20-architecture/
                30-module/ 40-verify/ 50-evolution/ has no row in the table
                (30-module/research/ memos are notes for an item, not rungs)
  stale-row     a row names a doc that does not exist
  dup-row       a doc has more than one row
  no-cert       a CLOSED or PARTIAL row cites no cert at all
  unknown-cert  a CLOSED or PARTIAL row cites a cert found nowhere: a [tag]
                must appear whole as "[tag]" (a [pre*] as "[pre...]"), not glued
                to an identifier, in the code / tests / samples / ci.yml, and
                be at least 3 characters; a `name` must be a ci.yml job id, a
                step's name:, a path from the repo root that exists (if it has
                a '/'), a file name in the repo or a samples/ directory with a
                script in it (not any other directory name), or a whole
                phrase of ci.yml with a space, '-' or '_' in it. The row's own
                design doc and V-MODEL.md are never a cert
  open-ceiling  more OPEN rows than OPEN_MAX (OPEN may only go down; lower
                OPEN_MAX in the same change that closes one)
  tally         a count in the "Rung tally" table differs from the rows, or one
                of its five lines is missing
  no-head       a design doc does not carry its own rung and certs right under
                its "# " title (second slice of 0-7, the doc is the source):
                    <!-- vmodel rung: CLOSED -->
                    <!-- vmodel certs: [tag] `name` -->     ("certs: -" if none)
  head-mismatch the doc's head and its V-MODEL.md row disagree on the rung or
                on the set of [tag] / `name` cert tokens (the table is the view
                of the heads; free text in the cert cell is not compared)
  ambiguous-cert a bare `name` (no '/') that is proved only by a file name
                which occurs more than once in the repo (`run.sh`): write the
                path from the repo root instead

`--selftest` breaks a copy of the real table sixteen ways (each check, the
three that audit-20 found green: a bare substring of ci.yml, a one-letter tag,
a deleted tally line, and the six audit-21 found green: a missing path with a
real basename, the row's own doc, V-MODEL.md, a directory name, a plain ci.yml
word, a step name's first word) and requires each to go red, and the unbroken
copy green, and then breaks the heads four more ways (a doc without a head, a
head with another rung, a head missing a cert token, a bare name that is two
files) and requires each of those to go red too.
`--migrate` writes the head into every design doc that has none, copied from
its row (it never rewrites an existing head).
Exit 0 = green, 1 = red.
"""
import collections
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ARCH = os.path.join(ROOT, "docs", "architecture")
VMODEL = os.path.join(ARCH, "V-MODEL.md")
LEVEL_DIRS = ["10-requirements", "20-architecture", "30-module", "40-verify", "50-evolution"]
SKIP_SUBDIRS = {"research"}
OPEN_MAX = 12  # 2026-10-04: memory-thought, web-os, self-access, self-access-design,
               # gpu-compute, gpu-3-wiring, webd-user-space, living-body-inspector,
               # n1-lan-direct-plan, mind-gen0-b, android-always-on.
               # 10 -> 11 for android-always-on (d9aec0b0): a new design memo for an
               # item not built yet (inbox #7-1) is honestly OPEN; it is not an old
               # rung slipping back. Raising this needs a reason here, every time.
               # 11 -> 12 for ark-entry (2026-11-06): the ark's entry v1 design memo
               # (audit-134, merged without a row, which reddened CI 42086847022);
               # nothing built yet, the build starts in December.
CORPUS_DIRS = ["arch", "boot", "tests", "samples", "tools", "relay", "apps", "android", "kernel/mtkernel3/lib"]
CORPUS_EXT = (".c", ".h", ".S", ".sh", ".py", ".yml", ".yaml", ".kt", ".java", ".rs", ".ts", ".js", ".mk")

ROW = re.compile(r"^\|\s*([A-Za-z0-9_.\-]+\.md)\s*\|(.*)\|(.*)\|(.*)\|\s*$")
TALLY = re.compile(r"^\|\s*\*\*([^*]+)\*\*.*?\|\s*\*\*(\d+)\*\*\s*\|")


def rung_of(cell):
    c = cell.strip()
    if c.startswith("✓"):
        return "CLOSED"
    if c.startswith("◐"):
        return "PARTIAL"
    if c.startswith("○"):
        return "OPEN"
    if c.startswith("N/A"):
        return "N/A"
    if c.startswith("DECLINED"):
        return "DECLINED"
    return "?"


def design_doc_paths():
    out = {}
    for d in LEVEL_DIRS:
        base = os.path.join(ARCH, d)
        for dp, dns, fns in os.walk(base):
            dns[:] = [x for x in dns if x not in SKIP_SUBDIRS]
            for f in fns:
                if f.endswith(".md"):
                    out[f] = os.path.join(dp, f)
    return out


def design_docs():
    return set(design_doc_paths())


HEAD_RUNG = re.compile(r"^<!-- vmodel rung: (CLOSED|PARTIAL|OPEN|N/A|DECLINED) -->$")
HEAD_CERTS = re.compile(r"^<!-- vmodel certs: (.+) -->$")


def read_head(path):
    """(rung, tags, names) from the two lines right under the doc's "# " title,
    or None when they are not there."""
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().split("\n")
    if len(lines) < 3 or not lines[0].startswith("# "):
        return None
    r, c = HEAD_RUNG.match(lines[1]), HEAD_CERTS.match(lines[2])
    if not r or not c:
        return None
    tags, nms = cert_tokens(c.group(1)) if c.group(1).strip() != "-" else ([], [])
    return r.group(1), tags, nms


def head_lines(rung, tags, nms):
    toks = ["[%s]" % t for t in tags] + ["`%s`" % n for n in nms]
    return ["<!-- vmodel rung: %s -->" % rung,
            "<!-- vmodel certs: %s -->" % (" ".join(toks) if toks else "-")]


def read_heads():
    return {f: read_head(p) for f, p in design_doc_paths().items()}


def root_docs():
    """docs filed at docs/architecture/ itself (scaling-law, vendor-patch-
    inventory, ...): a row may name them, but they are not required to have one."""
    return {f for f in os.listdir(ARCH) if f.endswith(".md")}


def all_md_names():
    out = set()
    for dp, dns, fns in os.walk(os.path.join(ROOT, "docs")):
        out.update(f for f in fns if f.endswith(".md"))
    return out


def load_corpus():
    text = []
    count = collections.Counter()  # how many files / sample dirs carry each name
    for dp, dns, fns in os.walk(os.path.join(ROOT, "docs")):
        count.update(f for f in fns if f.endswith(".md"))
    names = set(all_md_names())   # a cited audit/evidence doc (`x.md`) must exist
    for d in CORPUS_DIRS:
        base = os.path.join(ROOT, d)
        for dp, dns, fns in os.walk(base):
            # files, and a samples/ directory with a script in it (`14_genome`);
            # other directory names (`host`, `common`) prove nothing
            if (os.path.dirname(dp) == os.path.join(ROOT, "samples")
                    and any(f.endswith(".sh") for f in fns)):
                names.add(os.path.basename(dp))
                count[os.path.basename(dp)] += 1
            for f in fns:
                names.add(f)
                count[f] += 1
                if f.endswith(CORPUS_EXT):
                    p = os.path.join(dp, f)
                    if os.path.abspath(p) == os.path.abspath(__file__):
                        continue          # its own selftest strings are not certs
                    try:
                        if os.path.getsize(p) > 4 * 1024 * 1024:
                            continue
                        with open(p, encoding="utf-8", errors="ignore") as fh:
                            text.append(fh.read())
                    except OSError:
                        pass
    with open(os.path.join(ROOT, ".github", "workflows", "ci.yml"), encoding="utf-8") as fh:
        ci = fh.read()
    jobs = set(re.findall(r"^  ([A-Za-z0-9_\-]+):\s*$", ci, flags=re.M))
    steps = {x.strip().strip("'\"").strip()
             for x in re.findall(r"^\s*-?\s*name:\s*(.+?)\s*$", ci, flags=re.M)}
    return "\n".join(text) + "\n" + ci, ci, names, jobs | steps, count


def cert_tokens(cell):
    """[tag] and `name` tokens of a cert cell (a backticked [tag] is a tag)."""
    tags, names = [], []
    for b in re.findall(r"`([^`]+)`", cell):
        b = b.strip()
        if b.startswith("["):
            tags += re.findall(r"\[([A-Za-z0-9][A-Za-z0-9_\-./*]*)\]", b)
        else:
            names.append(b)
    rest = re.sub(r"`[^`]*`", " ", cell)
    tags += re.findall(r"\[([A-Za-z0-9][A-Za-z0-9_\-./*]*)\]", rest)
    return tags, names


TAG_MIN = 3        # a tag (or the prefix before '*') shorter than this proves nothing
ID = r"A-Za-z0-9_\-./"


def tag_found(tag, blob):
    """[tag] must appear whole, not glued to an identifier (`a[s]` is C, not a
    cert); [pre*] needs [pre followed by a tag body. Too-short tags never count."""
    pre, star, _ = tag.partition("*")
    if len(pre.strip("-_./")) < TAG_MIN:
        return False
    lead = r"(?<![A-Za-z0-9_\])])\["
    if star:
        pat = lead + re.escape(pre) + "[" + ID + "]*\\]"
    else:
        pat = lead + re.escape(tag) + "\\]"
    return re.search(pat, blob) is not None


NOT_CERTS = {"V-MODEL.md"}   # the table cannot prove itself


def name_found(n, ci, names, jobs, doc=None):
    """a `name` is a ci.yml job id, a step's name:, a path from the repo root
    that exists (when it has a '/'), a file name (not a directory), or a whole
    phrase of ci.yml that has a space, '-' or '_' in it (not part of a longer
    one: `ump` is not `ump-x86_64`; a plain word like `make` proves nothing).
    The row's own design doc and V-MODEL.md never count."""
    if os.path.basename(n) in NOT_CERTS or (doc and os.path.basename(n) == doc):
        return False
    if n in jobs:
        return True
    if "/" in n:
        return os.path.isfile(os.path.join(ROOT, n))
    if n in names:
        return True
    if len(n) < TAG_MIN or not re.search(r"[ \-_]", n):
        return False
    return re.search("(?<![" + ID + "])" + re.escape(n) + "(?![" + ID + "])", ci) is not None


def name_ambiguous(n, ci, names, jobs, count):
    """a bare name that only a file name proves, and more than one file has it"""
    if n in jobs or "/" in n or n not in names or count.get(n, 0) < 2:
        return False
    return not name_found(n, ci, set(), jobs)   # does a ci.yml job/step/phrase prove it?


def check(vtext, docs, corpus, roots=frozenset(), heads=None):
    blob, ci, names, jobs, count = corpus
    errs = []
    rows = []
    for line in vtext.splitlines():
        m = ROW.match(line)
        if m:
            rows.append((m.group(1), m.group(3), rung_of(m.group(4))))
    seen = {r[0] for r in rows}
    for d in sorted({r[0] for r in rows if [x[0] for x in rows].count(r[0]) > 1}):
        errs.append("dup-row: %s has more than one row" % d)
    for d in sorted(docs - seen):
        errs.append("missing-row: %s has no row in V-MODEL.md" % d)
    for d in sorted(seen - docs - set(roots)):
        errs.append("stale-row: V-MODEL.md names %s, which does not exist" % d)
    counts = {}
    for doc, cert, rung in rows:
        counts[rung] = counts.get(rung, 0) + 1
        if rung == "?":
            errs.append("rung: %s has an unreadable rung cell" % doc)
        if rung not in ("CLOSED", "PARTIAL"):
            continue
        tags, nms = cert_tokens(cert)
        if not tags and not nms:
            errs.append("no-cert: %s is %s but cites no cert" % (doc, rung))
        for t in tags:
            if not tag_found(t, blob):
                errs.append("unknown-cert: %s cites [%s], found nowhere as a whole tag" % (doc, t))
        for n in nms:
            if not name_found(n, ci, names, jobs, doc):
                errs.append("unknown-cert: %s cites `%s`, not a ci.yml job / step / token or a file name"
                            % (doc, n))
            elif name_ambiguous(n, ci, names, jobs, count):
                errs.append("ambiguous-cert: %s cites `%s`, a file name %d files share; give the path"
                            % (doc, n, count[n]))
    if heads is not None:
        for doc, cert, rung in rows:
            if doc not in heads:
                continue                     # a root doc: no head required
            h = heads[doc]
            if h is None:
                errs.append("no-head: %s has no '<!-- vmodel rung/certs -->' lines under its title" % doc)
                continue
            tags, nms = cert_tokens(cert)
            if h[0] != rung:
                errs.append("head-mismatch: %s head says %s, V-MODEL.md says %s" % (doc, h[0], rung))
            if set(h[1]) != set(tags) or set(h[2]) != set(nms):
                errs.append("head-mismatch: %s head certs %s, V-MODEL.md certs %s"
                            % (doc, sorted(set(h[1]) | set(h[2])), sorted(set(tags) | set(nms))))
    if counts.get("OPEN", 0) > OPEN_MAX:
        errs.append("open-ceiling: %d OPEN rows > OPEN_MAX %d (OPEN may only go down)"
                    % (counts.get("OPEN", 0), OPEN_MAX))
    key = {"✓ CLOSED": "CLOSED", "◐ PARTIAL": "PARTIAL", "○ OPEN": "OPEN",
           "N/A": "N/A", "DECLINED": "DECLINED"}
    tallied = set()
    for line in vtext.splitlines():
        m = TALLY.match(line)
        if m and m.group(1).strip() in key:
            k = key[m.group(1).strip()]
            tallied.add(k)
            if int(m.group(2)) != counts.get(k, 0):
                errs.append("tally: Rung tally says %s %s, the table has %d"
                            % (k, m.group(2), counts.get(k, 0)))
    for k in sorted(set(key.values()) - tallied):
        errs.append("tally: the Rung tally has no %s line" % k)
    return errs, counts


def selftest(vtext, docs, corpus, roots, heads):
    ok = True
    base, _ = check(vtext, docs, corpus, roots, heads)
    if base:
        print("[vmodel] selftest: the unbroken table is not green:", base[0])
        ok = False
    lines = vtext.splitlines()
    closed = next(l for l in lines if ROW.match(l) and rung_of(ROW.match(l).group(4)) == "CLOSED")
    partial = next(l for l in lines if ROW.match(l) and rung_of(ROW.match(l).group(4)) == "PARTIAL")
    cdoc = ROW.match(closed).group(1)
    breaks = [
        ("missing-row", vtext.replace(closed + "\n", ""), docs),
        ("stale-row", vtext, docs - {cdoc}),
        ("unknown-cert", vtext.replace(closed, "| %s | x | `[zz-no-such-cert]` | ✓ CLOSED |" % cdoc), docs),
        ("no-cert", vtext.replace(closed, "| %s | x | — | ✓ CLOSED |" % cdoc), docs),
        ("open-ceiling", vtext.replace(partial, re.sub(r"\|[^|]*\|\s*$", "| ○ OPEN |", partial)), docs),
        ("tally", re.sub(r"(\*\*✓ CLOSED\*\*.*?\|\s*\*\*)(\d+)", lambda m: m.group(1) + str(int(m.group(2)) + 1),
                         vtext, count=1), docs),
        # audit-20's three breaks: a bare substring of ci.yml, a one-letter tag
        # (`[s]` is everywhere in C), and a deleted tally line all used to stay green
        ("unknown-cert", vtext.replace(closed, "| %s | x | `ump` | ✓ CLOSED |" % cdoc), docs),
        ("unknown-cert", vtext.replace(closed, "| %s | x | `[s]` | ✓ CLOSED |" % cdoc), docs),
        ("tally", "\n".join(l for l in lines if not l.startswith("| **✓ CLOSED**")) + "\n", docs),
        # audit-21's six: a path that does not exist (its basename does), the
        # row's own doc, V-MODEL.md, a directory name, a plain ci.yml word, the
        # first word of a step name; and a doc with two rows
        ("unknown-cert", vtext.replace(closed, "| %s | x | `tests/no/such/dir/run.sh` | ✓ CLOSED |" % cdoc), docs),
        ("unknown-cert", vtext.replace(closed, "| %s | x | `%s` | ✓ CLOSED |" % (cdoc, cdoc)), docs),
        ("unknown-cert", vtext.replace(closed, "| %s | x | `V-MODEL.md` | ✓ CLOSED |" % cdoc), docs),
        ("unknown-cert", vtext.replace(closed, "| %s | x | `host` | ✓ CLOSED |" % cdoc), docs),
        ("unknown-cert", vtext.replace(closed, "| %s | x | `echo` | ✓ CLOSED |" % cdoc), docs),
        ("unknown-cert", vtext.replace(closed, "| %s | x | `ARK` | ✓ CLOSED |" % cdoc), docs),
        ("dup-row", vtext.replace(closed, closed + "\n" + closed), docs),
    ]
    # the second slice: the heads, and a bare name two files share
    ctags, cnms = cert_tokens(ROW.match(closed).group(3))
    drop = dict(heads); drop[cdoc] = None
    other = dict(heads); other[cdoc] = ("PARTIAL", ctags, cnms)
    fewer = dict(heads); fewer[cdoc] = ("CLOSED", ctags[1:], cnms) if ctags else ("CLOSED", ctags, cnms[1:])
    breaks = [b + (heads,) for b in breaks] + [
        ("no-head", vtext, docs, drop),
        ("head-mismatch", vtext, docs, other),
        ("head-mismatch", vtext, docs, fewer),
        ("ambiguous-cert", vtext.replace(closed, "| %s | x | `run.sh` | ✓ CLOSED |" % cdoc), docs, heads),
    ]
    for name, vt, ds, hs in breaks:
        errs, _ = check(vt, ds, corpus, roots, hs)
        hit = [e for e in errs if e.startswith(name)]
        print("[vmodel] selftest %-13s %s (%d error(s), %d of this kind)"
              % (name, "RED as expected" if hit else "STAYED GREEN", len(errs), len(hit)))
        ok = ok and bool(hit)
    return ok


def migrate(vtext):
    """write the head (copied from the row) into each design doc that has none"""
    rows = {}
    for line in vtext.splitlines():
        m = ROW.match(line)
        if m:
            rows.setdefault(m.group(1), (rung_of(m.group(4)), m.group(3)))
    n = 0
    for doc, path in sorted(design_doc_paths().items()):
        if doc not in rows or read_head(path) is not None:
            continue
        rung, cert = rows[doc]
        tags, nms = cert_tokens(cert)
        with open(path, encoding="utf-8") as fh:
            lines = fh.read().split("\n")
        if not lines[0].startswith("# "):
            print("[vmodel] migrate: %s does not start with a '# ' title, skipped" % doc)
            continue
        lines[1:1] = head_lines(rung, tags, nms)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write("\n".join(lines))
        n += 1
    print("[vmodel] migrate: wrote %d head(s)" % n)
    return 0


def main():
    with open(VMODEL, encoding="utf-8") as fh:
        vtext = fh.read()
    docs = design_docs()
    if "--migrate" in sys.argv:
        return migrate(vtext)
    corpus = load_corpus()
    heads = read_heads()
    if "--selftest" in sys.argv:
        ok = selftest(vtext, docs, corpus, root_docs(), heads)
        print("[vmodel] selftest", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    errs, counts = check(vtext, docs, corpus, root_docs(), heads)
    print("[vmodel] design docs %d, rows %d: %s" % (len(docs), sum(counts.values()),
          ", ".join("%s %d" % (k, counts[k]) for k in sorted(counts))))
    for e in errs:
        print("[vmodel]", e)
    print("[vmodel]", "PASS" if not errs else "FAIL (%d)" % len(errs))
    return 0 if not errs else 1


if __name__ == "__main__":
    sys.exit(main())
