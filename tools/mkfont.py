#!/usr/bin/env python3
"""
Bake TrueType fonts into ZenithOS .zft files (8-bit anti-aliased glyph
bitmaps + metrics). The kernel embeds these; no font rasterizer runs at
boot.

usage: mkfont.py <font.ttf> <pixel-size> <charset> <out.zft>
charset: full | mono | ascii | digits | chess

Format (little endian):
  header  : "ZFNT" u16 version u16 size i16 ascent i16 descent i16 line_height
            u16 nglyphs u32 bitmap_offset
  glyph[] : u32 codepoint i16 bx i16 by u16 w u16 h i16 advance u16 pad u32 offset
  bitmaps : w*h bytes each (coverage 0..255)
Glyphs are sorted by codepoint.
"""
import struct
import sys

import freetype


def charset(name):
    ascii_ = list(range(0x20, 0x7F))
    latin1 = list(range(0xA0, 0x100))
    extra = [0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026,
             0x20AC, 0x2122, 0x2190, 0x2191, 0x2192, 0x2193, 0x2212, 0x2713,
             0x2715, 0x25B2, 0x25B6, 0x25BC, 0x25C0, 0x25CF, 0x2605, 0x2630,
             0x276F, 0x279C, 0x03BB, 0x2714, 0x2718, 0x2665, 0x2699, 0x26A1, 0x03C0,
             0x221A, 0x00B1, 0x2264, 0x2265, 0x2260, 0x221E, 0x2211,
             0x2039, 0x203A, 0x25B8, 0x25BE, 0x2303, 0x21B5, 0x23F8, 0x23F5, 0x232B, 0x2326]
    if name == 'ascii':
        return ascii_
    if name == 'chess':
        return [0x20] + list(range(0x2654, 0x2660))
    if name == 'digits':
        return [ord(c) for c in " 0123456789:.,-+%/°APMapm"]
    if name == 'full':
        return ascii_ + latin1 + extra
    if name == 'mono':
        return (ascii_ + latin1 + extra + list(range(0x2500, 0x2580)) +
                list(range(0x2580, 0x25A0)) + list(range(0x25A0, 0x2600)))
    raise SystemExit('unknown charset ' + name)


def main():
    path, size, cs, out = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    face = freetype.Face(path)
    face.set_pixel_sizes(0, size)
    flags = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_LIGHT

    glyphs = []
    blob = bytearray()
    for cp in sorted(set(charset(cs))):
        if face.get_char_index(cp) == 0 and cp != 0x20:
            continue
        face.load_char(cp, flags)
        g = face.glyph
        bm = g.bitmap
        w, h = bm.width, bm.rows
        data = bytearray()
        for row in range(h):
            data += bytes(bm.buffer[row * bm.pitch: row * bm.pitch + w])
        adv = (g.advance.x + 32) >> 6
        glyphs.append((cp, g.bitmap_left, g.bitmap_top, w, h, adv, len(blob)))
        blob += data

    m = face.size
    ascent = (m.ascender + 63) >> 6
    descent = -((m.descender) >> 6)
    line_h = (m.height + 63) >> 6
    hdr_size = 4 + 2 * 2 + 2 * 3 + 2 + 4
    glyph_size = 4 + 2 * 2 + 2 * 2 + 2 + 2 + 4
    bitmap_off = hdr_size + glyph_size * len(glyphs)
    with open(out, 'wb') as f:
        f.write(b'ZFNT')
        f.write(struct.pack('<HHhhhHI', 1, size, ascent, descent, line_h, len(glyphs), bitmap_off))
        for cp, bx, by, w, h, adv, off in glyphs:
            f.write(struct.pack('<IhhHHhHI', cp, bx, by, w, h, adv, 0, off))
        f.write(blob)
    print(f'{out}: {len(glyphs)} glyphs, {len(blob)} bytes, ascent {ascent} descent {descent} line {line_h}')


if __name__ == '__main__':
    main()
