#!/usr/bin/env python3
"""Write the keyboard layout files in root/usr/share/keymaps/.

usage: mkkeymaps.py <output-dir>

A column is a character, "-" for nothing or dead:<accent>. Dead keys are
combined with a following letter by the "compose" lines at the end.
"""
import os
import sys
import unicodedata

ROWS = [
    ['grave', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 'minus', 'equal'],
    ['q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', 'lbrace', 'rbrace'],
    ['a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 'semicolon', 'apostrophe', 'backslash'],
    ['102nd', 'z', 'x', 'c', 'v', 'b', 'n', 'm', 'comma', 'dot', 'slash'],
]
COMBINING = {'acute': '́', 'grave': '̀', 'circumflex': '̂', 'diaeresis': '̈', 'tilde': '̃'}


def letters(keys):
    return {k: (k, k.upper(), '-', '-') for k in keys if len(k) == 1 and k.isalpha()}


def layout(overrides, base=None):
    m = letters([k for r in ROWS for k in r])
    if base:
        m.update(base)
    m.update(overrides)
    return m


def spec(s):
    """'a A @ -' -> tuple"""
    t = tuple(s.split())
    return t + ('-',) * (4 - len(t))


def table(rows):
    return {k: spec(v) for k, v in rows.items()}


US = table({
    'grave': '` ~', '1': '1 !', '2': '2 @', '3': '3 #', '4': '4 $', '5': '5 %', '6': '6 ^', '7': '7 &', '8': '8 *',
    '9': '9 (', '0': '0 )', 'minus': '- _', 'equal': '= +', 'lbrace': '[ {', 'rbrace': '] }', 'semicolon': '; :',
    'apostrophe': "' \"", 'backslash': '\\ |', '102nd': '\\ |', 'comma': ', <', 'dot': '. >', 'slash': '/ ?',
})

DVORAK = table({
    'minus': '[ {', 'equal': '] }', 'q': "' \"", 'w': ', <', 'e': '. >', 'r': 'p P', 't': 'y Y', 'y': 'f F',
    'u': 'g G', 'i': 'c C', 'o': 'r R', 'p': 'l L', 'lbrace': '/ ?', 'rbrace': '= +', 's': 'o O', 'd': 'e E',
    'f': 'u U', 'g': 'i I', 'h': 'd D', 'j': 'h H', 'k': 't T', 'l': 'n N', 'semicolon': 's S',
    'apostrophe': '- _', 'z': '; :', 'x': 'q Q', 'c': 'j J', 'v': 'k K', 'b': 'x X', 'n': 'b B', 'comma': 'w W',
    'dot': 'v V', 'slash': 'z Z',
})

DE = table({
    'grave': 'dead:circumflex °', '2': '2 " ²', '3': '3 § ³', '4': '4 $', '5': '5 %', '6': '6 &', '7': '7 / {',
    '8': '8 ( [', '9': '9 ) ]', '0': '0 = }', 'minus': 'ß ? \\', 'equal': 'dead:acute dead:grave', '1': '1 !',
    'e': 'e E €', 'q': 'q Q @', 'y': 'z Z', 'z': 'y Y', 'lbrace': 'ü Ü', 'rbrace': '+ * ~',
    'semicolon': 'ö Ö', 'apostrophe': 'ä Ä', 'backslash': "# '", '102nd': '< > |', 'm': 'm M µ',
    'comma': ', ;', 'dot': '. :', 'slash': '- _', 'kpdot': ',',
})

GB = table({
    'grave': '` ¬ ¦', '1': '1 !', '2': '2 "', '3': '3 £', '4': '4 $ €', '5': '5 %', '6': '6 ^', '7': '7 &',
    '8': '8 *', '9': '9 (', '0': '0 )', 'minus': '- _', 'equal': '= +', 'lbrace': '[ {', 'rbrace': '] }',
    'semicolon': '; :', 'apostrophe': "' @", 'backslash': '# ~', '102nd': '\\ |', 'comma': ', <', 'dot': '. >',
    'slash': '/ ?', 'a': 'a A á Á', 'e': 'e E é É', 'i': 'i I í Í', 'o': 'o O ó Ó', 'u': 'u U ú Ú',
})

FR = table({
    'grave': '²', '1': '& 1', '2': 'é 2 ~', '3': '" 3 #', '4': "' 4 {", '5': '( 5 [', '6': '- 6 |', '7': 'è 7 `',
    '8': '_ 8 \\', '9': 'ç 9 ^', '0': 'à 0 @', 'minus': ') ° ]', 'equal': '= + }', 'q': 'a A', 'w': 'z Z',
    'e': 'e E €', 'lbrace': 'dead:circumflex dead:diaeresis', 'rbrace': '$ £ ¤', 'a': 'q Q', 'semicolon': 'm M',
    'apostrophe': 'ù %', 'backslash': '* µ', '102nd': '< >', 'z': 'w W', 'm': ', ?', 'comma': '; .', 'dot': ': /',
    'slash': '! §',
})

ES = table({
    'grave': 'º ª \\', '1': '1 ! |', '2': '2 " @', '3': '3 · #', '4': '4 $ ~', '5': '5 %', '6': '6 & ¬', '7': '7 /',
    '8': '8 (', '9': '9 )', '0': '0 =', 'minus': "' ?", 'equal': '¡ ¿', 'e': 'e E €',
    'lbrace': 'dead:grave dead:circumflex [', 'rbrace': '+ * ]', 'semicolon': 'ñ Ñ',
    'apostrophe': 'dead:acute dead:diaeresis {', 'backslash': 'ç Ç }', '102nd': '< >', 'comma': ', ;',
    'dot': '. :', 'slash': '- _',
})

IT = table({
    'grave': '\\ |', '1': '1 !', '2': '2 "', '3': '3 £', '4': '4 $', '5': '5 % €', '6': '6 &', '7': '7 /',
    '8': '8 (', '9': '9 )', '0': '0 =', 'minus': "' ?", 'equal': 'ì ^', 'lbrace': 'è é [ {', 'rbrace': '+ * ] }',
    'semicolon': 'ò ç @', 'apostrophe': 'à ° #', 'backslash': 'ù §', '102nd': '< >', 'comma': ', ;', 'dot': '. :',
    'slash': '- _',
})

LAYOUTS = {
    'us': ('English (US)', 'US', False, US, False),
    'gb': ('English (UK)', 'GB', True, GB, False),
    'dvorak': ('English (Dvorak)', 'DV', False, DVORAK, False),
    'de': ('Deutsch (QWERTZ)', 'DE', True, DE, True),
    'fr': ('Français (AZERTY)', 'FR', True, FR, True),
    'es': ('Español', 'ES', True, ES, True),
    'it': ('Italiano', 'IT', True, IT, False),
}


def compose_lines():
    out = []
    for dead, comb in COMBINING.items():
        for base in 'aeiouyAEIOUYnNc':
            c = unicodedata.normalize('NFC', base + comb)
            if len(c) == 1 and ord(c) < 0x100:
                out.append('compose %s %s U+%04X' % (dead, base, ord(c)))
    return out


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else 'root/usr/share/keymaps'
    os.makedirs(outdir, exist_ok=True)
    for lid, (name, short, altgr, rows, dead) in LAYOUTS.items():
        m = layout(rows, rows)
        lines = ['#name=' + name, '#short=' + short, '#altgr=' + ('yes' if altgr else 'no'),
                 '# key  plain  shift  AltGr  AltGr+Shift   (- = nothing)']
        for row in ROWS:
            for k in row:
                if k in m:
                    lines.append('%-10s %s' % (k, '  '.join(m[k])))
        lines.append('%-10s %s' % ('space', '␣'.replace('␣', 'U+0020') + '  U+0020'))
        if 'kpdot' in m:
            lines.append('%-10s %s' % ('kpdot', m['kpdot'][0]))
        if dead:
            lines += compose_lines()
        with open(os.path.join(outdir, lid + '.map'), 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines) + '\n')


main()
