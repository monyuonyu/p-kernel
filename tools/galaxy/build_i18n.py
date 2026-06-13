#!/usr/bin/env python3
# build_i18n.py — splice the full 32-language I18N object literal (from
# i18n_data.py) into arch/common/web/galaxy.html, between the
# "const I18N = {" line and its closing "};".
#
# This is a SOURCE-EDIT helper (run once by the implementer / on translation
# changes), NOT a build-time step. galaxy.html stays the committed source of
# truth; the build still goes galaxy.html -> gen_page.py -> galaxy_page.h.
#
# JS string escaping is delegated to json.dumps so apostrophes (fr/it/uk),
# quotes, backslashes, CJK and RTL all survive intact.
import json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import i18n_data as D

HTML = os.path.normpath(os.path.join(HERE, "..", "..", "arch", "common", "web", "galaxy.html"))
# repo-root relative: tools/galaxy/ -> ../../arch/common/web
if not os.path.exists(HTML):
    HTML = os.path.normpath(os.path.join(HERE, "..", "..", "..", "arch", "common", "web", "galaxy.html"))

# Key order = the en key order (stable, human-readable diff).
KEY_ORDER = list(D.T["en"].keys())

def js_lang(code, d):
    parts = []
    for k in KEY_ORDER:
        parts.append("%s:%s" % (json.dumps(k, ensure_ascii=False),
                                 json.dumps(d[k], ensure_ascii=False)))
    # key codes that are not bare identifiers need quoting (zh-Hans etc.)
    keytok = code if re.match(r"^[A-Za-z_$][\w$]*$", code) else json.dumps(code, ensure_ascii=False)
    return "  %s:{%s}" % (keytok, ",".join(parts))

def build_literal():
    out = ["const I18N = {"]
    langs = []
    for code in D.LANG_ORDER:
        langs.append(js_lang(code, D.T[code]))
    out.append(",\n".join(langs))
    out.append("};")
    return "\n".join(out)

def main():
    src = open(HTML, encoding="utf-8").read()
    # Match from "const I18N = {" up to the first "\n};" that closes it.
    pat = re.compile(r"const I18N = \{.*?\n\};", re.S)
    m = pat.search(src)
    if not m:
        sys.stderr.write("could not locate const I18N = { ... };\n")
        return 1
    new = build_literal()
    src2 = src[:m.start()] + new + src[m.end():]
    open(HTML, "w", encoding="utf-8").write(src2)
    sys.stderr.write("spliced %d languages x %d keys into %s\n"
                     % (len(D.LANG_ORDER), len(KEY_ORDER), HTML))
    return 0

if __name__ == "__main__":
    sys.exit(main())
