#!/usr/bin/env python3
"""List every translatable string in the kernel sources: tr("..."), _("...") and N_("...").

usage: i18n-extract.py [--missing po/de.tsv]
"""
import re
import sys
import glob

PAT = re.compile(r'(?<![A-Za-z0-9_])(?:tr|_|N_)\(\s*"((?:[^"\\]|\\.)*)"\s*\)')
strings = {}
for f in sorted(glob.glob('kernel/**/*.c', recursive=True)):
    for m in PAT.finditer(open(f, encoding='utf-8').read()):
        strings.setdefault(m.group(1), f)


for f in sorted(glob.glob('root/usr/share/applications/*.zapp')):
    for line in open(f, encoding='utf-8'):
        if line.startswith(('name=', 'comment=')):
            strings.setdefault(line.split('=', 1)[1].strip(), f)


def unescape(s):
    return s.replace('\\n', '\n').replace('\\"', '"').replace('\\\\', '\\')


have = set()
if len(sys.argv) > 2 and sys.argv[1] == '--missing':
    for line in open(sys.argv[2], encoding='utf-8'):
        if line.startswith('#') or '\t' not in line:
            continue
        have.add(line.split('\t')[0])
for s, f in strings.items():
    if s not in have:
        print(s + '\t' + f)
