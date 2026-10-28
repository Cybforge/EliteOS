/*
 * 2D drawing: rectangles, gradients, anti-aliased shapes, blits, blur.
 * Everything clips against surface->clip.
 */
#include <gfx.h>
#include <mm.h>

/* ------------------------------------------------------------------------
 * rects and surfaces
 * ---------------------------------------------------------------------- */

rect_t rect_intersect(rect_t a, rect_t b)
{
    int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
    int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
    rect_t r = { x0, y0, x1 - x0, y1 - y0 };
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

rect_t rect_union(rect_t a, rect_t b)
{
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    int x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y);
    int x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);
    return R(x0, y0, x1 - x0, y1 - y0);
}

bool rect_overlaps(rect_t a, rect_t b)
{
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

rect_t rect_inset(rect_t r, int d)
{
    return R(r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d);
}

surface_t *surface_new(int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    surface_t *s = kzalloc(sizeof(*s));
    if (!s) return NULL;
    s->px = kmalloc((size_t)w * h * 4);
    if (!s->px) { kfree(s); return NULL; }
    s->w = w;
    s->h = h;
    s->stride = w;
    s->clip = R(0, 0, w, h);
    s->owned = true;
    return s;
}

void surface_free(surface_t *s)
{
    if (!s) return;
    if (s->owned) kfree(s->px);
    kfree(s);
}

surface_t surface_wrap(uint32_t *px, int w, int h, int stride)
{
    surface_t s = { px, w, h, stride, { 0, 0, w, h }, false };
    return s;
}

void surface_reset_clip(surface_t *s) { s->clip = R(0, 0, s->w, s->h); }

void surface_set_clip(surface_t *s, rect_t r)
{
    s->clip = rect_intersect(r, R(0, 0, s->w, s->h));
}

/* ------------------------------------------------------------------------
 * color helpers
 * ---------------------------------------------------------------------- */

color_t color_lerp(color_t a, color_t b, int t)
{
    if (t <= 0) return a;
    if (t >= 256) return b;
    uint32_t rb = (((a & 0xFF00FF) * (256 - (uint32_t)t)) + ((b & 0xFF00FF) * (uint32_t)t)) >> 8;
    uint32_t g = (((a & 0x00FF00) * (256 - (uint32_t)t)) + ((b & 0x00FF00) * (uint32_t)t)) >> 8;
    uint32_t al = ((C_A(a) * (256 - (uint32_t)t)) + (C_A(b) * (uint32_t)t)) >> 8;
    return (rb & 0xFF00FF) | (g & 0xFF00) | (al << 24);
}

color_t color_mix(color_t a, color_t b, float t)
{
    return color_lerp(a, b, (int)(t * 256.0f));
}

color_t color_lighten(color_t c, int amt)
{
    int r = MIN(255, (int)C_R(c) + amt), g = MIN(255, (int)C_G(c) + amt), b = MIN(255, (int)C_B(c) + amt);
    return (c & 0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

color_t color_darken(color_t c, int amt)
{
    int r = MAX(0, (int)C_R(c) - amt), g = MAX(0, (int)C_G(c) - amt), b = MAX(0, (int)C_B(c) - amt);
    return (c & 0xFF000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

color_t color_hsv(float h, float s, float v)
{
    while (h < 0) h += 360.0f;
    while (h >= 360.0f) h -= 360.0f;
    float c = v * s;
    float hh = h / 60.0f;
    float x = c * (1.0f - (float)k_fabs(k_fmod(hh, 2.0) - 1.0));
    float r = 0, g = 0, b = 0;
    if (hh < 1) { r = c; g = x; }
    else if (hh < 2) { r = x; g = c; }
    else if (hh < 3) { g = c; b = x; }
    else if (hh < 4) { g = x; b = c; }
    else if (hh < 5) { r = x; b = c; }
    else { r = c; b = x; }
    float m = v - c;
    return RGB((int)((r + m) * 255.0f + 0.5f), (int)((g + m) * 255.0f + 0.5f), (int)((b + m) * 255.0f + 0.5f));
}

/* ------------------------------------------------------------------------
 * fills
 * ---------------------------------------------------------------------- */

static inline bool clip_rect(surface_t *s, int *x, int *y, int *w, int *h)
{
    rect_t r = rect_intersect(R(*x, *y, *w, *h), s->clip);
    *x = r.x; *y = r.y; *w = r.w; *h = r.h;
    return r.w > 0 && r.h > 0;
}

void gfx_fill(surface_t *s, int x, int y, int w, int h, color_t c)
{
    if (!clip_rect(s, &x, &y, &w, &h)) return;
    uint32_t a = C_A(c);
    if (a == 0) return;
    if (a == 255) {
        for (int j = 0; j < h; j++)
            memset32(s->px + (size_t)(y + j) * s->stride + x, c, (size_t)w);
        return;
    }
    for (int j = 0; j < h; j++) {
        uint32_t *p = s->px + (size_t)(y + j) * s->stride + x;
        for (int i = 0; i < w; i++) p[i] = blend(p[i], c, a);
    }
}

void gfx_fill_r(surface_t *s, rect_t r, color_t c) { gfx_fill(s, r.x, r.y, r.w, r.h, c); }
void gfx_clear(surface_t *s, color_t c) { gfx_fill(s, 0, 0, s->w, s->h, c | 0xFF000000u); }
void gfx_hline(surface_t *s, int x, int y, int w, color_t c) { gfx_fill(s, x, y, w, 1, c); }
void gfx_vline(surface_t *s, int x, int y, int h, color_t c) { gfx_fill(s, x, y, 1, h, c); }

void gfx_outline(surface_t *s, int x, int y, int w, int h, color_t c)
{
    gfx_fill(s, x, y, w, 1, c);
    gfx_fill(s, x, y + h - 1, w, 1, c);
    gfx_fill(s, x, y + 1, 1, h - 2, c);
    gfx_fill(s, x + w - 1, y + 1, 1, h - 2, c);
}

void gfx_pixel(surface_t *s, int x, int y, color_t c)
{
    if (!rect_has(s->clip, x, y)) return;
    uint32_t *p = s->px + (size_t)y * s->stride + x;
    uint32_t a = C_A(c);
    *p = a == 255 ? c : blend(*p, c, a);
}

void gfx_pixel_aa(surface_t *s, int x, int y, color_t c, int cov)
{
    if (cov <= 0 || !rect_has(s->clip, x, y)) return;
    uint32_t a = (C_A(c) * (uint32_t)MIN(cov, 255)) / 255;
    uint32_t *p = s->px + (size_t)y * s->stride + x;
    *p = blend(*p, c, a);
}

void gfx_vgradient(surface_t *s, int x, int y, int w, int h, color_t top, color_t bottom)
{
    int y0 = y, hh = h;
    if (!clip_rect(s, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        int t = hh > 1 ? ((y + j - y0) * 256) / (hh - 1) : 0;
        color_t c = color_lerp(top, bottom, t);
        uint32_t a = C_A(c);
        uint32_t *p = s->px + (size_t)(y + j) * s->stride + x;
        if (a == 255) memset32(p, c, (size_t)w);
        else for (int i = 0; i < w; i++) p[i] = blend(p[i], c, a);
    }
}

void gfx_hgradient(surface_t *s, int x, int y, int w, int h, color_t left, color_t right)
{
    int x0 = x, ww = w;
    if (!clip_rect(s, &x, &y, &w, &h)) return;
    for (int i = 0; i < w; i++) {
        int t = ww > 1 ? ((x + i - x0) * 256) / (ww - 1) : 0;
        color_t c = color_lerp(left, right, t);
        uint32_t a = C_A(c);
        for (int j = 0; j < h; j++) {
            uint32_t *p = s->px + (size_t)(y + j) * s->stride + x + i;
            *p = a == 255 ? c : blend(*p, c, a);
        }
    }
}

/* ------------------------------------------------------------------------
 * rounded rectangles
 * ---------------------------------------------------------------------- */

/* coverage of pixel (px,py) relative to a corner circle centered (cx,cy) */
static inline int corner_cov(float dx, float dy, float r)
{
    float d = (float)k_sqrt((double)(dx * dx + dy * dy));
    float c = r - d + 0.5f;
    if (c <= 0) return 0;
    if (c >= 1) return 255;
    return (int)(c * 255.0f);
}

static void round_rect_rows(surface_t *s, int x, int y, int w, int h, int r,
                            color_t top, color_t bottom, bool grad)
{
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    rect_t cl = s->clip;
    int ystart = MAX(y, cl.y), yend = MIN(y + h, cl.y + cl.h);
    for (int yy = ystart; yy < yend; yy++) {
        color_t c = grad ? color_lerp(top, bottom, h > 1 ? ((yy - y) * 256) / (h - 1) : 0) : top;
        uint32_t ca = C_A(c);
        if (!ca) continue;
        int row = yy - y;
        int inset = 0;
        bool edge = false;
        float fy = 0;
        if (row < r) { fy = (float)(r - row) - 0.5f; edge = true; }
        else if (row >= h - r) { fy = (float)(row - (h - r)) + 0.5f; edge = true; }
        uint32_t *line = s->px + (size_t)yy * s->stride;
        if (edge) {
            float fr = (float)r;
            float span = fr * fr - fy * fy;
            float inner = span > 0 ? (float)k_sqrt((double)span) : 0;
            inset = r - (int)(inner + 0.0f);
            /* anti-aliased corner pixels */
            for (int i = 0; i < inset + 1 && i < w / 2 + 1; i++) {
                float fx = (float)(r - i) - 0.5f;
                int cov = corner_cov(fx, fy, fr);
                if (!cov) continue;
                uint32_t a = ca * (uint32_t)cov / 255;
                int lx = x + i, rx = x + w - 1 - i;
                if (lx >= cl.x && lx < cl.x + cl.w) line[lx] = blend(line[lx], c, a);
                if (rx != lx && rx >= cl.x && rx < cl.x + cl.w) line[rx] = blend(line[rx], c, a);
            }
            inset += 1;
        }
        int x0 = MAX(x + inset, cl.x), x1 = MIN(x + w - inset, cl.x + cl.w);
        if (x1 <= x0) continue;
        if (ca == 255) memset32(line + x0, c, (size_t)(x1 - x0));
        else for (int xx = x0; xx < x1; xx++) line[xx] = blend(line[xx], c, ca);
    }
}

void gfx_round_rect(surface_t *s, int x, int y, int w, int h, int r, color_t c)
{
    if (r <= 0) { gfx_fill(s, x, y, w, h, c); return; }
    round_rect_rows(s, x, y, w, h, r, c, c, false);
}

void gfx_round_rect_grad(surface_t *s, int x, int y, int w, int h, int r, color_t top, color_t bottom)
{
    if (r <= 0) { gfx_vgradient(s, x, y, w, h, top, bottom); return; }
    round_rect_rows(s, x, y, w, h, r, top, bottom, true);
}

void gfx_round_outline(surface_t *s, int x, int y, int w, int h, int r, color_t c)
{
    if (r <= 0) { gfx_outline(s, x, y, w, h, c); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    gfx_fill(s, x + r, y, w - 2 * r, 1, c);
    gfx_fill(s, x + r, y + h - 1, w - 2 * r, 1, c);
    gfx_fill(s, x, y + r, 1, h - 2 * r, c);
    gfx_fill(s, x + w - 1, y + r, 1, h - 2 * r, c);
    /* corner arcs */
    float fr = (float)r - 0.5f;
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            float dx = (float)(r - i) - 0.5f, dy = (float)(r - j) - 0.5f;
            float d = (float)k_sqrt((double)(dx * dx + dy * dy));
            float cov = 1.0f - k_fabs(d - fr);
            if (cov <= 0) continue;
            int cv = (int)(cov * 255.0f);
            gfx_pixel_aa(s, x + i, y + j, c, cv);
            gfx_pixel_aa(s, x + w - 1 - i, y + j, c, cv);
            gfx_pixel_aa(s, x + i, y + h - 1 - j, c, cv);
            gfx_pixel_aa(s, x + w - 1 - i, y + h - 1 - j, c, cv);
        }
    }
}

/* ------------------------------------------------------------------------
 * circles, arcs, lines
 * ---------------------------------------------------------------------- */

void gfx_circle(surface_t *s, float cx, float cy, float r, color_t c)
{
    int y0 = (int)(cy - r - 1), y1 = (int)(cy + r + 2);
    rect_t cl = s->clip;
    y0 = MAX(y0, cl.y);
    y1 = MIN(y1, cl.y + cl.h);
    uint32_t ca = C_A(c);
    for (int y = y0; y < y1; y++) {
        float dy = (float)y + 0.5f - cy;
        float outer2 = (r + 0.5f) * (r + 0.5f) - dy * dy;
        if (outer2 <= 0) continue;
        float outer = (float)k_sqrt((double)outer2);
        float inner2 = (r - 0.5f) * (r - 0.5f) - dy * dy;
        float inner = inner2 > 0 ? (float)k_sqrt((double)inner2) : 0;
        int xo0 = (int)k_floor((double)(cx - outer)), xo1 = (int)k_floor((double)(cx + outer));
        int xi0 = inner > 0 ? (int)k_floor((double)(cx - inner)) + 1 : xo1 + 1;
        int xi1 = inner > 0 ? (int)k_floor((double)(cx + inner)) - 1 : xo1;
        uint32_t *line = s->px + (size_t)y * s->stride;
        for (int x = xo0; x <= xo1; x++) {
            if (x < cl.x || x >= cl.x + cl.w) continue;
            if (x >= xi0 && x <= xi1) {
                line[x] = ca == 255 ? c : blend(line[x], c, ca);
                continue;
            }
            float dx = (float)x + 0.5f - cx;
            float d = (float)k_sqrt((double)(dx * dx + dy * dy));
            float cov = r + 0.5f - d;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            line[x] = blend(line[x], c, (uint32_t)((float)ca * cov));
        }
    }
}

void gfx_ring(surface_t *s, float cx, float cy, float r, float t, color_t c)
{
    gfx_arc(s, cx, cy, r, t, 0, (float)(2 * K_PI), c);
}

/* Arc from angle a0 to a1 (radians, 0 = 12 o'clock, clockwise). */
void gfx_arc(surface_t *s, float cx, float cy, float r, float t, float a0, float a1, color_t c)
{
    float ro = r + t * 0.5f, ri = r - t * 0.5f;
    int x0 = (int)(cx - ro - 1), x1 = (int)(cx + ro + 2);
    int y0 = (int)(cy - ro - 1), y1 = (int)(cy + ro + 2);
    rect_t cl = s->clip;
    x0 = MAX(x0, cl.x); x1 = MIN(x1, cl.x + cl.w);
    y0 = MAX(y0, cl.y); y1 = MIN(y1, cl.y + cl.h);
    bool full = a1 - a0 >= (float)(2 * K_PI) - 0.0001f;
    uint32_t ca = C_A(c);
    for (int y = y0; y < y1; y++) {
        float dy = (float)y + 0.5f - cy;
        uint32_t *line = s->px + (size_t)y * s->stride;
        for (int x = x0; x < x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float d2 = dx * dx + dy * dy;
            if (d2 > (ro + 1) * (ro + 1) || d2 < (ri - 1) * (ri - 1)) continue;
            float d = (float)k_sqrt((double)d2);
            float cov = MIN(ro - d + 0.5f, d - ri + 0.5f);
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            if (!full) {
                float ang = (float)k_atan2((double)dx, (double)-dy);
                if (ang < 0) ang += (float)(2 * K_PI);
                if (ang < a0 || ang > a1) continue;
            }
            line[x] = blend(line[x], c, (uint32_t)((float)ca * cov));
        }
    }
}

/* Xiaolin Wu anti-aliased line */
void gfx_line(surface_t *s, float x0, float y0, float x1, float y1, color_t c)
{
    bool steep = k_fabs(y1 - y0) > k_fabs(x1 - x0);
    float t;
    if (steep) { t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    float dx = x1 - x0, dy = y1 - y0;
    float grad = dx == 0 ? 1 : dy / dx;
    float y = y0;
    uint32_t ca = C_A(c);
    for (int x = (int)(x0 + 0.5f); x <= (int)(x1 + 0.5f); x++) {
        int iy = (int)k_floor((double)y);
        float f = y - (float)iy;
        int c1 = (int)((1.0f - f) * (float)ca), c2 = (int)(f * (float)ca);
        if (steep) {
            gfx_pixel_aa(s, iy, x, c, c1);
            gfx_pixel_aa(s, iy + 1, x, c, c2);
        } else {
            gfx_pixel_aa(s, x, iy, c, c1);
            gfx_pixel_aa(s, x, iy + 1, c, c2);
        }
        y += grad;
    }
}

/* thick anti-aliased line with round caps (capsule distance field) */
void gfx_line_w(surface_t *s, float x0, float y0, float x1, float y1, float w, color_t c)
{
    float hw = w * 0.5f;
    int bx0 = (int)(MIN(x0, x1) - hw - 1), bx1 = (int)(MAX(x0, x1) + hw + 2);
    int by0 = (int)(MIN(y0, y1) - hw - 1), by1 = (int)(MAX(y0, y1) + hw + 2);
    rect_t cl = s->clip;
    bx0 = MAX(bx0, cl.x); bx1 = MIN(bx1, cl.x + cl.w);
    by0 = MAX(by0, cl.y); by1 = MIN(by1, cl.y + cl.h);
    float vx = x1 - x0, vy = y1 - y0;
    float len2 = vx * vx + vy * vy;
    uint32_t ca = C_A(c);
    for (int y = by0; y < by1; y++) {
        uint32_t *line = s->px + (size_t)y * s->stride;
        for (int x = bx0; x < bx1; x++) {
            float px = (float)x + 0.5f - x0, py = (float)y + 0.5f - y0;
            float h = len2 > 0 ? (px * vx + py * vy) / len2 : 0;
            if (h < 0) h = 0;
            if (h > 1) h = 1;
            float ex = px - vx * h, ey = py - vy * h;
            float d2 = ex * ex + ey * ey;
            if (d2 > (hw + 1) * (hw + 1)) continue;
            float d = (float)k_sqrt((double)d2);
            float cov = hw - d + 0.5f;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            line[x] = blend(line[x], c, (uint32_t)((float)ca * cov));
        }
    }
}

/* flat triangle, top-left fill rule, no AA */
void gfx_triangle(surface_t *s, float x0, float y0, float x1, float y1, float x2, float y2, color_t c)
{
    int minx = (int)MAX(k_floor(MIN(x0, MIN(x1, x2))), s->clip.x);
    int maxx = (int)MIN(k_floor(MAX(x0, MAX(x1, x2))) + 1, s->clip.x + s->clip.w - 1);
    int miny = (int)MAX(k_floor(MIN(y0, MIN(y1, y2))), s->clip.y);
    int maxy = (int)MIN(k_floor(MAX(y0, MAX(y1, y2))) + 1, s->clip.y + s->clip.h - 1);
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area == 0) return;
    if (area < 0) { float t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
    uint32_t ca = C_A(c);
    for (int y = miny; y <= maxy; y++) {
        float py = (float)y + 0.5f;
        uint32_t *line = s->px + (size_t)y * s->stride;
        for (int x = minx; x <= maxx; x++) {
            float px = (float)x + 0.5f;
            float w0 = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1);
            float w1 = (x0 - x2) * (py - y2) - (y0 - y2) * (px - x2);
            float w2 = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
            if (w0 >= 0 && w1 >= 0 && w2 >= 0)
                line[x] = ca == 255 ? c : blend(line[x], c, ca);
        }
    }
}

/* ------------------------------------------------------------------------
 * blits
 * ---------------------------------------------------------------------- */

static bool clip_blit(surface_t *dst, int *dx, int *dy, surface_t *src, int *sx, int *sy, int *w, int *h)
{
    /* clip against source bounds */
    if (*sx < 0) { *dx -= *sx; *w += *sx; *sx = 0; }
    if (*sy < 0) { *dy -= *sy; *h += *sy; *sy = 0; }
    if (*sx + *w > src->w) *w = src->w - *sx;
    if (*sy + *h > src->h) *h = src->h - *sy;
    /* clip against destination clip */
    rect_t r = rect_intersect(R(*dx, *dy, *w, *h), dst->clip);
    *sx += r.x - *dx;
    *sy += r.y - *dy;
    *dx = r.x; *dy = r.y; *w = r.w; *h = r.h;
    return *w > 0 && *h > 0;
}

void gfx_blit(surface_t *dst, int dx, int dy, surface_t *src, int sx, int sy, int w, int h)
{
    if (!clip_blit(dst, &dx, &dy, src, &sx, &sy, &w, &h)) return;
    for (int j = 0; j < h; j++)
        memcpy(dst->px + (size_t)(dy + j) * dst->stride + dx,
               src->px + (size_t)(sy + j) * src->stride + sx, (size_t)w * 4);
}

void gfx_blit_alpha(surface_t *dst, int dx, int dy, surface_t *src, int sx, int sy, int w, int h, int opacity)
{
    if (opacity <= 0) return;
    if (!clip_blit(dst, &dx, &dy, src, &sx, &sy, &w, &h)) return;
    uint32_t op = (uint32_t)MIN(opacity, 255);
    for (int j = 0; j < h; j++) {
        uint32_t *d = dst->px + (size_t)(dy + j) * dst->stride + dx;
        uint32_t *sp = src->px + (size_t)(sy + j) * src->stride + sx;
        for (int i = 0; i < w; i++) {
            uint32_t p = sp[i];
            uint32_t a = C_A(p);
            if (op != 255) a = (a * op + 127) / 255;
            if (a == 255) d[i] = p;
            else if (a) d[i] = blend(d[i], p, a);
        }
    }
}

void gfx_blit_scaled(surface_t *dst, rect_t d, surface_t *src, rect_t sr)
{
    if (d.w <= 0 || d.h <= 0 || sr.w <= 0 || sr.h <= 0) return;
    rect_t c = rect_intersect(d, dst->clip);
    uint32_t fx = (uint32_t)(((uint64_t)sr.w << 16) / (uint32_t)d.w);
    uint32_t fy = (uint32_t)(((uint64_t)sr.h << 16) / (uint32_t)d.h);
    for (int y = c.y; y < c.y + c.h; y++) {
        int sy = sr.y + (int)(((uint64_t)(y - d.y) * fy) >> 16);
        uint32_t *dl = dst->px + (size_t)y * dst->stride;
        uint32_t *sl = src->px + (size_t)sy * src->stride;
        for (int x = c.x; x < c.x + c.w; x++) {
            int sx = sr.x + (int)(((uint64_t)(x - d.x) * fx) >> 16);
            uint32_t p = sl[sx];
            uint32_t a = C_A(p);
            if (a == 255) dl[x] = p;
            else if (a) dl[x] = blend(dl[x], p, a);
        }
    }
}

/* ------------------------------------------------------------------------
 * blur and shadows
 * ---------------------------------------------------------------------- */

void gfx_box_blur(surface_t *s, rect_t r, int radius, int passes)
{
    r = rect_intersect(r, R(0, 0, s->w, s->h));
    if (rect_empty(r) || radius < 1) return;
    int n = MAX(r.w, r.h);
    uint32_t *tmp = kmalloc((size_t)n * 4);
    if (!tmp) return;
    int div = radius * 2 + 1;
    for (int p = 0; p < passes; p++) {
        /* horizontal */
        for (int y = r.y; y < r.y + r.h; y++) {
            uint32_t *line = s->px + (size_t)y * s->stride + r.x;
            int sr = 0, sg = 0, sb = 0;
            for (int i = -radius; i <= radius; i++) {
                uint32_t c = line[CLAMP(i, 0, r.w - 1)];
                sr += (int)C_R(c); sg += (int)C_G(c); sb += (int)C_B(c);
            }
            for (int x = 0; x < r.w; x++) {
                tmp[x] = RGB(sr / div, sg / div, sb / div);
                uint32_t out = line[CLAMP(x - radius, 0, r.w - 1)];
                uint32_t in = line[CLAMP(x + radius + 1, 0, r.w - 1)];
                sr += (int)C_R(in) - (int)C_R(out);
                sg += (int)C_G(in) - (int)C_G(out);
                sb += (int)C_B(in) - (int)C_B(out);
            }
            memcpy(line, tmp, (size_t)r.w * 4);
        }
        /* vertical */
        for (int x = r.x; x < r.x + r.w; x++) {
            uint32_t *col = s->px + (size_t)r.y * s->stride + x;
            int st = s->stride;
            int sr = 0, sg = 0, sb = 0;
            for (int i = -radius; i <= radius; i++) {
                uint32_t c = col[(size_t)CLAMP(i, 0, r.h - 1) * st];
                sr += (int)C_R(c); sg += (int)C_G(c); sb += (int)C_B(c);
            }
            for (int y = 0; y < r.h; y++) {
                tmp[y] = RGB(sr / div, sg / div, sb / div);
                uint32_t out = col[(size_t)CLAMP(y - radius, 0, r.h - 1) * st];
                uint32_t in = col[(size_t)CLAMP(y + radius + 1, 0, r.h - 1) * st];
                sr += (int)C_R(in) - (int)C_R(out);
                sg += (int)C_G(in) - (int)C_G(out);
                sb += (int)C_B(in) - (int)C_B(out);
            }
            for (int y = 0; y < r.h; y++) col[(size_t)y * st] = tmp[y];
        }
    }
    kfree(tmp);
}

/* Soft drop shadow for a rounded box at (x,y,w,h). Cheap analytic falloff:
 * alpha depends on the distance to the box, squared for a gaussian look. */
void gfx_shadow(surface_t *s, int x, int y, int w, int h, int radius, int spread, int strength)
{
    int x0 = x - spread, y0 = y - spread, x1 = x + w + spread, y1 = y + h + spread;
    rect_t cl = rect_intersect(R(x0, y0, x1 - x0, y1 - y0), s->clip);
    if (rect_empty(cl)) return;
    float fs = (float)spread;
    for (int yy = cl.y; yy < cl.y + cl.h; yy++) {
        uint32_t *line = s->px + (size_t)yy * s->stride;
        float dy = 0;
        if (yy < y + radius) dy = (float)(y + radius - yy);
        else if (yy >= y + h - radius) dy = (float)(yy - (y + h - radius - 1));
        for (int xx = cl.x; xx < cl.x + cl.w; xx++) {
            bool inside = xx >= x && xx < x + w && yy >= y && yy < y + h;
            bool corner = (xx < x + radius || xx >= x + w - radius) &&
                          (yy < y + radius || yy >= y + h - radius);
            if (inside && !corner) {
                /* covered by the box itself: skip ahead */
                bool mid_row = yy >= y + radius && yy < y + h - radius;
                xx = mid_row ? x + w - 1 : x + w - radius - 1;
                continue;
            }
            float dx = 0;
            if (xx < x + radius) dx = (float)(x + radius - xx);
            else if (xx >= x + w - radius) dx = (float)(xx - (x + w - radius - 1));
            float d = (float)k_sqrt((double)(dx * dx + dy * dy)) - (float)radius;
            if (inside && d <= 0) continue;
            if (d < 0) d = 0;
            if (d >= fs) continue;
            float t = 1.0f - d / fs;
            t = t * t;
            uint32_t a = (uint32_t)(t * (float)strength);
            if (a) line[xx] = blend(line[xx], 0xFF000000u, a);
        }
    }
}
