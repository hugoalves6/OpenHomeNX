#!/usr/bin/env python3
"""check_selfcall.py — Passo A della verifica obbligatoria pre-commit.

Cerca funzioni UI::X il cui corpo richiama X (ricorsione quasi sempre
involontaria: il caso reale era popupRow() -> popupRow(), che -O2
trasforma in loop infinito senza crash). I commenti sono esclusi.

Uso: python3 tools/check_selfcall.py  (exit 1 se trova auto-chiamate)
"""
import re
import glob
import sys

files = (glob.glob('source/*.cpp') + glob.glob('platform/r36s/stub/*.cpp') +
         glob.glob('include/*.h'))
bad = []
for p in files:
    try:
        src = open(p).read()
    except OSError:
        continue
    src = re.sub(r'//.*', '', src)
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
    for m in re.finditer(r'^(?:[\w:<>]+[\s\*&]+)+UI::(\w+)\s*\(', src, re.M):
        name = m.group(1)
        i = src.index('{', m.end())
        depth = 0
        for j in range(i, len(src)):
            if src[j] == '{':
                depth += 1
            elif src[j] == '}':
                depth -= 1
                if depth == 0:
                    if re.search(r'(?<![\w:>.])' + re.escape(name) + r'\s*\(', src[i:j]):
                        bad.append('%s: UI::%s' % (p, name))
                    break
if bad:
    print('SELF-CALLS TROVATE:')
    for b in bad:
        print('  ' + b)
    sys.exit(1)
print('ok: nessuna auto-chiamata')
