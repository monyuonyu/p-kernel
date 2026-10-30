#!/usr/bin/env python3
"""N2a gate: node ids are spelled with the node-id type names, not a bare byte.

Design: docs/architecture/30-module/research/n2-wire16.md (N2a). Two names,
both defined in arch/common/include/pk_node.h:

  pk_node8_t  the 8-bit node id as it is on the v1 wire and on disk. It never
              widens: a v1 packet keeps its layout when N2b widens the ids.
  pk_node_t   the node id inside a node (today the same byte; N2b may widen it
              on hosted, never on bare metal).

Checked (red = exit 1):
  W  every node field of a wire/disk struct (the structs of inventory.py's
     table 1) is declared pk_node8_t. A bare byte is red (not named yet), and
     so is pk_node_t (it would change the packet's layout when it widens).
  P  every node parameter of a prototype in arch/common/include/*.h is
     pk_node_t (a bare byte is red; pk_node8_t is red too, it is not the
     inside type).
  D  the .c definitions of those functions name the same parameter
     pk_node_t (single-line parameter lists only; see def_params).
  R  prototypes of those functions written again outside the common headers
     (IMPORT/extern in a .c) name the same parameter pk_node_t (audit-103).
  H  pk_node.h defines both names as unsigned char.

Static and heuristic, like inventory.py (the same struct and name regexes): a
node id in a field or parameter whose name has none of inventory.NODE_WORDS is
not seen. NOT_NODE lists the names that match the words but are not node ids.

  python3 tools/n2scan/check_types.py [repo]
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
REPO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', '..'))
sys.argv = [sys.argv[0], REPO]
import inventory as inv  # noqa: E402  (reads REPO from argv)

BYTE = r'UB|U1|uint8_t|unsigned\s+char'
TYPES = r'(' + BYTE + r'|pk_node8_t|pk_node_t)'
FIELD = re.compile(r'\b' + TYPES + r'\s+(\w*' + inv.NODE_WORDS + r'\w*)\s*(\[[^\]]*\])?\s*;', re.I)
PARAM = re.compile(r'[(,]\s*(?:const\s+)?' + TYPES + r'\s+(\w*' + inv.NODE_WORDS + r'\w*)\s*(?=[,)])', re.I)

# (struct or function, name): matches the node words but is not a node id.
NOT_NODE = {
    ('ARK_PROV', 'src'): 'ARK_PROV_SRC_SHELL/WEB, where a fact came from',
    ('MT_TEACH_PKT', 'src'): 'ARK_PROV_SRC_*, carried for the provenance',
    ('VITAL_DATA', 'peers_alive'): 'a count of peers',
    ('ark_prov_record', 'src'): 'ARK_PROV_SRC_*',
    ('ark_teach_src_set', 'src'): 'ARK_PROV_SRC_*',
}


def is_byte(t):
    return re.fullmatch(BYTE, t) is not None


# inventory.STRUCT does not see "typedef struct __attribute__((packed)) {":
# KLOAD_PKT and KL_BEACON_PKT were missed by table 1 and by W (audit-103).
PRE_PACKED = re.compile(r'typedef\s+struct\s+__attribute__\s*\(\(\s*packed\s*\)\)\s*\w*\s*\{(.*?)\}\s*(\w+)\s*;', re.S)


def structs(txt):
    """(body, packed, name, body start) of each typedef'd struct."""
    for m in inv.STRUCT.finditer(txt):
        yield m.group(1), bool(m.group(2)), m.group(3), m.start(1)
    for m in PRE_PACKED.finditer(txt):
        yield m.group(1), True, m.group(2), m.start(1)


def wire_fields():
    for relp, txt in inv.walk():
        for body, packed, sname, at in structs(txt):
            if not (packed or re.search(r'\bmagic\b', body)
                    or re.search(r'(PKT|MSG|ANN|ANNOUNCE|BEACON|HDR|MANIFEST)$', sname)):
                continue
            for g in FIELD.finditer(body):
                name = g.group(2)
                if (g.group(4) or '').strip() == '[6]' and name in ('src', 'dst'):
                    continue                                   # MAC addresses
                ln = txt[:at + g.start()].count('\n') + 1
                yield relp, ln, sname, name, g.group(1)


def proto_params():
    inc = os.path.join(REPO, 'arch', 'common', 'include')
    for f in sorted(os.listdir(inc)):
        if not f.endswith('.h'):
            continue
        txt = open(os.path.join(inc, f), errors='replace').read()
        txt = re.sub(r'/\*.*?\*/', lambda c: '\n' * c.group(0).count('\n'), txt, flags=re.S)
        txt = re.sub(r'//[^\n]*', '', txt)
        # a prototype or a function-pointer typedef: name '(' ... ')' up to ';' or '{'
        for m in re.finditer(r'(\w+)\s*\)?\s*\(([^;{}()]*(?:\([^()]*\)[^;{}()]*)*)\)\s*[;{]', txt):
            fn = m.group(1)
            if fn in ('if', 'while', 'for', 'switch', 'return', 'sizeof'):
                continue
            args = '(' + m.group(2) + ')'
            for g in PARAM.finditer(args):
                ln = txt[:m.start(2)].count('\n') + 1
                yield 'arch/common/include/' + f, ln, fn, g.group(2), g.group(1)


def def_params(protos):
    """Definitions (.c, outside inventory.SKIP) of the functions checked in P:
    the same parameter, by name, must be pk_node_t there too."""
    want = {}
    for _, _, fn, name, _ in protos:
        want.setdefault(fn, set()).add(name)
    rx = re.compile(r'^[A-Za-z_][\w \t\*]*?\b(' + '|'.join(sorted(map(re.escape, want))) +
                    r')\s*\(([^;{}()]*)\)\s*\{', re.M)
    for relp, txt in inv.walk():
        if not relp.endswith('.c'):
            continue
        for m in rx.finditer(txt):
            fn = m.group(1)
            for g in PARAM.finditer('(' + m.group(2) + ')'):
                if g.group(2) in want[fn]:
                    ln = txt[:m.start(2)].count('\n') + 1
                    yield relp, ln, fn, g.group(2), g.group(1)


def redecl_params(protos):
    """Prototypes of the functions checked in P written again outside the
    common headers (IMPORT/extern in a .c, e.g. dkva.c's dtr_seed_kv_cache):
    the same parameter must be pk_node_t. A .c that does not see the header
    would otherwise keep the byte silently when pk_node_t widens (audit-103:
    dkva.c does not include dtr.h)."""
    want = {}
    for _, _, fn, name, _ in protos:
        want.setdefault(fn, set()).add(name)
    rx = re.compile(r'^[ \t]*(?:IMPORT|extern)?[ \t]*[A-Za-z_][\w \t\*]*?\b(' +
                    '|'.join(sorted(map(re.escape, want))) + r')\s*\(([^;{}()]*)\)\s*;', re.M)
    for relp, txt in inv.walk():
        if relp.startswith('arch/common/include/') or not relp.endswith(('.c', '.h')):
            continue
        for m in rx.finditer(txt):
            fn = m.group(1)
            if re.match(r'\s*(return|if|while)\b', m.group(0)) or '=' in m.group(0):
                continue                                       # a call, not a prototype
            for g in PARAM.finditer('(' + m.group(2) + ')'):
                if g.group(2) in want[fn]:
                    ln = txt[:m.start(2)].count('\n') + 1
                    yield relp, ln, fn, g.group(2), g.group(1)


def main():
    bad = []
    nw = np_ = nd = 0
    for relp, ln, owner, name, t in wire_fields():
        if (owner, name) in NOT_NODE:
            continue
        nw += 1
        if t != 'pk_node8_t':
            bad.append(f'W {relp}:{ln} {owner}.{name} is {t}, want pk_node8_t')
    protos = [p for p in proto_params() if (p[2], p[3]) not in NOT_NODE]
    for relp, ln, owner, name, t in protos:
        np_ += 1
        if t != 'pk_node_t':
            bad.append(f'P {relp}:{ln} {owner}({name}) is {t}, want pk_node_t')
    for relp, ln, owner, name, t in def_params(protos):
        nd += 1
        if t != 'pk_node_t':
            bad.append(f'D {relp}:{ln} {owner}({name}) is {t}, want pk_node_t')
    nr = 0
    for relp, ln, owner, name, t in redecl_params(protos):
        nr += 1
        if t != 'pk_node_t':
            bad.append(f'R {relp}:{ln} {owner}({name}) is {t}, want pk_node_t')
    hp = os.path.join(REPO, 'arch', 'common', 'include', 'pk_node.h')
    h = open(hp).read() if os.path.exists(hp) else ''
    for t in ('pk_node8_t', 'pk_node_t'):
        if not re.search(r'typedef\s+unsigned\s+char\s+' + t + r'\s*;', h):
            bad.append(f'H arch/common/include/pk_node.h has no "typedef unsigned char {t};"')
    for b in bad:
        print(b)
    print(f'[n2a-types] wire node fields {nw}, prototype node params {np_}, '
          f'their definitions {nd}, re-declarations {nr}, not named {len(bad)} -> {"FAIL" if bad else "PASS"}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
