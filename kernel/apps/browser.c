/*
 * Zenith Web: a small HTML browser on top of the ZenithOS TCP/IP stack.
 * It speaks HTTP/1.0 (no TLS), follows redirects, and renders headings,
 * paragraphs, links, lists, preformatted text, quotes and simple tables.
 */
#include <wm.h>
#include <sys.h>
#include <mm.h>
#include <net.h>
#include <sched.h>
#include <vfs.h>
#include <tty.h>
#include <image.h>

enum { ST_BODY, ST_H1, ST_H2, ST_H3, ST_PRE, ST_SMALL, ST_BOLD, ST_COUNT };
#define ST_IMAGE 254
#define ST_RULE 255

struct box {
    int x, y, w;
    int16_t h;              /* images only */
    uint8_t style;
    bool italic;
    int16_t link;           /* -1 = none */
    uint32_t off;           /* into page text pool; image index for ST_IMAGE */
    uint16_t len;
};

#define MAX_IMGS 48

enum { IMG_PENDING, IMG_OK, IMG_FAILED };

struct pimg {
    char *url;
    surface_t *img;
    int aw, ah;             /* width/height attributes, 0 if absent */
    volatile int state;
};

struct item {
    uint8_t kind;           /* 0 text, 1 line break, 2 block break, 3 rule, 4 bullet, 5 image */
    uint8_t style;
    bool italic;
    bool pre;
    int16_t link;
    int8_t indent;
    int8_t space;           /* block break spacing in px/4 */
    uint32_t off, len;
};

#define MAX_ITEMS 8000
#define MAX_BOXES 20000
#define MAX_LINKS 1500

struct page {
    char url[512];
    char title[128];
    char *text;             /* pool for item text */
    size_t tlen, tcap;
    struct item *items;
    int nitems;
    char *links[MAX_LINKS];
    int nlinks;
    struct box *boxes;
    int nboxes;
    int layout_w;
    int height;
    struct pimg imgs[MAX_IMGS];
    int nimgs;
};

struct browser {
    struct page page;
    char addr[512];
    char history[32][512];
    int hpos, hlen;
    int scroll;
    int hover_link;
    volatile int loading;       /* 0 idle, 1 loading, 2 done (page ready) */
    volatile bool fetch_active; /* fetch thread still references this struct */
    char status[160];
    char *pending_body;
    size_t pending_len;
    char pending_url[512];
    bool pending_plain;
    char pending_tls[96];
    char tls[96];                   /* security of the page on screen, "" = not encrypted */
    app_t *app;
    volatile bool closed;
    /* images load in the background; a new page bumps the generation */
    mutex_t img_lock;
    volatile int page_gen;
    volatile int img_threads;
};

/* ------------------------------------------------------------------------
 * HTML -> items
 * ---------------------------------------------------------------------- */

static uint32_t pool_add(struct page *p, const char *s, size_t n)
{
    if (p->tlen + n + 1 > p->tcap) {
        size_t cap = MAX(p->tcap * 2, p->tlen + n + 4096);
        char *t = krealloc(p->text, cap);
        if (!t) return 0;
        p->text = t;
        p->tcap = cap;
    }
    memcpy(p->text + p->tlen, s, n);
    uint32_t off = (uint32_t)p->tlen;
    p->tlen += n;
    return off;
}

static struct item *add_item(struct page *p, int kind)
{
    if (p->nitems >= MAX_ITEMS) return NULL;
    struct item *it = &p->items[p->nitems++];
    memset(it, 0, sizeof(*it));
    it->kind = (uint8_t)kind;
    it->link = -1;
    return it;
}

static void block(struct page *p, int space, int indent)
{
    if (p->nitems && p->items[p->nitems - 1].kind == 2) {
        struct item *last = &p->items[p->nitems - 1];
        last->space = (int8_t)MAX(last->space, space);
        last->indent = (int8_t)indent;
        return;
    }
    struct item *it = add_item(p, 2);
    if (it) { it->space = (int8_t)space; it->indent = (int8_t)indent; }
}

static int decode_entity(const char *s, char *out)
{
    /* s points after '&'; returns bytes consumed including ';' (0 if unknown) */
    static const struct { const char *n; uint32_t cp; } ents[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", ' ' },
        { "copy", 0xA9 }, { "reg", 0xAE }, { "euro", 0x20AC }, { "mdash", 0x2014 }, { "ndash", 0x2013 },
        { "hellip", 0x2026 }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "auml", 0xE4 }, { "ouml", 0xF6 },
        { "uuml", 0xFC }, { "Auml", 0xC4 }, { "Ouml", 0xD6 }, { "Uuml", 0xDC }, { "szlig", 0xDF },
        { "middot", 0xB7 }, { "bull", 0x2022 }, { "rsquo", 0x2019 }, { "lsquo", 0x2018 }, { "rdquo", 0x201D },
        { "ldquo", 0x201C }, { "times", 0xD7 }, { "deg", 0xB0 }, { "eacute", 0xE9 }, { "egrave", 0xE8 },
    };
    uint32_t cp = 0;
    int n = 0;
    if (s[0] == '#') {
        int base = 10, i = 1;
        if (s[1] == 'x' || s[1] == 'X') { base = 16; i = 2; }
        char *end;
        cp = (uint32_t)strtol(s + i, &end, base);
        if (*end != ';') return 0;
        n = (int)(end - s) + 1;
    } else {
        for (unsigned k = 0; k < ARRAY_SIZE(ents); k++) {
            size_t l = strlen(ents[k].n);
            if (!strncmp(s, ents[k].n, l) && s[l] == ';') { cp = ents[k].cp; n = (int)l + 1; break; }
        }
        if (!n) return 0;
    }
    if (cp == 0xA0) cp = ' ';
    return utf8_encode(cp ? cp : '?', out) ? n : 0;
}

static void add_text(struct page *p, const char *s, size_t n, int style, bool italic, bool pre, int link)
{
    /* decode entities and collapse whitespace (except in <pre>) */
    char *buf = kmalloc(n * 3 + 8);
    size_t o = 0;
    bool space = p->nitems && p->items[p->nitems - 1].kind == 0 && p->tlen &&
                 p->text[p->items[p->nitems - 1].off + p->items[p->nitems - 1].len - 1] == ' ';
    if (!p->nitems || p->items[p->nitems - 1].kind != 0) space = true;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '&') {
            char e[5];
            int used = decode_entity(s + i + 1, e);
            if (used) {
                const char *pe = e;
                uint32_t cp = utf8_next(&pe);
                char enc[4];
                int k = utf8_encode(cp, enc);
                if (cp == ' ' && !pre) { if (!space) { buf[o++] = ' '; space = true; } }
                else { memcpy(buf + o, enc, (size_t)k); o += (size_t)k; space = false; }
                i += (size_t)used;
                continue;
            }
        }
        if (!pre && (c == ' ' || c == '\n' || c == '\r' || c == '\t')) {
            if (!space) { buf[o++] = ' '; space = true; }
            continue;
        }
        if (pre && c == '\r') continue;
        if (pre && c == '\n') {
            if (o) {
                struct item *it = add_item(p, 0);
                if (it) {
                    it->off = pool_add(p, buf, o); it->len = (uint32_t)o;
                    it->style = (uint8_t)style; it->italic = italic; it->pre = true; it->link = (int16_t)link;
                }
                o = 0;
            }
            add_item(p, 1);
            continue;
        }
        buf[o++] = c;
        space = false;
    }
    if (o) {
        struct item *it = add_item(p, 0);
        if (it) {
            it->off = pool_add(p, buf, o);
            it->len = (uint32_t)o;
            it->style = (uint8_t)style;
            it->italic = italic;
            it->pre = pre;
            it->link = (int16_t)link;
        }
    }
    kfree(buf);
}

static bool tag_is(const char *tag, size_t tl, const char *name)
{
    size_t n = strlen(name);
    return tl == n && !strncasecmp(tag, name, n);
}

static bool attr(const char *a, size_t al, const char *name, char *out, size_t n)
{
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl < al; i++) {
        if (strncasecmp(a + i, name, nl) || (i && isalnum(a[i - 1]))) continue;
        size_t j = i + nl;
        while (j < al && a[j] == ' ') j++;
        if (j >= al || a[j] != '=') continue;
        j++;
        while (j < al && a[j] == ' ') j++;
        char q = (j < al && (a[j] == '"' || a[j] == '\'')) ? a[j++] : 0;
        size_t k = 0;
        while (j < al && k + 1 < n && (q ? a[j] != q : (a[j] != ' ' && a[j] != '>'))) out[k++] = a[j++];
        out[k] = 0;
        return true;
    }
    return false;
}

static void resolve_url(const char *base, const char *href, char *out, size_t n)
{
    if (!strncasecmp(href, "http://", 7) || !strncasecmp(href, "https://", 8) || !strncasecmp(href, "about:", 6) ||
        !strncasecmp(href, "file://", 7)) {
        strlcpy(out, href, n);
        return;
    }
    if (href[0] == '#' || !href[0]) {
        strlcpy(out, base, n);
        char *h = strchr(out, '#');
        if (h) *h = 0;
        return;
    }
    if (!strncmp(href, "//", 2)) { snprintf(out, n, "http:%s", href); return; }
    /* scheme + host of base */
    const char *hs = strstr(base, "://");
    hs = hs ? hs + 3 : base;
    const char *path = strchr(hs, '/');
    size_t origin_len = path ? (size_t)(path - base) : strlen(base);
    char origin[256];
    strlcpy(origin, base, MIN(origin_len + 1, sizeof(origin)));
    if (href[0] == '/') { snprintf(out, n, "%s%s", origin, href); return; }
    if (href[0] == '?') {
        char b[512];
        strlcpy(b, base, sizeof(b));
        char *q = strchr(b, '?');
        if (q) *q = 0;
        snprintf(out, n, "%s%s", b, href);
        return;
    }
    char dir[512];
    strlcpy(dir, path ? path : "/", sizeof(dir));
    char *q = strchr(dir, '?');
    if (q) *q = 0;
    char *last = strrchr(dir, '/');
    if (last) last[1] = 0;
    snprintf(out, n, "%s%s%s", origin, dir, href);
}

static void parse_html(struct page *p, const char *h, size_t n, bool plain)
{
    if (plain) {
        add_text(p, h, n, ST_PRE, false, true, -1);
        return;
    }
    int style = ST_BODY, link = -1, indent = 0;
    bool italic = false, pre = false;
    int skip = 0;                /* inside script/style/head */
    int list_depth = 0;
    int ol_num[8] = { 0 };       /* per nesting level: -1 bulleted, else the last <ol> number */
    size_t i = 0;
    while (i < n) {
        if (h[i] == '<') {
            if (!strncmp(h + i, "<!--", 4)) {
                const char *e = strstr(h + i + 4, "-->");
                i = e ? (size_t)(e - h) + 3 : n;
                continue;
            }
            size_t j = i + 1;
            bool closing = j < n && h[j] == '/';
            if (closing) j++;
            size_t ts = j;
            while (j < n && (isalnum(h[j]) || h[j] == '!')) j++;
            const char *tag = h + ts;
            size_t tl = j - ts;
            size_t as = j;
            while (j < n && h[j] != '>') j++;
            const char *attrs = h + as;
            size_t al = j - as;
            i = j + 1;

            if (tag_is(tag, tl, "script") || tag_is(tag, tl, "style")) {
                if (!closing) {
                    /* jump to the matching close tag */
                    char close[16];
                    snprintf(close, sizeof(close), "</%.*s", (int)tl, tag);
                    const char *e = strstr_ci(h + i, close);
                    i = e ? (size_t)(e - h) : n;
                }
                continue;
            }
            if (tag_is(tag, tl, "title") && !closing) {
                const char *e = strstr_ci(h + i, "</title");
                size_t len = e ? (size_t)(e - (h + i)) : 0;
                char tmp[128];
                strlcpy(tmp, h + i, MIN(len + 1, sizeof(tmp)));
                /* collapse whitespace */
                size_t o = 0;
                for (size_t k = 0; tmp[k]; k++) {
                    if (isspace(tmp[k])) { if (o && p->title[o - 1] != ' ') p->title[o++] = ' '; }
                    else p->title[o++] = tmp[k];
                }
                p->title[o] = 0;
                i = e ? (size_t)(e - h) : n;
                continue;
            }
            if (tag_is(tag, tl, "head")) { skip = closing ? 0 : 1; continue; }
            if (skip) continue;

            if (tag_is(tag, tl, "br")) add_item(p, 1);
            else if (tag_is(tag, tl, "p") || tag_is(tag, tl, "div") || tag_is(tag, tl, "section") ||
                     tag_is(tag, tl, "article") || tag_is(tag, tl, "header") || tag_is(tag, tl, "footer") ||
                     tag_is(tag, tl, "nav") || tag_is(tag, tl, "form") || tag_is(tag, tl, "center") ||
                     tag_is(tag, tl, "main") || tag_is(tag, tl, "dl"))
                block(p, tag_is(tag, tl, "p") ? 3 : 1, indent);
            else if (tl == 2 && (tag[0] == 'h' || tag[0] == 'H') && tag[1] >= '1' && tag[1] <= '6') {
                block(p, closing ? 2 : 5, indent);
                style = closing ? ST_BODY : tag[1] == '1' ? ST_H1 : tag[1] == '2' ? ST_H2 : tag[1] == '3' ? ST_H3 : ST_BOLD;
            } else if (tag_is(tag, tl, "hr")) {
                block(p, 2, indent);
                add_item(p, 3);
                block(p, 2, indent);
            } else if (tag_is(tag, tl, "ul") || tag_is(tag, tl, "ol") || tag_is(tag, tl, "blockquote")) {
                list_depth += closing ? -1 : 1;
                if (list_depth < 0) list_depth = 0;
                if (!closing && list_depth < 8) ol_num[list_depth] = tag_is(tag, tl, "ol") ? 0 : -1;
                indent = MIN(list_depth, 6);
                block(p, 2, indent);
            } else if (tag_is(tag, tl, "li") || tag_is(tag, tl, "dt")) {
                if (!closing) {
                    block(p, 1, indent);
                    struct item *it = add_item(p, 4);
                    if (it) {
                        it->indent = (int8_t)indent;
                        if (list_depth > 0 && list_depth < 8 && ol_num[list_depth] >= 0 && tag_is(tag, tl, "li")) {
                            char num[16];
                            int nl = snprintf(num, sizeof(num), "%d.", ++ol_num[list_depth]);
                            it->off = pool_add(p, num, (size_t)nl);
                            it->len = (uint32_t)nl;
                        }
                    }
                }
            } else if (tag_is(tag, tl, "dd")) {
                block(p, 1, indent + 1);
            } else if (tag_is(tag, tl, "tr")) {
                block(p, 1, indent);
            } else if ((tag_is(tag, tl, "td") || tag_is(tag, tl, "th")) && !closing) {
                add_text(p, "   ", 3, style, italic, true, -1);
            } else if (tag_is(tag, tl, "pre")) {
                pre = !closing;
                block(p, 2, indent);
                style = closing ? ST_BODY : ST_PRE;
                /* a newline right after <pre> is not content */
                if (pre && i < n && h[i] == '\r') i++;
                if (pre && i < n && h[i] == '\n') i++;
            } else if (tag_is(tag, tl, "code") || tag_is(tag, tl, "tt") || tag_is(tag, tl, "kbd")) {
                if (!pre) style = closing ? ST_BODY : ST_PRE;
            } else if (tag_is(tag, tl, "b") || tag_is(tag, tl, "strong")) {
                if (style == ST_BODY || style == ST_BOLD) style = closing ? ST_BODY : ST_BOLD;
            } else if (tag_is(tag, tl, "i") || tag_is(tag, tl, "em") || tag_is(tag, tl, "cite")) {
                italic = !closing;
            } else if (tag_is(tag, tl, "small") || tag_is(tag, tl, "sub") || tag_is(tag, tl, "sup")) {
                if (style == ST_BODY || style == ST_SMALL) style = closing ? ST_BODY : ST_SMALL;
            } else if (tag_is(tag, tl, "a")) {
                if (closing) link = -1;
                else {
                    char href[400];
                    if (attr(attrs, al, "href", href, sizeof(href)) && p->nlinks < MAX_LINKS && strncmp(href, "javascript:", 11)) {
                        char full[512];
                        resolve_url(p->url, href, full, sizeof(full));
                        p->links[p->nlinks] = strdup(full);
                        link = p->nlinks++;
                    }
                }
            } else if (tag_is(tag, tl, "img") && !closing) {
                char src[400], num[16];
                if (attr(attrs, al, "src", src, sizeof(src)) && src[0] && p->nimgs < MAX_IMGS &&
                    strncmp(src, "data:", 5)) {
                    struct pimg *im = &p->imgs[p->nimgs];
                    char full[512];
                    resolve_url(p->url, src, full, sizeof(full));
                    im->url = strdup(full);
                    im->img = NULL;
                    im->state = IMG_PENDING;
                    im->aw = attr(attrs, al, "width", num, sizeof(num)) ? (int)strtol(num, NULL, 10) : 0;
                    im->ah = attr(attrs, al, "height", num, sizeof(num)) ? (int)strtol(num, NULL, 10) : 0;
                    struct item *it = add_item(p, 5);
                    if (it) { it->off = (uint32_t)p->nimgs; it->link = (int16_t)link; }
                    p->nimgs++;
                } else {
                    char alt[96];
                    if (attr(attrs, al, "alt", alt, sizeof(alt)) && alt[0]) {
                        char t[120];
                        int k = snprintf(t, sizeof(t), "[%s] ", alt);
                        add_text(p, t, (size_t)k, ST_SMALL, true, false, link);
                    }
                }
            } else if (tag_is(tag, tl, "input") && !closing) {
                char val[64];
                if (attr(attrs, al, "value", val, sizeof(val)) && val[0]) {
                    char t[80];
                    int k = snprintf(t, sizeof(t), "[ %s ] ", val);
                    add_text(p, t, (size_t)k, ST_SMALL, false, false, -1);
                }
            }
            continue;
        }
        size_t j = i;
        while (j < n && h[j] != '<') j++;
        if (!skip) add_text(p, h + i, j - i, style, italic, pre, link);
        i = j;
    }
}

static void page_free(struct page *p)
{
    for (int i = 0; i < p->nimgs; i++) {
        kfree(p->imgs[i].url);
        surface_free(p->imgs[i].img);
        p->imgs[i].url = NULL;
        p->imgs[i].img = NULL;
    }
    p->nimgs = 0;
    for (int i = 0; i < p->nlinks; i++) kfree(p->links[i]);
    kfree(p->text);
    memset(p->links, 0, sizeof(p->links));
    p->nlinks = 0;
    p->text = NULL;
    p->tlen = p->tcap = 0;
    p->nitems = 0;
    p->nboxes = 0;
    p->layout_w = 0;
    p->title[0] = 0;
}

/* ------------------------------------------------------------------------
 * layout
 * ---------------------------------------------------------------------- */

static font_t *style_font(int st)
{
    switch (st) {
    case ST_H1: return font_light;
    case ST_H2: return font_bold_lg;
    case ST_H3: return font_title;
    case ST_PRE: return font_mono;
    case ST_SMALL: return font_ui;
    case ST_BOLD: return font_ui_bold;
    default: return font_ui_lg;
    }
}

static void emit_box(struct page *p, int x, int y, int w, const struct item *it, uint32_t off, int len)
{
    if (p->nboxes >= MAX_BOXES || len <= 0) return;
    struct box *b = &p->boxes[p->nboxes++];
    b->x = x;
    b->y = y;
    b->w = w;
    b->h = 0;
    b->style = it->style;
    b->italic = it->italic;
    b->link = it->link;
    b->off = off;
    b->len = (uint16_t)len;
}

/* the size an image takes on the page (0x0 while unknown) */
static void image_size(struct page *p, int idx, int maxw, int *w, int *h)
{
    struct pimg *im = &p->imgs[idx];
    *w = *h = 0;
    if (im->state == IMG_OK && im->img) {
        int iw = im->img->w, ih = im->img->h;
        if (im->aw > 0 && im->ah > 0) { iw = im->aw; ih = im->ah; }
        else if (im->aw > 0) { ih = ih * im->aw / MAX(iw, 1); iw = im->aw; }
        else if (im->ah > 0) { iw = iw * im->ah / MAX(ih, 1); ih = im->ah; }
        if (iw > maxw) { ih = ih * maxw / iw; iw = maxw; }
        *w = MAX(iw, 1);
        *h = MAX(ih, 1);
    } else if (im->aw > 0 && im->ah > 0 && im->state != IMG_FAILED) {
        *w = MIN(im->aw, maxw);
        *h = im->ah * *w / im->aw;
    }
    *h = MIN(*h, 2000);
}

static void layout(struct page *p, int width)
{
    p->nboxes = 0;
    p->layout_w = width;
    int margin = 28;
    font_t *bf = style_font(ST_BODY);
    int def_asc = font_ascent(bf), def_desc = font_height(bf) - def_asc;
    int x0 = margin, x = margin, y = 18;
    /* the current line: tallest ascent/descent seen so far, boxes sit on a shared baseline */
    int asc = def_asc, desc = def_desc, gap = 5;
    int line_start_box = 0;
    int indent_px = 0, hang = 0;   /* hang: wrapped list item lines line up with the text */
    int maxw = width - 2 * margin;
    for (int k = 0; k < p->nitems; k++) {
        struct item *it = &p->items[k];
        if (it->kind == 1 || it->kind == 2 || it->kind == 3) {
            if (it->kind != 1) { indent_px = it->kind == 2 ? it->indent * 28 : indent_px; hang = 0; }
            if (!p->nboxes && it->kind != 3) { x = x0 + indent_px; continue; }   /* no gap above the first line */
            y += asc + desc + gap;
            if (it->kind == 2) y += it->space * 3;
            if (it->kind == 3) {
                struct item fake = *it;
                emit_box(p, x0, y, maxw, &fake, 0, 1);
                p->boxes[p->nboxes - 1].style = ST_RULE;
                y += 10;
            }
            x = x0 + indent_px + hang;
            asc = def_asc; desc = def_desc; gap = 5;
            line_start_box = p->nboxes;
            continue;
        }
        if (it->kind == 5) {
            int iw, ih;
            image_size(p, (int)it->off, maxw - indent_px - hang, &iw, &ih);
            if (!iw || !ih) continue;
            if (x + iw > x0 + maxw && x > x0 + indent_px + hang) {
                y += asc + desc + gap;
                asc = def_asc; desc = def_desc; gap = 5;
                x = x0 + indent_px + hang;
                line_start_box = p->nboxes;
            }
            /* the image sits on the baseline: it is all ascent */
            if (line_start_box == p->nboxes) { asc = ih; desc = 2; gap = 4; }
            else if (ih > asc) {
                for (int bi = line_start_box; bi < p->nboxes; bi++) p->boxes[bi].y += ih - asc;
                asc = ih;
            }
            struct item im = *it;
            im.style = ST_IMAGE;
            emit_box(p, x, y + asc - ih, iw, &im, it->off, 1);
            if (p->nboxes) p->boxes[p->nboxes - 1].h = (int16_t)ih;
            x += iw + 4;
            continue;
        }
        if (it->kind == 4) {
            struct item b = *it;
            b.style = ST_BODY;
            b.link = -1;
            if (it->len) {
                /* ordered list number, right aligned in the marker column */
                int w = font_text_width_n(bf, p->text + it->off, (int)it->len);
                emit_box(p, x0 + indent_px + 18 - w, y + asc - def_asc, w, &b, it->off, (int)it->len);
            } else {
                uint32_t off = pool_add(p, "•", 3);
                emit_box(p, x0 + indent_px + 6, y + asc - def_asc, 10, &b, off, 3);
            }
            hang = 24;
            x = x0 + indent_px + hang;
            continue;
        }
        font_t *f = style_font(it->style);
        int fa = font_ascent(f), fd = font_height(f) - fa, fg = it->style == ST_PRE ? 2 : 5;
        if (line_start_box == p->nboxes) {
            asc = fa; desc = fd; gap = fg;
        } else {
            if (fa > asc) {
                for (int b = line_start_box; b < p->nboxes; b++) p->boxes[b].y += fa - asc;
                asc = fa;
            }
            desc = MAX(desc, fd);
            gap = MAX(gap, fg);
        }
        const char *s = p->text + it->off;
        uint32_t pos = 0;
        while (pos < it->len) {
            /* next word (including a trailing space) */
            uint32_t end = pos;
            if (it->pre) end = it->len;
            else {
                while (end < it->len && s[end] != ' ') end++;
                if (end < it->len) end++;
            }
            int w = font_text_width_n(f, s + pos, (int)(end - pos));
            if (x + w > x0 + maxw && x > x0 + indent_px + hang + 30 && !it->pre) {
                y += asc + desc + gap;
                asc = fa; desc = fd; gap = fg;
                x = x0 + indent_px + hang;
                line_start_box = p->nboxes;
                if (s[pos] == ' ') { pos++; continue; }
            }
            emit_box(p, x, y + asc - fa, w, it, it->off + pos, (int)(end - pos));
            x += w;
            pos = end;
        }
    }
    p->height = y + asc + desc + gap + 40;
}

/* ------------------------------------------------------------------------
 * loading
 * ---------------------------------------------------------------------- */

/* the start page: /usr/share/zenith/home.html with @HOME@ and @BOOKMARKS@ filled in */
static char *replace_all(char *src, const char *key, const char *val)
{
    size_t kl = strlen(key), vl = strlen(val), n = 0;
    for (char *p = src; (p = strstr(p, key)); p += kl) n++;
    char *out = kmalloc(strlen(src) + n * vl + 1);
    if (!out) return src;
    char *o = out;
    for (char *p = src;;) {
        char *k = strstr(p, key);
        if (!k) { strcpy(o, p); break; }
        memcpy(o, p, (size_t)(k - p));
        o += k - p;
        memcpy(o, val, vl);
        o += vl;
        p = k + kl;
    }
    kfree(src);
    return out;
}

static char *build_home(void)
{
    size_t n;
    char *t = vfs_read_file("/usr/share/zenith/home.html", &n);
    if (!t) t = strdup("<html><body><h1>Zenith Web</h1><p>Type an address into the bar.</p></body></html>");
    t = replace_all(t, "@HOME@", session_home());
    char path[VFS_PATH_MAX];
    char list[2048] = "";
    if (session_path(".config/zenith/bookmarks", path, sizeof(path))) {
        size_t bn;
        char *bm = vfs_read_file(path, &bn);
        if (bm) {
            strlcat(list, "<ul>", sizeof(list));
            for (char *line = bm; line && *line;) {
                char *next = strchr(line, '\n');
                if (next) *next++ = 0;
                char *tab = strchr(line, '\t');
                if (tab) *tab++ = 0;
                if (*line && strlen(list) + strlen(line) * 2 + 64 < sizeof(list)) {
                    strlcat(list, "<li><a href=\"", sizeof(list));
                    strlcat(list, line, sizeof(list));
                    strlcat(list, "\">", sizeof(list));
                    strlcat(list, tab && *tab ? tab : line, sizeof(list));
                    strlcat(list, "</a></li>", sizeof(list));
                }
                line = next;
            }
            strlcat(list, "</ul>", sizeof(list));
            kfree(bm);
        }
    }
    if (!list[0]) strlcpy(list, "<p>No bookmarks yet.</p>", sizeof(list));
    return replace_all(t, "@BOOKMARKS@", list);
}

static void set_status(struct browser *b, const char *s) { strlcpy(b->status, s, sizeof(b->status)); }

static void notify_ui(struct browser *b)
{
    if (b->closed) return;
    struct gui_event ev = { 0 };
    ev.type = EV_TIMER;
    wm_post_event(b->app->win, &ev);
}

static int fetch_thread(void *arg)
{
    struct browser *b = arg;
    char url[512];
    strlcpy(url, b->pending_url, sizeof(url));
    struct net_info ni;
    if (!net_get_info(&ni)) {
        set_status(b, "No network adapter");
        b->loading = 0;
        notify_ui(b);
        return 0;
    }
    if (!ni.ip) {
        set_status(b, "Getting an address (DHCP)...");
        notify_ui(b);
        net_dhcp(3000);
    }
    b->pending_tls[0] = 0;
    struct http_result *r = kmalloc(sizeof(*r));
    for (int redirects = 0; redirects < 6; redirects++) {
        char status[96];
        snprintf(status, sizeof(status), "%s %.80s ...", strncasecmp(url, "https://", 8) ? "Loading" : "Connecting securely to", url);
        set_status(b, status);
        notify_ui(b);
        int code = net_http_request(url, r, 15000);
        if (code >= 300 && code < 400 && r->location[0]) {
            kfree(r->body);
            char next[512];
            resolve_url(url, r->location, next, sizeof(next));
            strlcpy(url, next, sizeof(url));
            continue;
        }
        if (code < 0) {
            bool secure = !strncasecmp(url, "https://", 8);
            char *msg = kmalloc(2048);
            snprintf(msg, 2048, "<title>%s</title><h1>%s</h1><p>%s</p><p><small>%s</small></p>%s",
                     secure && strstr(r->error, "certificate") ? "Not secure" : "Could not load the page",
                     secure && strstr(r->error, "certificate") ? "This connection is not private" : "Could not load the page",
                     r->error[0] ? r->error : "The server did not answer.", url,
                     secure && strstr(r->error, "certificate")
                         ? "<p>Zenith Web checked the server's certificate against the trusted root certificates and "
                           "it did not pass. The page was not loaded, so nobody could read or change what you send.</p>"
                         : "<p>Check the address and the network connection.</p>");
            b->pending_body = msg;
            b->pending_len = strlen(msg);
            b->pending_plain = false;
            set_status(b, r->error[0] ? r->error : "Failed");
            kfree(r->body);
        } else {
            char *body = r->body;
            size_t len = r->len;
            strlcpy(b->pending_tls, r->tls, sizeof(b->pending_tls));
            b->pending_body = body ? body : strdup("");
            b->pending_len = body ? len : 0;
            /* treat bodies without tags as plain text */
            b->pending_plain = body && !strstr_ci(r->content_type, "html") && !strstr_ci(body, "<html") &&
                               !strstr_ci(body, "<body") && !strstr_ci(body, "<p") && !strstr_ci(body, "<a ");
            if (r->tls[0]) snprintf(status, sizeof(status), "Secure  ·  %s  ·  HTTP %d  ·  %lu bytes", r->tls, code, len);
            else snprintf(status, sizeof(status), "Done  ·  HTTP %d  ·  %lu bytes (not encrypted)", code, len);
            set_status(b, status);
        }
        break;
    }
    kfree(r);
    strlcpy(b->pending_url, url, sizeof(b->pending_url));
    b->loading = 2;
    notify_ui(b);
    b->fetch_active = false;
    return 0;
}

/* shrink very large pictures so a photo-heavy page does not eat the RAM */
static surface_t *shrink(surface_t *s, int maxw)
{
    if (!s || s->w <= maxw) return s;
    int nw = maxw, nh = MAX(1, s->h * maxw / s->w);
    surface_t *d = surface_new(nw, nh);
    if (!d) return s;
    for (int y = 0; y < nh; y++) {
        int y0 = y * s->h / nh, y1 = MAX(y0 + 1, (y + 1) * s->h / nh);
        for (int x = 0; x < nw; x++) {
            int x0 = x * s->w / nw, x1 = MAX(x0 + 1, (x + 1) * s->w / nw);
            uint32_t r = 0, g = 0, bl = 0, al = 0, n = 0;
            for (int yy = y0; yy < y1; yy += 1 + (y1 - y0) / 4)
                for (int xx = x0; xx < x1; xx += 1 + (x1 - x0) / 4) {
                    color_t c = s->px[(size_t)yy * s->stride + xx];
                    r += C_R(c); g += C_G(c); bl += C_B(c); al += C_A(c); n++;
                }
            d->px[(size_t)y * d->stride + x] = RGBA(r / n, g / n, bl / n, al / n);
        }
    }
    surface_free(s);
    return d;
}

static void *fetch_bytes(const char *url, size_t *len)
{
    if (!strncasecmp(url, "file://", 7)) return vfs_read_file(url + 7, len);
    if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8)) return NULL;
    char cur[512], loc[512];
    strlcpy(cur, url, sizeof(cur));
    for (int redirects = 0; redirects < 4; redirects++) {
        char *body = NULL;
        loc[0] = 0;
        int code = net_http_get_ex(cur, &body, len, loc, sizeof(loc), 10000);
        if (code >= 300 && code < 400 && loc[0]) {
            kfree(body);
            char next[512];
            resolve_url(cur, loc, next, sizeof(next));
            strlcpy(cur, next, sizeof(cur));
            continue;
        }
        if (code == 200) return body;
        kfree(body);
        return NULL;
    }
    return NULL;
}

static int img_thread(void *arg)
{
    struct browser *b = arg;
    int gen = b->page_gen;
    for (int i = 0;; i++) {
        char url[512];
        mutex_lock(&b->img_lock);
        bool stale = b->closed || gen != b->page_gen || i >= b->page.nimgs;
        if (!stale) strlcpy(url, b->page.imgs[i].url, sizeof(url));
        mutex_unlock(&b->img_lock);
        if (stale) break;
        size_t len = 0;
        void *data = fetch_bytes(url, &len);
        surface_t *img = data ? shrink(image_decode(data, len), 1600) : NULL;
        kfree(data);
        mutex_lock(&b->img_lock);
        if (!b->closed && gen == b->page_gen && i < b->page.nimgs) {
            b->page.imgs[i].img = img;
            b->page.imgs[i].state = img ? IMG_OK : IMG_FAILED;
            b->page.layout_w = 0;              /* reflow around the new picture */
            img = NULL;
        }
        mutex_unlock(&b->img_lock);
        surface_free(img);
        notify_ui(b);
    }
    __atomic_fetch_sub(&b->img_threads, 1, __ATOMIC_SEQ_CST);
    return 0;
}

/* file:// pages come straight from the VFS; directories get an index page */
static void load_file(struct browser *b, const char *url, const char *path)
{
    struct vfs_stat st;
    size_t cap = 4096, len = 0;
    char *out = NULL;
    if (vfs_stat(path, &st)) {
        out = kmalloc(cap);
        len = (size_t)snprintf(out, cap, "<h1>File not found</h1><p>There is no file called <code>%s</code>.</p>", path);
        b->pending_plain = false;
    } else if (st.type == VN_DIR) {
        struct vfs_dirent *ents = kmalloc(sizeof(*ents) * 256);
        int n = vfs_list(path, ents, 256);
        cap = 1024 + (size_t)MAX(n, 0) * 400;
        out = kmalloc(cap);
        len = (size_t)snprintf(out, cap, "<title>Index of %s</title><h1>Index of %s</h1><ul>", path, path);
        if (strcmp(path, "/")) len += (size_t)snprintf(out + len, cap - len, "<li><a href=\"../\">Parent directory</a></li>");
        for (int i = 0; i < n; i++) {
            bool dir = ents[i].type == VN_DIR;
            if (dir)
                len += (size_t)snprintf(out + len, cap - len, "<li><a href=\"%s/\">%s/</a></li>", ents[i].name, ents[i].name);
            else
                len += (size_t)snprintf(out + len, cap - len, "<li><a href=\"%s\">%s</a> <small>%lu bytes</small></li>",
                                        ents[i].name, ents[i].name, ents[i].size);
        }
        len += (size_t)snprintf(out + len, cap - len, "</ul>");
        kfree(ents);
        b->pending_plain = false;
    } else {
        out = vfs_read_file(path, &len);
        if (!out) { out = strdup(""); len = 0; }
        size_t pl = strlen(path);
        b->pending_plain = !((pl > 5 && !strcasecmp(path + pl - 5, ".html")) || (pl > 4 && !strcasecmp(path + pl - 4, ".htm")));
    }
    b->pending_body = out;
    b->pending_len = len;
    b->pending_tls[0] = 0;
    strlcpy(b->pending_url, url, sizeof(b->pending_url));
    char status[96];
    snprintf(status, sizeof(status), "Local file  ·  %lu bytes", len);
    set_status(b, status);
    b->loading = 2;
    notify_ui(b);
}

static void navigate(struct browser *b, const char *target, bool record)
{
    if (b->loading == 1) return;
    char url[512];
    if (!strncmp(target, "about:", 6) || !strncasecmp(target, "http://", 7) || !strncasecmp(target, "https://", 8) ||
        !strncasecmp(target, "file://", 7))
        strlcpy(url, target, sizeof(url));
    else if (target[0] == '/')
        snprintf(url, sizeof(url), "file://%s", target);
    else if (strchr(target, '.') && !strchr(target, ' '))
        snprintf(url, sizeof(url), "https://%s", target);
    else {
        /* search via FrogFind */
        char q[400];
        size_t o = 0;
        for (const char *s = target; *s && o + 4 < sizeof(q); s++) {
            if (*s == ' ') q[o++] = '+';
            else if (isalnum(*s) || *s == '-' || *s == '.') q[o++] = *s;
            else o += (size_t)snprintf(q + o, sizeof(q) - o, "%%%02X", (uint8_t)*s);
        }
        q[o] = 0;
        snprintf(url, sizeof(url), "https://html.duckduckgo.com/html/?q=%s", q);
    }
    char *frag = strchr(url, '#');
    if (frag) *frag = 0;
    if (record) {
        int pos = b->hlen ? b->hpos + 1 : 0;
        if (pos >= 32) {
            memmove(b->history[0], b->history[1], sizeof(b->history[0]) * 31);
            pos = 31;
        }
        b->hpos = pos;
        strlcpy(b->history[b->hpos], url, sizeof(b->history[0]));
        b->hlen = b->hpos + 1;
    }
    strlcpy(b->addr, url, sizeof(b->addr));
    if (!strcmp(url, "about:home")) {
        mutex_lock(&b->img_lock);
        b->page_gen++;
        page_free(&b->page);
        strlcpy(b->page.url, url, sizeof(b->page.url));
        char *home = build_home();
        parse_html(&b->page, home, strlen(home), false);
        kfree(home);
        mutex_unlock(&b->img_lock);
        b->scroll = 0;
        set_status(b, "Home");
        b->tls[0] = 0;
        if (b->app->win) wm_set_title(b->app->win, "Zenith Web");
        return;
    }
    if (!strncasecmp(url, "file://", 7)) {
        /* normalise ".." and "." */
        char path[VFS_PATH_MAX];
        if (!vfs_resolve("/", url + 7, path, sizeof(path))) strlcpy(path, "/", sizeof(path));
        struct vfs_stat st;
        bool dir = !vfs_stat(path, &st) && st.type == VN_DIR && strcmp(path, "/");
        snprintf(url, sizeof(url), "file://%s%s", path, dir ? "/" : "");
        strlcpy(b->addr, url, sizeof(b->addr));
        if (record) strlcpy(b->history[b->hpos], url, sizeof(b->history[0]));
        load_file(b, url, path);
        return;
    }
    strlcpy(b->pending_url, url, sizeof(b->pending_url));
    b->loading = 1;
    b->fetch_active = true;
    thread_create("web-fetch", fetch_thread, b);
}

/* ------------------------------------------------------------------------
 * UI
 * ---------------------------------------------------------------------- */

#define BAR_H 52
#define STATUS_H 26

static void br_paint(app_t *a, surface_t *s)
{
    struct browser *b = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;

    if (b->loading == 2) {
        mutex_lock(&b->img_lock);
        b->page_gen++;
        page_free(&b->page);
        strlcpy(b->page.url, b->pending_url, sizeof(b->page.url));
        strlcpy(b->addr, b->pending_url, sizeof(b->addr));
        parse_html(&b->page, b->pending_body, b->pending_len, b->pending_plain);
        mutex_unlock(&b->img_lock);
        kfree(b->pending_body);
        b->pending_body = NULL;
        b->scroll = 0;
        b->loading = 0;
        strlcpy(b->tls, b->pending_tls, sizeof(b->tls));
        if (b->page.nimgs) {
            __atomic_fetch_add(&b->img_threads, 1, __ATOMIC_SEQ_CST);
            thread_create("web-images", img_thread, b);
        }
        char title[96];
        const char *host = strstr(b->page.url, "://");
        snprintf(title, sizeof(title), "%s - Zenith Web", b->page.title[0] ? b->page.title : host ? host + 3 : b->page.url);
        wm_set_title(a->win, title);
    }
    int view_h = H - BAR_H - STATUS_H;
    mutex_lock(&b->img_lock);
    if (b->page.layout_w != W) layout(&b->page, W - 14);

    /* page */
    gfx_fill(s, 0, BAR_H, W, view_h, HEX(0xF7F8FC));
    surface_t sub = *s;
    sub.clip = rect_intersect(s->clip, R(0, BAR_H, W, view_h));
    if (u->wheel && ui_hover(u, R(0, BAR_H, W, view_h)))
        b->scroll = CLAMP(b->scroll - u->wheel * 60, 0, MAX(0, b->page.height - view_h));
    b->scroll = CLAMP(b->scroll, 0, MAX(0, b->page.height - view_h));
    int hover = -1;
    for (int i = 0; i < b->page.nboxes; i++) {
        struct box *bx = &b->page.boxes[i];
        int y = BAR_H + bx->y - b->scroll;
        if (y > BAR_H + view_h || y + MAX(40, bx->h) < BAR_H) continue;
        if (bx->style == ST_RULE) {
            gfx_fill(&sub, bx->x, y + 4, bx->w, 1, HEX(0xD5D9E6));
            continue;
        }
        if (bx->style == ST_IMAGE) {
            struct pimg *im = &b->page.imgs[bx->off];
            if (im->img) gfx_blit_scaled(&sub, R(bx->x, y, bx->w, bx->h), im->img, R(0, 0, im->img->w, im->img->h));
            else {
                gfx_fill(&sub, bx->x, y, bx->w, bx->h, HEX(0xE6E8F0));
                if (bx->w >= 40 && bx->h >= 40) icon_draw(&sub, ICON_IMAGE, bx->x + bx->w / 2 - 14, y + bx->h / 2 - 14, 28);
            }
            if (bx->link >= 0 && rect_has(R(bx->x, y, bx->w, bx->h), u->mx, u->my)) {
                hover = bx->link;
                gfx_round_outline(&sub, bx->x - 2, y - 2, bx->w + 4, bx->h + 4, 4, HEX(0x2F5BD8));
            }
            continue;
        }
        font_t *f = style_font(bx->style);
        color_t c = HEX(0x1D2238);
        if (bx->style == ST_H1 || bx->style == ST_H2) c = HEX(0x2A2250);
        if (bx->style == ST_SMALL || bx->italic) c = HEX(0x5A6078);
        if (bx->link >= 0) {
            bool hov = u->mx >= bx->x && u->mx < bx->x + bx->w && u->my >= y && u->my < y + font_height(f);
            if (hov) hover = bx->link;
            c = bx->link == b->hover_link ? HEX(0x7C3AED) : HEX(0x2F5BD8);
            gfx_fill(&sub, bx->x, y + font_ascent(f) + 2, bx->w, 1, ALPHA(c, 150));
        }
        if (bx->style == ST_PRE) gfx_fill(&sub, bx->x, y - 1, bx->w, font_height(f) + 2, HEX(0xECEEF6));
        gfx_text_n(&sub, f, bx->x, y, b->page.text + bx->off, bx->len, c);
    }
    char target[512] = "";
    if (hover >= 0) strlcpy(target, b->page.links[hover], sizeof(target));
    mutex_unlock(&b->img_lock);
    if (hover != b->hover_link) { b->hover_link = hover; u->want_repaint = true; }
    if (hover >= 0 && u->mreleased && ui_hover(u, R(0, BAR_H, W, view_h))) navigate(b, target, true);
    int sc = b->scroll;
    ui_scrollbar(u, R(W - 12, BAR_H + 2, 10, view_h - 4), &sc, MAX(b->page.height, 1), view_h);
    b->scroll = sc;

    /* toolbar */
    gfx_fill(s, 0, 0, W, BAR_H, theme.panel);
    gfx_fill(s, 0, BAR_H - 1, W, 1, ALPHA(0xFFFFFF, 14));
    if (ui_button(u, R(10, 10, 34, 32), "←", BTN_FLAT) && b->hpos > 0) { b->hpos--; navigate(b, b->history[b->hpos], false); }
    if (ui_button(u, R(48, 10, 34, 32), "→", BTN_FLAT) && b->hpos + 1 < b->hlen) { b->hpos++; navigate(b, b->history[b->hpos], false); }
    if (ui_icon_button(u, R(86, 10, 34, 32), ICON_RESTART, "Reload")) navigate(b, b->addr, false);
    if (ui_icon_button(u, R(124, 10, 34, 32), ICON_HOME, "Home")) navigate(b, "about:home", true);
    rect_t ar = R(166, 9, W - 176, 34);
    bool secure = b->tls[0] && !strncasecmp(b->addr, "https://", 8) && !u->focus;
    if (secure) {
        /* a small padlock in front of the address */
        int lx = ar.x + 10, ly = ar.y + 9;
        gfx_ring(s, (float)lx + 6, (float)ly + 6, 4.5f, 2, theme.success);
        gfx_round_rect(s, lx, ly + 6, 12, 10, 2, theme.success);
        ar.x += 22;
        ar.w -= 22;
    }
    if (ui_textbox(u, ar, b->addr, sizeof(b->addr), "Search or enter an address")) {
        char t[512];
        strlcpy(t, b->addr, sizeof(t));
        navigate(b, t, true);
        u->focus = 0;
    }
    if (b->loading == 1) {
        /* indeterminate progress bar */
        int t = (int)(uptime_ms() / 4 % (uint64_t)(W + 200)) - 200;
        gfx_fill(s, MAX(0, t), BAR_H - 2, 200, 2, theme.accent);
        u->want_repaint = true;
    }

    /* status bar */
    gfx_fill(s, 0, H - STATUS_H, W, STATUS_H, theme.panel);
    const char *st = hover >= 0 ? b->page.links[hover] : b->status;
    surface_t ss = *s;
    ss.clip = rect_intersect(s->clip, R(0, H - STATUS_H, W - 10, STATUS_H));
    gfx_text_ellipsis(&ss, font_ui, 12, H - STATUS_H + 5, W - 24, st, theme.text_dim);
}

static int br_event(app_t *a, struct gui_event *ev)
{
    struct browser *b = a->data;
    if (ev->type == EV_KEY_DOWN) {
        if ((ev->mods & MOD_ALT) && ev->key == KEY_LEFT && b->hpos > 0) { b->hpos--; navigate(b, b->history[b->hpos], false); }
        else if ((ev->mods & MOD_ALT) && ev->key == KEY_RIGHT && b->hpos + 1 < b->hlen) { b->hpos++; navigate(b, b->history[b->hpos], false); }
        else if (ev->key == KEY_F5) navigate(b, b->addr, false);
        else if (!a->ui.focus) {
            int view_h = a->win->ch - BAR_H - STATUS_H;
            if (ev->key == KEY_DOWN) b->scroll += 40;
            if (ev->key == KEY_UP) b->scroll -= 40;
            if (ev->key == KEY_PAGEDOWN || ev->key == KEY_SPACE) b->scroll += view_h - 40;
            if (ev->key == KEY_PAGEUP) b->scroll -= view_h - 40;
            if (ev->key == KEY_HOME) b->scroll = 0;
            if (ev->key == KEY_END) b->scroll = b->page.height;
        }
    }
    return 1;
}

static void br_close(app_t *a)
{
    struct browser *b = a->data;
    b->closed = true;
    while (b->fetch_active || b->img_threads) sched_sleep(20);
    a->quit = true;
}

int browser_main(void *arg)
{
    struct browser *b = kzalloc(sizeof(*b));
    b->page.items = kmalloc(sizeof(struct item) * MAX_ITEMS);
    b->page.boxes = kmalloc(sizeof(struct box) * MAX_BOXES);
    b->hover_link = -1;
    b->img_lock = (mutex_t)MUTEX_INIT("web-images");
    app_t a = { 0 };
    b->app = &a;
    a.data = b;
    a.win = wm_create("Zenith Web", 960, 640, WF_RESIZABLE);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_BROWSER);
    wm_set_min_size(a.win, 480, 320);
    a.on_paint = br_paint;
    a.on_event = br_event;
    a.on_close = br_close;
    navigate(b, arg ? (const char *)arg : "about:home", true);
    kfree(arg);
    int r = app_run(&a);
    if (b->pending_body) kfree(b->pending_body);
    page_free(&b->page);
    kfree(b->page.items);
    kfree(b->page.boxes);
    kfree(b);
    return r;
}
