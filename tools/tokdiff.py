#!/usr/bin/env python3
"""Formatting-insensitive diff of two C++ files (e.g. Xenia Canary vs ReXGlue).
Strips comments/includes, tokenizes, applies namespace/rename normalisation,
then prints differing token runs with the line numbers in both files.
Usage: tokdiff.py canary.cc rexglue.cpp [context_tokens]"""
import re, sys, difflib

TOK = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|0[xX][0-9a-fA-F]+[uUlL]*|\d+\.?\d*(?:[eE][-+]?\d+)?[fFuUlL]*|::|->|<<=?|>>=?|[-+*/%&|^!=<>]=|&&|\|\||\+\+|--|\S')
RENAMES = {'xe': 'NS', 'rex': 'NS', 'gpu': '', 'graphics': ''}

def toks(path):
    src = open(path, encoding='utf-8', errors='replace').read()
    src = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group().count('\n'), src, flags=re.S)
    src = re.sub(r'//[^\n]*', '', src)
    out = []
    for ln, line in enumerate(src.split('\n'), 1):
        s = line.strip()
        if s.startswith('#include') or s.startswith('#pragma'):
            continue
        for t in TOK.findall(line):
            t = RENAMES.get(t, t)
            if t:
                out.append((t, ln))
    return out

a, b = toks(sys.argv[1]), toks(sys.argv[2])
ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 8
sm = difflib.SequenceMatcher(None, [t for t, _ in a], [t for t, _ in b], autojunk=False)
for tag, i1, i2, j1, j2 in sm.get_opcodes():
    if tag == 'equal':
        continue
    la = a[i1][1] if i1 < len(a) else a[-1][1]
    lb = b[j1][1] if j1 < len(b) else b[-1][1]
    pre = ' '.join(t for t, _ in a[max(0, i1 - ctx):i1])
    print(f'--- {tag} canary:{la} rex:{lb}   ...{pre}')
    if i2 > i1: print('  C: ' + ' '.join(t for t, _ in a[i1:i2])[:1500])
    if j2 > j1: print('  R: ' + ' '.join(t for t, _ in b[j1:j2])[:1500])
