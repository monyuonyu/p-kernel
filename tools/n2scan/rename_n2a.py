#!/usr/bin/env python3
"""N2a: rename the 8-bit node types that check_types.py reports.

  python3 tools/n2scan/rename_n2a.py <repo>   # run once for N2a (dcdc3a19); a re-run finds nothing

W (wire field) -> pk_node8_t, P (common prototype param) and D (its .c
definition) -> pk_node_t.
Adds #include "pk_node.h" to every changed header (a .c file sees it through
the header it includes; N2a's first run, dcdc3a19, also added it to 3 .c files). Prints every change. Uses check_types.py's own scanners so
it changes exactly what the check reports.
"""
import os, re, sys
repo = os.path.abspath(sys.argv[1])
sys.path.insert(0, os.path.join(repo, 'tools', 'n2scan'))
sys.argv = [sys.argv[0], repo]
import check_types as ct

BYTE_AT = re.compile(r'\b(UB|U1|uint8_t|unsigned\s+char)(\s+)')
edits = {}   # relp -> list of (line, name, newtype)
for relp, ln, owner, name, t in ct.wire_fields():
    if (owner, name) in ct.NOT_NODE or t == 'pk_node8_t':
        continue
    edits.setdefault(relp, []).append((ln, name, 'pk_node8_t', 0))
protos = [q for q in ct.proto_params() if (q[2], q[3]) not in ct.NOT_NODE]
for relp, ln, owner, name, t in protos + list(ct.def_params(protos)):
    if t == 'pk_node_t':
        continue
    edits.setdefault(relp, []).append((ln, name, 'pk_node_t', 3))

total = 0
for relp, es in sorted(edits.items()):
    p = os.path.join(repo, relp)
    lines = open(p).read().split('\n')
    for ln, name, nt, span in es:
        done = False
        for i in range(ln - 1, min(len(lines), ln + span)):
            rx = re.compile(r'\b(UB|U1|uint8_t|unsigned\s+char)(\s+)(' + re.escape(name) + r')\b(?!\s*\()')
            m = rx.search(lines[i])
            if not m:
                continue
            old = m.group(1) + m.group(2)
            pad = ' ' * max(1, len(old) - len(nt))
            new = lines[i][:m.start()] + nt + pad + lines[i][m.start(3):]
            print(f'{relp}:{i+1}: {lines[i].strip()}\n    -> {new.strip()}')
            lines[i] = new
            done = True
            total += 1
            break
        if not done:
            print(f'!! {relp}:{ln} {name} not found')
    txt = '\n'.join(lines)
    if relp.endswith('.h') and '"pk_node.h"' not in txt:
        inc = [m for m in re.finditer(r'^#include[^\n]*\n', txt, re.M)]
        if inc:
            at = inc[0].end()
            txt = txt[:at] + '#include "pk_node.h"   /* pk_node_t, pk_node8_t (N2a) */\n' + txt[at:]
            print(f'{relp}: + #include "pk_node.h"')
        else:
            print(f'!! {relp}: no #include to anchor on')
    open(p, 'w').write(txt)
print(f'{total} renamed in {len(edits)} files')
