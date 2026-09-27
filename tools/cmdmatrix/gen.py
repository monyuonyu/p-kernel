#!/usr/bin/env python3
# tools/cmdmatrix/gen.py — inbox #4 stage 0: the shell-command x platform table.
#
# Reads each platform's shell dispatch STATICALLY (design:
# docs/architecture/20-architecture/command-parity.md) and writes
# docs/architecture/command-matrix.md. `--check` regenerates in memory and
# exits 1 if the committed table differs (CI), or if a source contains a
# command comparison in an idiom this reader does not know (the table would
# silently under-count).
#
# Idioms read (first word of the command only; sub-commands are stage 1):
#   starts_with(line, n, "word")     arch/linux/*/usermain.c (+ Windows, Android)
#   strneq(line, "word", N)          arch/aarch64/usermain.c
#   str_eq(cmd, "word")              arch/x86/shell.c
#   cmd[0]=='w' && cmd[1]=='o' ...   arch/x86/shell.c (one-char chains)
import os, re, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
OUT = "docs/architecture/command-matrix.md"
EXC = "tools/cmdmatrix/exceptions.txt"

# column -> (source file, note). Windows and Android build the Linux usermain.c
# (boot/windows/x86_64/Makefile ARCH_SHARED_X86_SRCS; android CMakeLists.txt).
COLS = [
    ("x86 bare",        "arch/x86/shell.c",               ""),
    ("AArch64 bare",    "arch/aarch64/usermain.c",        ""),
    ("Linux x86_64",    "arch/linux/x86_64/usermain.c",   ""),
    ("Linux aarch64",   "arch/linux/aarch64/usermain.c",  ""),
    ("Android",         "arch/linux/aarch64/usermain.c",  "= Linux aarch64 の usermain.c"),
    ("Windows x86_64",  "arch/linux/x86_64/usermain.c",   "= Linux x86_64 の usermain.c"),
]

KNOWN = [
    re.compile(r'\bstarts_with\(\s*line\s*,\s*\w+\s*,\s*"([^"]+)"\s*\)'),
    re.compile(r'\bstrneq\(\s*line\s*,\s*"([^"]+)"\s*,\s*\w+\s*\)'),
    re.compile(r'\bstr_eq\(\s*cmd\s*,\s*"([^"]+)"\s*\)'),
]
# any call that compares line/cmd against a string literal, known or not
ANYCMP = re.compile(r'\b(\w+)\(\s*(?:line|cmd)\s*,[^;{]*?"([^"]*)"')
CHAR = re.compile(r"\bcmd\[(\d+)\]\s*==\s*'(\\.|[^'])'")
WORDCH = re.compile(r"[A-Za-z0-9_-]")
KNOWN_FUNCS = {"starts_with", "strneq", "str_eq"}


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def read_cmds(path):
    return read_src(open(os.path.join(ROOT, path), encoding="utf-8", errors="replace").read(), path)


CHARLIT = re.compile(r"'(\\.|[^'])'")


def logical_lines(src):
    """Yield (line_no, text). A one-char chain whose `if (` condition is not
    closed on its line (arch/x86/shell.c splits sensor/replica/persist/degrade
    after `&&`) is joined with the following lines until the parentheses
    balance, so the word is not cut at the line break (audit-12)."""
    lines = src.splitlines()
    i = 0
    while i < len(lines):
        ln, no = lines[i], i + 1
        if CHAR.search(ln) and re.search(r"\bcmd\[0\]", ln):
            depth = lambda s: s.count("(") - s.count(")")
            d = depth(CHARLIT.sub("", ln))
            while d > 0 and i + 1 < len(lines) and i + 1 - (no - 1) < 6:
                i += 1
                ln += " " + lines[i]
                d += depth(CHARLIT.sub("", lines[i]))
        yield no, ln
        i += 1


def read_src(src, path):
    src = strip_comments(src)
    cmds, unknown = set(), []
    for ln_no, ln in logical_lines(src):
        for rx in KNOWN:
            for m in rx.finditer(ln):
                w = m.group(1).split()[0] if m.group(1).split() else ""
                if w:
                    cmds.add(w)
        for m in ANYCMP.finditer(ln):
            if m.group(1) not in KNOWN_FUNCS:
                unknown.append("%s:%d %s(..\"%s\"..)" % (path, ln_no, m.group(1), m.group(2)))
        chain = CHAR.findall(ln)
        if chain and chain[0][0] == "0":
            by_i = {}
            for i, c in chain:
                by_i.setdefault(int(i), c)
            w = ""
            k = 0
            while k in by_i and WORDCH.match(by_i[k]):
                w += by_i[k]; k += 1
            if w:
                cmds.add(w)
    return cmds, unknown


def load_exceptions():
    exc = {}
    p = os.path.join(ROOT, EXC)
    if not os.path.exists(p):
        return exc
    for ln in open(p, encoding="utf-8"):
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        # <command> <column> <reason...>   (column with spaces written with _)
        parts = ln.split(None, 2)
        if len(parts) == 3:
            exc[(parts[0], parts[1].replace("_", " "))] = parts[2]
    return exc


def render():
    per_src, unknown_all = {}, []
    for _, src, _ in COLS:
        if src not in per_src:
            per_src[src], unk = read_cmds(src)
            unknown_all += unk
    exc = load_exceptions()
    allc = sorted(set().union(*per_src.values()))
    L = []
    L.append("# コマンド × プラットフォーム（自動生成。手で直さない）")
    L.append("")
    L.append("`python3 tools/cmdmatrix/gen.py` が各シェルのソースを静的に読んで書く。")
    L.append("CI は `--check` で、この表と生成し直した結果が食い違えば赤にする。")
    L.append("設計と限界: `docs/architecture/20-architecture/command-parity.md`（inbox #4 段階0）。")
    L.append("✓ = 振り分けがある / — = 無い / 不可 = `tools/cmdmatrix/exceptions.txt` に理由")
    L.append("")
    L.append("| 対象 | 読んだソース | コマンド数 |")
    L.append("|---|---|---|")
    for name, src, note in COLS:
        L.append("| %s | `%s`%s | %d |" % (name, src, ("（" + note + "）") if note else "", len(per_src[src])))
    L.append("")
    L.append("| コマンド | " + " | ".join(c[0] for c in COLS) + " |")
    L.append("|---|" + "---|" * len(COLS))
    for c in allc:
        row = []
        for name, src, _ in COLS:
            if c in per_src[src]:
                row.append("✓")
            elif (c, name) in exc:
                row.append("不可: " + exc[(c, name)])
            else:
                row.append("—")
        L.append("| `%s` | " % c + " | ".join(row) + " |")
    L.append("")
    return "\n".join(L), unknown_all


def main():
    text, unknown = render()
    path = os.path.join(ROOT, OUT)
    if "--check" in sys.argv:
        ok = True
        if unknown:
            ok = False
            print("[cmdmatrix] FAIL: command comparisons in an idiom this reader does not know:")
            for u in unknown:
                print("  " + u)
        cur = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
        if cur != text:
            ok = False
            print("[cmdmatrix] FAIL: %s is stale; run python3 tools/cmdmatrix/gen.py" % OUT)
        print("[cmdmatrix] PASS" if ok else "[cmdmatrix] FAILURES ABOVE")
        sys.exit(0 if ok else 1)
    open(path, "w", encoding="utf-8").write(text)
    for u in unknown:
        print("[cmdmatrix] WARNING unknown idiom: " + u)
    print("[cmdmatrix] wrote %s" % OUT)


if __name__ == "__main__":
    main()
