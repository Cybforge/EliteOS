#ifndef ZENITH_GFX_H
#define ZENITH_GFX_H

#include <kernel.h>

typedef uint32_t color_t;   /* 0xAARRGGBB, not premultiplied */

#define RGB(r, g, b)     (0xFF000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define RGBA(r, g, b, a) (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define HEX(v)           (0xFF000000u | (uint32_t)(v))
#define ALPHA(c, a)      (((c) & 0x00FFFFFFu) | ((uint32_t)(a) << 24))
#define C_A(c) (((c) >> 24) & 0xFF)
#define C_R(c) (((c) >> 16) & 0xFF)
#define C_G(c) (((c) >> 8) & 0xFF)
#define C_B(c) ((c) & 0xFF)

typedef struct { int x, y, w, h; } rect_t;

typedef struct surface {
    uint32_t *px;
    int w, h;
    int stride;         /* in pixels */
    rect_t clip;        /* in surface coordinates */
    bool owned;
} surface_t;

static inline rect_t R(int x, int y, int w, int h) { rect_t r = { x, y, w, h }; return r; }
static inline bool rect_empty(rect_t r) { return r.w <= 0 || r.h <= 0; }
static inline bool rect_has(rect_t r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
rect_t rect_intersect(rect_t a, rect_t b);
rect_t rect_union(rect_t a, rect_t b);
bool   rect_overlaps(rect_t a, rect_t b);
rect_t rect_inset(rect_t r, int d);

/* surfaces */
surface_t *surface_new(int w, int h);
void       surface_free(surface_t *s);
surface_t  surface_wrap(uint32_t *px, int w, int h, int stride);
void       surface_reset_clip(surface_t *s);
void       surface_set_clip(surface_t *s, rect_t r);   /* intersected with bounds */

/* color math */
color_t color_lerp(color_t a, color_t b, int t /*0..256*/);
color_t color_mix(color_t a, color_t b, float t);
color_t color_lighten(color_t c, int amount);
color_t color_darken(color_t c, int amount);
color_t color_hsv(float h, float s, float v);
static inline uint32_t blend(uint32_t dst, uint32_t src, uint32_t a)
{
    /* a: 0..255 */
    a += a >> 7;   /* 0..256 */
    uint32_t rb = ((src & 0xFF00FF) * a + (dst & 0xFF00FF) * (256 - a)) >> 8;
    uint32_t g = ((src & 0x00FF00) * a + (dst & 0x00FF00) * (256 - a)) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00) | 0xFF000000u;
}

/* primitives (all clip to s->clip) */
void gfx_clear(surface_t *s, color_t c);
void gfx_fill(surface_t *s, int x, int y, int w, int h, color_t c);
void gfx_fill_r(surface_t *s, rect_t r, color_t c);
void gfx_outline(surface_t *s, int x, int y, int w, int h, color_t c);
void gfx_hline(surface_t *s, int x, int y, int w, color_t c);
void gfx_vline(surface_t *s, int x, int y, int h, color_t c);
void gfx_pixel(surface_t *s, int x, int y, color_t c);
void gfx_pixel_aa(surface_t *s, int x, int y, color_t c, int cov /*0..255*/);
void gfx_vgradient(surface_t *s, int x, int y, int w, int h, color_t top, color_t bottom);
void gfx_hgradient(surface_t *s, int x, int y, int w, int h, color_t left, color_t right);
void gfx_round_rect(surface_t *s, int x, int y, int w, int h, int r, color_t c);
void gfx_round_rect_grad(surface_t *s, int x, int y, int w, int h, int r, color_t top, color_t bottom);
void gfx_round_outline(surface_t *s, int x, int y, int w, int h, int r, color_t c);
void gfx_circle(surface_t *s, float cx, float cy, float r, color_t c);
void gfx_ring(surface_t *s, float cx, float cy, float r, float thickness, color_t c);
void gfx_arc(surface_t *s, float cx, float cy, float r, float thickness, float a0, float a1, color_t c);
void gfx_line(surface_t *s, float x0, float y0, float x1, float y1, color_t c);
void gfx_line_w(surface_t *s, float x0, float y0, float x1, float y1, float width, color_t c);
void gfx_triangle(surface_t *s, float x0, float y0, float x1, float y1, float x2, float y2, color_t c);
void gfx_blit(surface_t *dst, int dx, int dy, surface_t *src, int sx, int sy, int w, int h);
void gfx_blit_alpha(surface_t *dst, int dx, int dy, surface_t *src, int sx, int sy, int w, int h, int opacity);
void gfx_blit_scaled(surface_t *dst, rect_t d, surface_t *src, rect_t s);
void gfx_box_blur(surface_t *s, rect_t r, int radius, int passes);
void gfx_shadow(surface_t *s, int x, int y, int w, int h, int radius, int spread, int strength);

/* fonts: font.c */
typedef struct font font_t;
extern font_t *font_ui, *font_ui_md, *font_ui_lg, *font_ui_bold, *font_title,
              *font_bold_lg, *font_light, *font_huge, *font_mono, *font_mono_bold, *font_chess;

void fonts_init(void);
int  font_height(font_t *f);
int  font_ascent(font_t *f);
int  font_text_width(font_t *f, const char *utf8);
int  font_text_width_n(font_t *f, const char *utf8, int bytes);
int  font_char_width(font_t *f, uint32_t cp);
int  gfx_text(surface_t *s, font_t *f, int x, int y, const char *utf8, color_t c);
int  gfx_text_n(surface_t *s, font_t *f, int x, int y, const char *utf8, int bytes, color_t c);
int  gfx_char(surface_t *s, font_t *f, int x, int y, uint32_t cp, color_t c);
void gfx_text_center(surface_t *s, font_t *f, rect_t r, const char *utf8, color_t c);
void gfx_text_right(surface_t *s, font_t *f, int right_x, int y, const char *utf8, color_t c);
int  gfx_text_ellipsis(surface_t *s, font_t *f, int x, int y, int max_w, const char *utf8, color_t c);
int  gfx_text_wrap(surface_t *s, font_t *f, rect_t r, const char *utf8, color_t c, int line_gap);

uint32_t utf8_next(const char **s);
int      utf8_encode(uint32_t cp, char *out);
int      utf8_prev_len(const char *start, const char *p);

#endif
