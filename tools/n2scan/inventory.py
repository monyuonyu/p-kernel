#!/usr/bin/env python3
"""N2 (wire v2, 16-bit node ids): where the tree assumes an 8-bit node id.

Static and heuristic (regex over the source, not a C parser): it finds the
shapes below and prints a markdown table. It can MISS things (a node id kept
in a field whose name has none of the words below, ids written byte by byte
into a buffer) and it can over-count (a UB named *src* that is not a node).
It is a starting list for N2, not a proof that nothing else is 8-bit.

  python3 tools/n2scan/inventory.py [repo]          # table to stdout
"""
import os
import re
import sys
import collections

REPO = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else
                       os.path.join(os.path.dirname(__file__), '..', '..'))
SKIP = ('/mtk3', '/vendor', '/third_party', '/.git', '/samples/', '/tests/',
        '/llm/', '/android/')
NODE_WORDS = r'(node|nid|peer|src|dst|owner|origin|coord|leader|sender|teacher|heard_from|target|voter|member)'
U8 = r'(UB|U1|uint8_t|unsigned char)'

STRUCT = re.compile(r'typedef\s+struct\s*\w*\s*\{(.*?)\}\s*(__attribute__\s*\(\(\s*packed\s*\)\))?\s*(\w+)\s*;', re.S)
FIELD = re.compile(r'\b' + U8 + r'\s+(\w*' + NODE_WORDS + r'\w*)\s*(\[[^\]]*\])?\s*;', re.I)
VERSION_DROP = re.compile(r'(?:version|wire_ver|\bver)\s*!=\s*(\w+_VER(?:SION)?\w*)\b')
KINDS = {
    'u8 node var/field (all)': re.compile(r'\b' + U8 + r'\s+\**\s*\w*' + NODE_WORDS + r'\w*\b(?!\s*\()', re.I),
    'u8 node parameter': re.compile(r'[(,]\s*(const\s+)?' + U8 + r'\s+\w*' + NODE_WORDS + r'\w*\s*[,)]', re.I),
    '0xFF as "no node"': re.compile(NODE_WORDS + r'\w*\s*[!=]=\s*0x[Ff][Ff]\b|0x[Ff][Ff]\s*[;)]?\s*/\*[^*]*(node|not.a.node|seen here)', re.I),
    'array indexed by a node id': re.compile(r'\[\s*(\w+(->|\.))?\w*' + NODE_WORDS + r'\w*\s*\]', re.I),
    'DNODE_MAX': re.compile(r'\bDNODE_MAX\b'),
    'MAC/IP last octet = node': re.compile(r'mac\[5\]|52:54:00:00:00', re.I),
}


def walk():
    for root, dirs, files in os.walk(REPO):
        rel = root[len(REPO):] + '/'
        if any(s in rel for s in SKIP):
            continue
        for f in sorted(files):
            if f.endswith(('.c', '.h')):
                p = os.path.join(root, f)
                yield p[len(REPO) + 1:], open(p, errors='replace').read()


def area(relp):
    parts = relp.split('/')
    return '/'.join(parts[:2]) if parts[0] in ('arch', 'boot') else parts[0]


def main():
    wire, drops = [], []
    count = collections.defaultdict(collections.Counter)
    for relp, txt in walk():
        for m in STRUCT.finditer(txt):
            # a wire/disk shape: packed, or carries a magic, or is named like one
            if not (m.group(2) or re.search(r'\bmagic\b', m.group(1))
                    or re.search(r'(PKT|MSG|ANN|ANNOUNCE|BEACON|HDR|MANIFEST)$', m.group(3))):
                continue
            fs = [g.group(2) + (g.group(4) or '') for g in FIELD.finditer(m.group(1))]
            fs = [f for f in fs if not re.match(r'(dst|src)\[6\]', f)]   # MAC addresses
            if fs:
                wire.append((relp, txt[:m.start()].count('\n') + 1, m.group(3), fs))
        for i, ln in enumerate(txt.split('\n'), 1):
            if ln.lstrip().startswith(('*', '/*', '//')):
                continue
            g = VERSION_DROP.search(ln)
            if g:
                drops.append((relp, i, g.group(1)))
            for k, rx in KINDS.items():
                if rx.search(ln):
                    count[k][area(relp)] += 1

    print('## 1. Wire/disk structs (packed, or with a magic, or named *_PKT etc.) with an 8-bit node field\n')
    print('| file:line | struct | 8-bit node fields |')
    print('|---|---|---|')
    for relp, ln, name, fs in sorted(wire):
        print(f'| {relp}:{ln} | {name} | {", ".join(fs)} |')
    print(f'\n{len(wire)} structs in {len(set(w[0] for w in wire))} files.\n')

    print('## 2. Receivers that drop a packet whose version byte is not theirs\n')
    print('| file:line | constant |')
    print('|---|---|')
    for relp, ln, c in sorted(drops):
        print(f'| {relp}:{ln} | {c} |')
    print(f'\n{len(drops)} places.\n')

    areas = sorted({a for k in count for a in count[k]})
    print('## 3. Line counts by shape and area (comments skipped)\n')
    print('| shape | ' + ' | '.join(areas) + ' | total |')
    print('|---|' + '---|' * (len(areas) + 1))
    for k in KINDS:
        row = [str(count[k].get(a, 0)) for a in areas]
        print(f'| {k} | ' + ' | '.join(row) + f' | {sum(count[k].values())} |')


if __name__ == '__main__':
    main()
