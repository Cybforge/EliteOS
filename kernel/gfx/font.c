/*
 * Anti-aliased text from pre-rendered .zft fonts (see tools/mkfont.py).
 */
#include <gfx.h>
#include <mm.h>

struct PACKED zft_header {
    char magic[4];
    uint16_t version, size;
    int16_t ascent, descent, line_height;
    uint16_t nglyphs;
    uint32_t bitmap_off;
};

struct PACKED zft_glyph {
    uint32_t cp;
    int16_t bx, by;
    uint16_t w, h;
    int16_t advance;
    uint16_t pad;
    uint32_t off;
};

struct font {
    const struct zft_header *hdr;
    const struct zft_glyph *glyphs;
    const uint8_t *bitmaps;
    int16_t latin[256];     /* glyph index for cp < 256, -1 if none */
    struct font *fallback;
};

#define FONT_BLOB(name) \
    extern const uint8_t _font_##name[]; \
    static struct font f_##name;

FONT_BLOB(sans13)
FONT_BLOB(sans15)
FONT_BLOB(sans18)
FONT_BLOB(medium13)
FONT_BLOB(medium15)
FONT_BLOB(bold20)
FONT_BLOB(light30)
FONT_BLOB(light64)
FONT_BLOB(mono14)
FONT_BLOB(monob14)
FONT_BLOB(chess54)

font_t *font_ui, *font_ui_md, *font_ui_lg, *font_ui_bold, *font_title,
       *font_bold_lg, *font_light, *font_huge, *font_mono, *font_mono_bold, *font_chess;

static uint8_t cov_lut[256];

static void load(struct font *f, const uint8_t *blob, struct font *fallback)
{
    f->hdr = (const struct zft_header *)blob;
    if (memcmp(f->hdr->magic, "ZFNT", 4)) panic("font: bad magic");
    f->glyphs = (const struct zft_glyph *)(blob + sizeof(struct zft_header));
    f->bitmaps = blob + f->hdr->bitmap_off;
    for (int i = 0; i < 256; i++) f->latin[i] = -1;
    for (int i = 0; i < f->hdr->nglyphs; i++)
        if (f->glyphs[i].cp < 256) f->latin[f->glyphs[i].cp] = (int16_t)i;
    f->fallback = fallback;
}

void fonts_init(void)
{
    load(&f_mono14, _font_mono14, NULL);
    load(&f_monob14, _font_monob14, &f_mono14);
    load(&f_sans13, _font_sans13, &f_mono14);
    load(&f_sans15, _font_sans15, &f_mono14);
    load(&f_sans18, _font_sans18, &f_mono14);
    load(&f_medium13, _font_medium13, &f_mono14);
    load(&f_medium15, _font_medium15, &f_mono14);
    load(&f_bold20, _font_bold20, &f_sans18);
    load(&f_light30, _font_light30, &f_sans18);
    load(&f_light64, _font_light64, &f_light30);
    load(&f_chess54, _font_chess54, &f_light64);

    font_ui = &f_sans13;
    font_ui_md = &f_medium13;
    font_ui_lg = &f_sans15;
    font_ui_bold = &f_medium15;
    font_title = &f_sans18;
    font_bold_lg = &f_bold20;
    font_light = &f_light30;
    font_huge = &f_light64;
    font_mono = &f_mono14;
    font_mono_bold = &f_monob14;
    font_chess = &f_chess54;

    /* slight gamma boost so light text on dark backgrounds does not look thin */
    for (int i = 0; i < 256; i++)
        cov_lut[i] = (uint8_t)(k_pow(i / 255.0, 0.80) * 255.0 + 0.5);
}

int font_height(font_t *f) { return f->hdr->line_height; }
int font_ascent(font_t *f) { return f->hdr->ascent; }

static const struct zft_glyph *find_glyph(font_t *f, uint32_t cp, font_t **owner)
{
    for (font_t *cur = f; cur; cur = cur->fallback) {
        if (cp < 256) {
            int i = cur->latin[cp];
            if (i >= 0) { *owner = cur; return &cur->glyphs[i]; }
            continue;
        }
        int lo = 0, hi = cur->hdr->nglyphs - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            uint32_t c = cur->glyphs[mid].cp;
            if (c == cp) { *owner = cur; return &cur->glyphs[mid]; }
            if (c < cp) lo = mid + 1;
            else hi = mid - 1;
        }
    }
    /* last resort: '?' from the requested font */
    if (f->latin['?'] >= 0) { *owner = f; return &f->glyphs[f->latin['?']]; }
    return NULL;
}

/* ------------------------------------------------------------------------
 * UTF-8
 * ---------------------------------------------------------------------- */

uint32_t utf8_next(const char **sp)
{
    const uint8_t *s = (const uint8_t *)*sp;
    uint32_t c = *s++;
    if (c < 0x80) { *sp = (const char *)s; return c; }
    int n = 0;
    if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
    else { *sp = (const char *)s; return 0xFFFD; }
    while (n--) {
        if ((*s & 0xC0) != 0x80) { *sp = (const char *)s; return 0xFFFD; }
        c = (c << 6) | (*s++ & 0x3F);
    }
    *sp = (const char *)s;
    return c;
}

int utf8_encode(uint32_t cp, char *o)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* length in bytes of the UTF-8 character that ends right before p */
int utf8_prev_len(const char *start, const char *p)
{
    int n = 0;
    while (p > start) {
        p--;
        n++;
        if ((*(const uint8_t *)p & 0xC0) != 0x80) break;
    }
    return n;
}

/* ------------------------------------------------------------------------
 * measuring and drawing
 * ---------------------------------------------------------------------- */

int font_char_width(font_t *f, uint32_t cp)
{
    font_t *o;
    const struct zft_glyph *g = find_glyph(f, cp, &o);
    return g ? g->advance : 0;
}

int font_text_width_n(font_t *f, const char *s, int bytes)
{
    const char *end = s + bytes;
    int w = 0;
    while (s < end && *s) w += font_char_width(f, utf8_next(&s));
    return w;
}

int font_text_width(font_t *f, const char *s)
{
    int w = 0;
    while (*s) w += font_char_width(f, utf8_next(&s));
    return w;
}

int gfx_char(surface_t *s, font_t *f, int x, int y, uint32_t cp, color_t c)
{
    font_t *o;
    const struct zft_glyph *g = find_glyph(f, cp, &o);
    if (!g) return x;
    int gx = x + g->bx;
    /* baseline of the requested font, so fallback glyphs line up */
    int gy = y + f->hdr->ascent - g->by;
    const uint8_t *bm = o->bitmaps + g->off;
    rect_t cl = s->clip;
    int x0 = MAX(gx, cl.x), x1 = MIN(gx + g->w, cl.x + cl.w);
    int y0 = MAX(gy, cl.y), y1 = MIN(gy + g->h, cl.y + cl.h);
    uint32_t ca = C_A(c);
    for (int yy = y0; yy < y1; yy++) {
        const uint8_t *row = bm + (yy - gy) * g->w;
        uint32_t *d = s->px + (size_t)yy * s->stride;
        for (int xx = x0; xx < x1; xx++) {
            uint32_t cov = row[xx - gx];
            if (!cov) continue;
            uint32_t a = cov_lut[cov];
            if (ca != 255) a = a * ca / 255;
            d[xx] = a >= 255 ? (c | 0xFF000000u) : blend(d[xx], c, a);
        }
    }
    return x + g->advance;
}

int gfx_text_n(surface_t *s, font_t *f, int x, int y, const char *t, int bytes, color_t c)
{
    const char *end = t + bytes;
    while (t < end && *t) {
        uint32_t cp = utf8_next(&t);
        x = gfx_char(s, f, x, y, cp, c);
        if (x > s->clip.x + s->clip.w) break;
    }
    return x;
}

int gfx_text(surface_t *s, font_t *f, int x, int y, const char *t, color_t c)
{
    while (*t) {
        uint32_t cp = utf8_next(&t);
        x = gfx_char(s, f, x, y, cp, c);
        if (x > s->clip.x + s->clip.w) break;
    }
    return x;
}

void gfx_text_center(surface_t *s, font_t *f, rect_t r, const char *t, color_t c)
{
    int w = font_text_width(f, t);
    int h = f->hdr->ascent + f->hdr->descent;
    gfx_text(s, f, r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, t, c);
}

void gfx_text_right(surface_t *s, font_t *f, int rx, int y, const char *t, color_t c)
{
    gfx_text(s, f, rx - font_text_width(f, t), y, t, c);
}

int gfx_text_ellipsis(surface_t *s, font_t *f, int x, int y, int max_w, const char *t, color_t c)
{
    int w = font_text_width(f, t);
    if (w <= max_w) return gfx_text(s, f, x, y, t, c);
    int dots = font_text_width(f, "…");
    const char *p = t;
    int acc = 0;
    while (*p) {
        const char *q = p;
        int cw = font_char_width(f, utf8_next(&q));
        if (acc + cw + dots > max_w) break;
        acc += cw;
        p = q;
    }
    x = gfx_text_n(s, f, x, y, t, (int)(p - t), c);
    return gfx_text(s, f, x, y, "…", c);
}

/* word-wrapped text inside r; returns height used */
int gfx_text_wrap(surface_t *s, font_t *f, rect_t r, const char *t, color_t c, int gap)
{
    int lh = f->hdr->line_height + gap;
    int y = r.y;
    while (*t) {
        /* find the longest run of words that fits */
        const char *p = t, *brk = NULL;
        int w = 0;
        while (*p && *p != '\n') {
            const char *q = p;
            uint32_t cp = utf8_next(&q);
            int cw = font_char_width(f, cp);
            if (w + cw > r.w && brk) break;
            if (w + cw > r.w && !brk) { brk = p; break; }
            if (cp == ' ') brk = q;
            w += cw;
            p = q;
        }
        const char *end = (*p == 0 || *p == '\n') ? p : brk;
        if (!end || end == t) end = p == t ? t + 1 : p;
        if (s) gfx_text_n(s, f, r.x, y, t, (int)(end - t), c);
        y += lh;
        t = end;
        if (*t == '\n') t++;
        while (*t == ' ') t++;
    }
    return y - r.y;
}
