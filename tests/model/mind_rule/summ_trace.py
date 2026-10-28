#!/usr/bin/env python3
# summ_trace.py <TLC output>: one line per state of a TLC counterexample
# (held per node as v<value>(s<stamp>,t<teacher>), then the network). The
# files in cex/ were made with it from run_tlc.py outputs.
import re, sys
txt = open(sys.argv[1]).read()
parts = re.split(r'\nState (\d+): <([^>]*)>', txt)
for i in range(1, len(parts) - 2, 3):
    no, act, body = parts[i], parts[i + 1], parts[i + 2]
    act = act.split(' line')[0]
    def grab(name):
        m = re.search(r'/\\ ' + name + r' = (.*?)(?=\n/\\ |\n\n|\Z)', body, re.S)
        return re.sub(r'\s+', ' ', m.group(1)) if m else '?'
    held = grab('held')
    held = re.sub(r'\[v \|-> (\d+), s \|-> (\d+), t \|-> (\d+)\]', r'v\1(s\2,t\3)', held)
    net = grab('net')
    net = re.sub(r'\[v \|-> (\d+), s \|-> (\d+), o \|-> (\d+), to \|-> (\d+), k \|-> (\d+)\]',
                 r'to\4:v\1(s\2,o\3)', net)
    net = re.sub(r'\[to \|-> (\d+), o \|-> (\d+), k \|-> (\d+), s \|-> (\d+), v \|-> (\d+)\]',
                 r'to\1:v\5(s\4,o\2)', net)
    print(f'{no:>3} {act:<10} held {held}')
    print(f'    net {net}  last {grab("last")}  ndrop {grab("nDrop")} nevict {grab("nEvict")}')
