/*
 * Compositing window manager.
 *
 * Every window has two buffers: the app draws into `back` and calls
 * wm_present(), which swaps it with `front`. The compositor thread keeps a
 * list of dirty rectangles and rebuilds only those parts of the screen:
 * wallpaper, windows (shadows, rounded corners, fades), shell overlay and
 * the cursor, into an off-screen buffer that is then copied to the
 * framebuffer.
 */
#include <sys.h>
#include "wm_internal.h"
#include <audio.h>
#include <dev.h>
#include <gpu.h>
#include <mm.h>
#include <sched.h>
#include <x86.h>

extern struct bootinfo *boot_info;

mutex_t wm_mtx = MUTEX_INIT("wm");
window_t *wm_list;
window_t *wm_focus_win;
int wm_mx, wm_my;
int wm_mods;
volatile bool wm_ready;
bool ui_transparency = true;
bool ui_animations = true;

static surface_t *screen;
static uint32_t *fb;
static int fb_stride;
static bool fb_swap;
static void (*present_hook)(int x, int y, int w, int h);
static int frame_ms = 14;                       /* one frame of the monitor's refresh rate */
static int SW, SH;
static int next_id = 1;

#define MAX_DIRTY 24
static rect_t dirty[MAX_DIRTY];
static int ndirty;

static spinlock_t wake_lock = SPINLOCK_INIT("wm-wake");
static volatile int wake_pending;

static int mbuttons;
static window_t *drag_win;
static int drag_mode;           /* 1 move, 2 resize */
static int drag_edges;          /* resize: 1 right, 2 bottom */
static int drag_ox, drag_oy, drag_w0, drag_h0;
static window_t *capture_win;
static window_t *hover_win;
static uint64_t last_click_t;
static int last_click_x, last_click_y, click_count;
static bool super_alone;
static bool alt_tabbing;
static int cursor_kind;         /* 0 arrow, 1 resize diag, 2 resize h, 3 resize v */
static uint32_t fps_count, fps_value;
static uint64_t fps_t0;

/* ------------------------------------------------------------------------
 * theme
 * ---------------------------------------------------------------------- */

struct theme theme;
const color_t accent_choices[] = {
    HEX(0x7C5CFF), HEX(0x00B4FF), HEX(0x2ED47A), HEX(0xFF7A45), HEX(0xFF4F8B), HEX(0xFFC233),
};
const char *accent_names[] = { "Violet", "Ocean", "Mint", "Sunset", "Rose", "Gold" };
int accent_count = 6;
int accent_index = 0;

void theme_set_accent(int idx)
{
    accent_index = CLAMP(idx, 0, accent_count - 1);
    theme.accent = accent_choices[accent_index];
    theme.accent2 = color_lerp(theme.accent, HEX(0x00D4FF), 110);
}

static void theme_init(void)
{
    theme.bg = HEX(0x151827);
    theme.panel = HEX(0x1B1F33);
    theme.surface = HEX(0x232842);
    theme.surface2 = HEX(0x2C3250);
    theme.border = HEX(0x323956);
    theme.text = HEX(0xE9EBF8);
    theme.text_dim = HEX(0x9AA0C3);
    theme.text_faint = HEX(0x646B90);
    theme.danger = HEX(0xFF5C7A);
    theme.success = HEX(0x35D69A);
    theme.warning = HEX(0xFFB547);
    theme.title_active = HEX(0x1E2238);
    theme.title_inactive = HEX(0x181B2C);
    theme_set_accent(0);
}

/* ------------------------------------------------------------------------
 * cursor bitmaps (rasterised with 4x4 supersampling)
 * ---------------------------------------------------------------------- */

#define CUR_SZ 32
static surface_t *cursors[4];
static const int cursor_hot[4][2] = { { 1, 1 }, { 11, 11 }, { 11, 6 }, { 6, 11 } };

static bool in_poly(const float *pts, int n, float x, float y)
{
    bool in = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float xi = pts[i * 2], yi = pts[i * 2 + 1], xj = pts[j * 2], yj = pts[j * 2 + 1];
        if (((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) in = !in;
    }
    return in;
}

static float dist_to_poly(const float *pts, int n, float x, float y)
{
    float best = 1e9f;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float ax = pts[j * 2], ay = pts[j * 2 + 1], bx = pts[i * 2], by = pts[i * 2 + 1];
        float vx = bx - ax, vy = by - ay, wx = x - ax, wy = y - ay;
        float l2 = vx * vx + vy * vy;
        float t = l2 > 0 ? (wx * vx + wy * vy) / l2 : 0;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        float dx = wx - vx * t, dy = wy - vy * t;
        float d = (float)k_sqrt((double)(dx * dx + dy * dy));
        if (d < best) best = d;
    }
    return best;
}

static surface_t *make_cursor(const float *pts, int n)
{
    surface_t *s = surface_new(CUR_SZ, CUR_SZ);
    for (int y = 0; y < CUR_SZ; y++) {
        for (int x = 0; x < CUR_SZ; x++) {
            int fill = 0, edge = 0, shadow = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    float fx = (float)x + ((float)sx + 0.5f) / 4.0f;
                    float fy = (float)y + ((float)sy + 0.5f) / 4.0f;
                    bool in = in_poly(pts, n, fx, fy);
                    float d = dist_to_poly(pts, n, fx, fy);
                    if (in && d > 1.2f) fill++;
                    else if (in || d < 0.9f) edge++;
                    if (in_poly(pts, n, fx - 1.0f, fy - 1.5f) || dist_to_poly(pts, n, fx - 1.0f, fy - 1.5f) < 1.2f)
                        shadow++;
                }
            }
            uint32_t px = 0;
            if (shadow) px = RGBA(0, 0, 0, shadow * 70 / 16);
            if (edge + fill) {
                /* dark outline, white body */
                int total = edge + fill;
                int wv = fill * 255 / total;
                color_t c = RGB(wv * 250 / 255 + 5, wv * 250 / 255 + 5, wv * 250 / 255 + 5);
                int a = total * 255 / 16;
                uint32_t under = px;
                int ua = C_A(under);
                int oa = a + ua * (255 - a) / 255;
                px = oa ? ((uint32_t)oa << 24) | (c & 0xFFFFFF) : 0;
            }
            s->px[y * CUR_SZ + x] = px;
        }
    }
    return s;
}

static void cursors_init(void)
{
    const float arrow[] = { 1, 1, 1, 18.5f, 5.2f, 14.6f, 8.3f, 21.5f, 11.2f, 20.2f, 8.2f, 13.5f, 13.8f, 13.5f };
    cursors[0] = make_cursor(arrow, 7);
    const float diag[] = { 3, 3, 10, 3, 7.5f, 5.5f, 16.5f, 14.5f, 19, 12, 19, 19, 12, 19, 14.5f, 16.5f,
                           5.5f, 7.5f, 3, 10 };
    cursors[1] = make_cursor(diag, 10);
    const float horiz[] = { 1, 6, 6, 1, 6, 4.5f, 16, 4.5f, 16, 1, 21, 6, 16, 11, 16, 7.5f, 6, 7.5f, 6, 11 };
    cursors[2] = make_cursor(horiz, 10);
    const float vert[] = { 6, 1, 11, 6, 7.5f, 6, 7.5f, 16, 11, 16, 6, 21, 1, 16, 4.5f, 16, 4.5f, 6, 1, 6 };
    cursors[3] = make_cursor(vert, 10);
}

static rect_t cursor_rect(void)
{
    return R(wm_mx - cursor_hot[cursor_kind][0], wm_my - cursor_hot[cursor_kind][1], CUR_SZ, CUR_SZ);
}

static void set_cursor(int kind)
{
    if (kind == cursor_kind) return;
    wm_dirty(cursor_rect());
    cursor_kind = kind;
    wm_dirty(cursor_rect());
}

/* ------------------------------------------------------------------------
 * geometry
 * ---------------------------------------------------------------------- */

static inline bool decorated(window_t *w) { return !(w->flags & WF_NO_DECOR); }
static inline bool rounded(window_t *w) { return w->state != WS_MAXIMIZED; }
static inline int title_h(window_t *w) { return decorated(w) ? TITLE_H : 0; }

rect_t wm_frame_rect(window_t *w)
{
    return R(w->x, w->y + w->anim_dy, w->cw, w->ch + title_h(w));
}

rect_t wm_shadow_rect(window_t *w)
{
    rect_t r = wm_frame_rect(w);
    if (!decorated(w) || !rounded(w)) return r;
    return R(r.x - 24, r.y - 20, r.w + 48, r.h + 50);
}

static rect_t client_rect(window_t *w)
{
    return R(w->x, w->y + w->anim_dy + title_h(w), w->cw, w->ch);
}

/* ------------------------------------------------------------------------
 * dirty tracking
 * ---------------------------------------------------------------------- */

void wm_dirty(rect_t r)
{
    r = rect_intersect(r, R(0, 0, SW, SH));
    if (rect_empty(r)) return;
    for (int i = 0; i < ndirty; i++) {
        rect_t u = rect_union(dirty[i], r);
        /* merge if the union does not waste much area */
        long area = (long)u.w * u.h;
        long sum = (long)dirty[i].w * dirty[i].h + (long)r.w * r.h;
        if (rect_overlaps(dirty[i], r) || area <= sum + sum / 4) {
            dirty[i] = u;
            return;
        }
    }
    if (ndirty < MAX_DIRTY) {
        dirty[ndirty++] = r;
        return;
    }
    for (int i = 1; i < ndirty; i++) dirty[0] = rect_union(dirty[0], dirty[i]);
    dirty[0] = rect_union(dirty[0], r);
    ndirty = 1;
}

void wm_dirty_window(window_t *w) { wm_dirty(wm_shadow_rect(w)); }

void wm_invalidate_all(void)
{
    mutex_lock(&wm_mtx);
    wm_dirty(R(0, 0, SW, SH));
    mutex_unlock(&wm_mtx);
    wm_kick();
}

void wm_kick(void)
{
    spin_lock(&wake_lock);
    wake_pending = 1;
    spin_unlock(&wake_lock);
    sched_wake(input_chan());
}

/* ------------------------------------------------------------------------
 * drawing windows
 * ---------------------------------------------------------------------- */

static uint8_t cmask[WIN_RADIUS][WIN_RADIUS];   /* coverage, [row][col] from the corner */

static void corner_mask_init(void)
{
    float r = (float)WIN_RADIUS;
    for (int j = 0; j < WIN_RADIUS; j++) {
        for (int i = 0; i < WIN_RADIUS; i++) {
            int cov = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float dx = r - ((float)i + ((float)sx + 0.5f) / 4.0f);
                    float dy = r - ((float)j + ((float)sy + 0.5f) / 4.0f);
                    if (dx * dx + dy * dy <= r * r) cov++;
                }
            cmask[j][i] = (uint8_t)(cov * 255 / 16);
        }
    }
}

/* coverage of pixel (x,y) inside a box with rounded corners (top/bottom selectable) */
static inline int round_cov(rect_t b, int x, int y, bool top, bool bottom)
{
    int R_ = WIN_RADIUS;
    int dx = -1, dy = -1;
    if (x < b.x + R_) dx = x - b.x;
    else if (x >= b.x + b.w - R_) dx = b.x + b.w - 1 - x;
    if (dx < 0) return 255;
    if (top && y < b.y + R_) dy = y - b.y;
    else if (bottom && y >= b.y + b.h - R_) dy = b.y + b.h - 1 - y;
    if (dy < 0) return 255;
    return cmask[dy][dx];
}

static void fill_masked(rect_t box, rect_t clip, color_t top_c, color_t bot_c, int op, bool rtop, bool rbot)
{
    rect_t v = rect_intersect(box, clip);
    for (int y = v.y; y < v.y + v.h; y++) {
        color_t c = box.h > 1 ? color_lerp(top_c, bot_c, (y - box.y) * 256 / (box.h - 1)) : top_c;
        uint32_t *line = screen->px + (size_t)y * screen->stride;
        bool edge_row = (rtop && y < box.y + WIN_RADIUS) || (rbot && y >= box.y + box.h - WIN_RADIUS);
        for (int x = v.x; x < v.x + v.w; x++) {
            int a = op;
            if (edge_row) a = a * round_cov(box, x, y, rtop, rbot) / 255;
            if (a >= 255) line[x] = c;
            else if (a > 0) line[x] = blend(line[x], c, (uint32_t)a);
        }
    }
}

static void blit_client(window_t *w, rect_t cr, rect_t clip, int op, bool rbot)
{
    surface_t *src = w->front;
    if (!src) return;
    rect_t v = rect_intersect(rect_intersect(cr, clip), R(cr.x, cr.y, src->w, src->h));
    bool alpha = (w->flags & WF_TRANSLUCENT) != 0;
    rect_t box = R(cr.x, cr.y - title_h(w), cr.w, cr.h + title_h(w));
    bool rtop = rounded(w) && !decorated(w);
    for (int y = v.y; y < v.y + v.h; y++) {
        uint32_t *d = screen->px + (size_t)y * screen->stride;
        uint32_t *s = src->px + (size_t)(y - cr.y) * src->stride - cr.x;
        bool edge_row = (rbot && y >= box.y + box.h - WIN_RADIUS) || (rtop && y < box.y + WIN_RADIUS);
        if (!edge_row && !alpha && op >= 255) {
            memcpy(d + v.x, s + v.x, (size_t)v.w * 4);
            continue;
        }
        for (int x = v.x; x < v.x + v.w; x++) {
            uint32_t p = s[x];
            int a = alpha ? (int)C_A(p) : 255;
            a = a * op / 255;
            if (edge_row) a = a * round_cov(box, x, y, rtop, rbot) / 255;
            if (a >= 255) d[x] = p | 0xFF000000u;
            else if (a > 0) d[x] = blend(d[x], p, (uint32_t)a);
        }
    }
}

enum { HIT_NONE, HIT_CLIENT, HIT_TITLE, HIT_CLOSE, HIT_MAX, HIT_MIN, HIT_RESIZE };

static rect_t title_button(window_t *w, int which)
{
    /* which: 0 close, 1 maximize, 2 minimize */
    rect_t f = wm_frame_rect(w);
    int bw = 46;
    return R(f.x + f.w - bw * (which + 1), f.y, bw, TITLE_H);
}

static bool has_button(window_t *w, int which)
{
    if (which == 1) return (w->flags & WF_RESIZABLE) != 0;
    if (which == 2) return !(w->flags & WF_DIALOG);
    return true;
}

static void draw_title_glyph(int which, rect_t r, color_t c)
{
    float cx = (float)r.x + (float)r.w / 2.0f, cy = (float)r.y + (float)r.h / 2.0f;
    if (which == 0) {
        gfx_line_w(screen, cx - 5, cy - 5, cx + 5, cy + 5, 1.4f, c);
        gfx_line_w(screen, cx + 5, cy - 5, cx - 5, cy + 5, 1.4f, c);
    } else if (which == 1) {
        gfx_round_outline(screen, (int)cx - 5, (int)cy - 5, 11, 11, 2, c);
    } else {
        gfx_fill(screen, (int)cx - 5, (int)cy, 11, 1, c);
    }
}

static void draw_window(window_t *w, rect_t clip)
{
    int op = w->opacity * w->user_opacity / 255;
    if (op <= 0) return;
    bool focused = w == wm_focus_win;
    rect_t f = wm_frame_rect(w);
    bool rnd = rounded(w);

    surface_set_clip(screen, clip);
    if (decorated(w) && rnd && rect_overlaps(wm_shadow_rect(w), clip)) {
        int strength = (focused ? 110 : 70) * op / 255;
        gfx_shadow(screen, f.x, f.y + 6, f.w, f.h, WIN_RADIUS, 22, strength);
    }

    if (decorated(w)) {
        rect_t tb = R(f.x, f.y, f.w, TITLE_H);
        if (rect_overlaps(tb, clip)) {
            color_t tc = focused ? theme.title_active : theme.title_inactive;
            fill_masked(tb, clip, color_lighten(tc, 6), tc, op, rnd, false);
            rect_t tclip = rect_intersect(clip, tb);
            surface_set_clip(screen, tclip);
            color_t txt = focused ? theme.text : theme.text_dim;
            if (op < 255) txt = ALPHA(txt, op);
            int tx = f.x + 14;
            if (w->icon) {
                icon_draw(screen, w->icon, tx, f.y + (TITLE_H - 18) / 2, 18);
                tx += 26;
            }
            int max_title = f.w - (tx - f.x) - 46 * 3 - 8;
            gfx_text_ellipsis(screen, font_ui_md, tx, f.y + (TITLE_H - font_height(font_ui_md)) / 2 + 1,
                              max_title, w->title, txt);
            for (int b = 0; b < 3; b++) {
                if (!has_button(w, b)) continue;
                rect_t br = title_button(w, b);
                bool hov = w->hover_button == b + 1;
                if (hov) {
                    color_t hc = b == 0 ? HEX(0xE8465E) : ALPHA(0xFFFFFF, 22);
                    if (b == 0 && rnd) fill_masked(br, tclip, hc, hc, op, true, false);
                    else if (b == 0) gfx_fill_r(screen, br, ALPHA(hc, op));
                    else gfx_fill_r(screen, br, ALPHA(hc, 22 * op / 255));
                }
                color_t gc = (hov && b == 0) ? 0xFFFFFFFFu : (focused ? theme.text : theme.text_faint);
                draw_title_glyph(b, br, op < 255 ? ALPHA(gc, op) : gc);
            }
            surface_set_clip(screen, clip);
        }
    }

    rect_t cr = client_rect(w);
    if (rect_overlaps(cr, clip)) {
        /* area not covered by the (possibly smaller) front buffer */
        if (w->front && (w->front->w < cr.w || w->front->h < cr.h))
            fill_masked(cr, clip, theme.bg, theme.bg, op, false, rnd);
        blit_client(w, cr, clip, op, rnd);
    }

    if (decorated(w) && rnd) {
        surface_set_clip(screen, clip);
        gfx_round_outline(screen, f.x, f.y, f.w, f.h, WIN_RADIUS, ALPHA(0xFFFFFF, (focused ? 34 : 20) * op / 255));
    }
}

/* ------------------------------------------------------------------------
 * compose
 * ---------------------------------------------------------------------- */

static bool covers(window_t *w, rect_t r)
{
    if (w->opacity < 255 || w->user_opacity < 255 || (w->flags & WF_TRANSLUCENT)) return false;
    if (w->state == WS_MINIMIZED || w->anim) return false;
    rect_t f = wm_frame_rect(w);
    if (w->front && (w->front->w < w->cw || w->front->h < w->ch)) return false;
    if (!rounded(w)) return f.x <= r.x && f.y <= r.y && f.x + f.w >= r.x + r.w && f.y + f.h >= r.y + r.h;
    rect_t a = R(f.x, f.y + WIN_RADIUS, f.w, f.h - 2 * WIN_RADIUS);
    rect_t b = R(f.x + WIN_RADIUS, f.y, f.w - 2 * WIN_RADIUS, f.h);
    rect_t ra = rect_intersect(a, r), rb = rect_intersect(b, r);
    return (ra.w == r.w && ra.h == r.h) || (rb.w == r.w && rb.h == r.h);
}

static void compose(rect_t r)
{
    window_t *start = NULL;
    for (window_t *w = wm_list; w; w = w->next)
        if (covers(w, r)) start = w;

    surface_set_clip(screen, r);
    if (!start) desktop_draw_background(screen, r);
    for (window_t *w = start ? start : wm_list; w; w = w->next) {
        if (w->state == WS_MINIMIZED && !w->anim) continue;
        if (!rect_overlaps(wm_shadow_rect(w), r)) continue;
        draw_window(w, r);
    }
    surface_set_clip(screen, r);
    desktop_draw_overlay(screen, r);
    surface_set_clip(screen, r);
    rect_t cur = cursor_rect();
    if (rect_overlaps(cur, r))
        gfx_blit_alpha(screen, cur.x, cur.y, cursors[cursor_kind], 0, 0, CUR_SZ, CUR_SZ, 255);
}

static void flush(rect_t r)
{
    for (int y = r.y; y < r.y + r.h; y++) {
        uint32_t *s = screen->px + (size_t)y * screen->stride + r.x;
        uint32_t *d = fb + (size_t)y * fb_stride + r.x;
        if (!fb_swap) {
            memcpy(d, s, (size_t)r.w * 4);
        } else {
            for (int x = 0; x < r.w; x++) {
                uint32_t p = s[x];
                d[x] = (p & 0xFF00FF00u) | ((p >> 16) & 0xFF) | ((p & 0xFF) << 16);
            }
        }
    }
    if (present_hook) present_hook(r.x, r.y, r.w, r.h);
}

/* ------------------------------------------------------------------------
 * window list management (wm_mtx held)
 * ---------------------------------------------------------------------- */

static void list_remove(window_t *w)
{
    window_t **pp = &wm_list;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) *pp = w->next;
    w->next = NULL;
}

static void list_push_top(window_t *w)
{
    /* topmost windows stay above normal ones */
    window_t **pp = &wm_list;
    if (!(w->flags & WF_TOPMOST)) {
        while (*pp && !((*pp)->flags & WF_TOPMOST)) pp = &(*pp)->next;
    } else {
        while (*pp) pp = &(*pp)->next;
    }
    w->next = *pp;
    *pp = w;
}

void wm_raise(window_t *w)
{
    list_remove(w);
    list_push_top(w);
    wm_dirty_window(w);
}

window_t *wm_top_visible(void)
{
    window_t *top = NULL;
    for (window_t *w = wm_list; w; w = w->next)
        if (w->state != WS_MINIMIZED && w->anim != 2 && !w->destroyed && !(w->flags & WF_NO_TASKBAR)) top = w;
    return top;
}

void wm_focus_locked(window_t *w)
{
    if (w == wm_focus_win) return;
    window_t *old = wm_focus_win;
    wm_focus_win = w;
    struct gui_event ev = { 0 };
    if (old && !old->destroyed) {
        ev.type = EV_BLUR;
        wm_post_event(old, &ev);
        wm_dirty_window(old);
    }
    if (w) {
        ev.type = EV_FOCUS;
        wm_post_event(w, &ev);
        wm_dirty_window(w);
    }
    desktop_windows_changed();
}

void wm_focus(window_t *w)
{
    mutex_lock(&wm_mtx);
    if (w->state == WS_MINIMIZED) wm_restore(w);
    wm_raise(w);
    wm_focus_locked(w);
    mutex_unlock(&wm_mtx);
    wm_kick();
}

void wm_minimize(window_t *w)
{
    if (w->state == WS_MINIMIZED) return;
    w->anim = ui_animations ? 3 : 0;
    w->anim_start = uptime_ms();
    if (!ui_animations) { w->state = WS_MINIMIZED; w->opacity = 0; }
    wm_dirty_window(w);
    if (w == wm_focus_win) {
        wm_focus_win = NULL;
        window_t *top = NULL;
        for (window_t *x = wm_list; x; x = x->next)
            if (x != w && x->state != WS_MINIMIZED && !x->destroyed && x->anim != 2) top = x;
        wm_focus_locked(top);
    }
    desktop_windows_changed();
}

void wm_restore(window_t *w)
{
    if (w->state != WS_MINIMIZED && w->anim != 3) return;
    w->state = w->restore.w && w->state == WS_MAXIMIZED ? WS_MAXIMIZED : WS_NORMAL;
    if (w->anim == 3) w->state = WS_NORMAL;
    w->anim = ui_animations ? 4 : 0;
    w->anim_start = uptime_ms();
    if (!ui_animations) w->opacity = 255;
    wm_dirty_window(w);
    desktop_windows_changed();
}

static void set_request(window_t *w, int cw, int ch)
{
    cw = MAX(cw, w->min_w);
    ch = MAX(ch, w->min_h);
    if (cw == w->req_w && ch == w->req_h) return;
    w->req_w = cw;
    w->req_h = ch;
    struct gui_event ev = { 0 };
    ev.type = EV_RESIZE;
    ev.x = cw;
    ev.y = ch;
    wm_post_event(w, &ev);
}

void wm_toggle_maximize(window_t *w)
{
    if (!(w->flags & WF_RESIZABLE)) return;
    wm_dirty_window(w);
    if (w->state == WS_MAXIMIZED) {
        w->state = WS_NORMAL;
        w->x = w->restore.x;
        w->y = w->restore.y;
        set_request(w, w->restore.w, w->restore.h);
    } else {
        w->restore = R(w->x, w->y, w->cw, w->ch);
        w->state = WS_MAXIMIZED;
        w->x = 0;
        w->y = 0;
        set_request(w, SW, SH - desktop_taskbar_h() - title_h(w));
    }
    wm_dirty_window(w);
}

void wm_request_close(window_t *w)
{
    struct gui_event ev = { 0 };
    ev.type = EV_CLOSE;
    wm_post_event(w, &ev);
}

/* ------------------------------------------------------------------------
 * public window API
 * ---------------------------------------------------------------------- */

static int cascade;

window_t *wm_create(const char *title, int cw, int ch, int flags)
{
    window_t *w = kzalloc(sizeof(*w));
    if (!w) return NULL;
    strlcpy(w->title, title, sizeof(w->title));
    w->flags = flags;
    cw = MIN(cw, SW);
    ch = MIN(ch, SH - desktop_taskbar_h() - ((flags & WF_NO_DECOR) ? 0 : TITLE_H));
    w->cw = w->req_w = cw;
    w->ch = w->req_h = ch;
    w->min_w = 200;
    w->min_h = 120;
    w->front = surface_new(cw, ch);
    w->back = surface_new(cw, ch);
    if (!w->front || !w->back) {
        surface_free(w->front);
        surface_free(w->back);
        kfree(w);
        return NULL;
    }
    gfx_clear(w->front, theme.bg);
    gfx_clear(w->back, theme.bg);
    w->user_opacity = 255;
    w->opacity = ui_animations ? 0 : 255;
    w->anim = ui_animations ? 1 : 0;
    w->anim_dy = ui_animations ? 14 : 0;
    w->anim_start = uptime_ms();
    spin_init(&w->evlock, "win-ev");
    w->owner_thread = thread_current();

    mutex_lock(&wm_mtx);
    w->id = next_id++;
    int fh = ch + ((flags & WF_NO_DECOR) ? 0 : TITLE_H);
    int avail_h = SH - desktop_taskbar_h();
    if (flags & WF_CENTER) {
        w->x = (SW - cw) / 2;
        w->y = (avail_h - fh) / 2;
    } else {
        w->x = (SW - cw) / 2 - 120 + cascade * 32;
        w->y = (avail_h - fh) / 2 - 80 + cascade * 28;
        cascade = (cascade + 1) % 8;
    }
    w->x = CLAMP(w->x, 0, MAX(0, SW - cw));
    w->y = CLAMP(w->y, 0, MAX(0, avail_h - fh));
    list_push_top(w);
    wm_focus_locked(w);
    wm_dirty_window(w);
    desktop_windows_changed();
    mutex_unlock(&wm_mtx);
    wm_kick();
    return w;
}

surface_t *wm_begin(window_t *w)
{
    int rw = w->req_w, rh = w->req_h;
    if (!w->back || w->back->w != rw || w->back->h != rh) {
        surface_t *n = surface_new(rw, rh);
        if (n) {
            surface_free(w->back);
            w->back = n;
        }
    }
    surface_reset_clip(w->back);
    return w->back;
}

void wm_present(window_t *w)
{
    mutex_lock(&wm_mtx);
    surface_t *t = w->front;
    w->front = w->back;
    w->back = t;
    if (w->front->w != w->cw || w->front->h != w->ch) {
        wm_dirty_window(w);
        w->cw = w->front->w;
        w->ch = w->front->h;
        wm_dirty_window(w);
    } else if (w->state != WS_MINIMIZED) {
        wm_dirty(client_rect(w));
    }
    mutex_unlock(&wm_mtx);
    wm_kick();
}

void wm_post_event(window_t *w, const struct gui_event *ev)
{
    spin_lock(&w->evlock);
    if (ev->type == EV_MOUSE_MOVE && w->ev_head != w->ev_tail) {
        struct gui_event *last = &w->evq[(w->ev_head - 1) % EVQ_SIZE];
        if (last->type == EV_MOUSE_MOVE) {
            *last = *ev;
            spin_unlock(&w->evlock);
            sched_wake(w);
            return;
        }
    }
    if (w->ev_head - w->ev_tail < EVQ_SIZE) {
        w->evq[w->ev_head % EVQ_SIZE] = *ev;
        w->ev_head++;
    }
    spin_unlock(&w->evlock);
    sched_wake(w);
}

bool wm_wait_event(window_t *w, struct gui_event *ev, int timeout_ms)
{
    uint64_t deadline = timeout_ms > 0 ? uptime_ms() + (uint64_t)timeout_ms : 0;
    spin_lock(&w->evlock);
    for (;;) {
        if (w->ev_head != w->ev_tail) {
            *ev = w->evq[w->ev_tail % EVQ_SIZE];
            w->ev_tail++;
            spin_unlock(&w->evlock);
            return true;
        }
        uint64_t now = uptime_ms();
        if (w->timer_ms && now >= w->next_timer) {
            w->next_timer = MAX(w->next_timer + w->timer_ms, now + 1);
            spin_unlock(&w->evlock);
            memset(ev, 0, sizeof(*ev));
            ev->type = EV_TIMER;
            return true;
        }
        if (timeout_ms == 0 || (deadline && now >= deadline)) {
            spin_unlock(&w->evlock);
            return false;
        }
        uint64_t wait = 0;
        if (deadline) wait = deadline - now;
        if (w->timer_ms) {
            uint64_t tw = w->next_timer - now;
            if (!wait || tw < wait) wait = tw;
        }
        sched_wait(w, &w->evlock, wait ? wait : 0);
    }
}

void wm_set_timer(window_t *w, uint64_t ms)
{
    spin_lock(&w->evlock);
    w->timer_ms = ms;
    w->next_timer = uptime_ms() + ms;
    spin_unlock(&w->evlock);
    sched_wake(w);
}

void wm_set_title(window_t *w, const char *t)
{
    mutex_lock(&wm_mtx);
    strlcpy(w->title, t, sizeof(w->title));
    wm_dirty(R(w->x, w->y, w->cw, TITLE_H));
    desktop_windows_changed();
    mutex_unlock(&wm_mtx);
    wm_kick();
}

void wm_set_icon(window_t *w, int icon)
{
    mutex_lock(&wm_mtx);
    w->icon = icon;
    wm_dirty(R(w->x, w->y, w->cw, TITLE_H));
    desktop_windows_changed();
    mutex_unlock(&wm_mtx);
    wm_kick();
}

void wm_set_min_size(window_t *w, int mw, int mh)
{
    w->min_w = mw;
    w->min_h = mh;
}

void wm_request_resize(window_t *w, int cw, int ch)
{
    mutex_lock(&wm_mtx);
    set_request(w, cw, ch);
    mutex_unlock(&wm_mtx);
}

void wm_destroy(window_t *w)
{
    mutex_lock(&wm_mtx);
    if (capture_win == w) capture_win = NULL;
    if (drag_win == w) { drag_win = NULL; drag_mode = 0; }
    if (hover_win == w) hover_win = NULL;
    w->anim = ui_animations ? 2 : 0;
    w->anim_start = uptime_ms();
    w->close_requested = true;
    if (!ui_animations) w->destroyed = true;
    wm_dirty_window(w);
    if (wm_focus_win == w) {
        wm_focus_win = NULL;
        window_t *top = NULL;
        for (window_t *x = wm_list; x; x = x->next)
            if (x != w && x->state != WS_MINIMIZED && !x->destroyed && !x->close_requested) top = x;
        wm_focus_locked(top);
    }
    desktop_windows_changed();
    mutex_unlock(&wm_mtx);
    wm_kick();
}

int wm_window_list(window_t **out, int max)
{
    int n = 0;
    for (window_t *w = wm_list; w && n < max; w = w->next)
        if (!w->close_requested && !(w->flags & WF_NO_TASKBAR)) out[n++] = w;
    return n;
}

int wm_screen_w(void) { return SW; }
int wm_screen_h(void) { return SH; }
uint32_t wm_fps(void) { return fps_value; }

void wm_notify(const char *title, const char *body, int icon)
{
    mutex_lock(&wm_mtx);
    desktop_notify(title, body, icon);
    mutex_unlock(&wm_mtx);
    wm_kick();
}

/* ------------------------------------------------------------------------
 * animations
 * ---------------------------------------------------------------------- */

static float ease_out(float t) { t = 1.0f - t; return 1.0f - t * t * t; }

static bool run_animations(uint64_t now)
{
    bool active = false;
    window_t *w = wm_list;
    while (w) {
        window_t *next = w->next;
        if (w->anim) {
            float dur = w->anim == 1 ? 200.0f : w->anim == 2 ? 150.0f : 180.0f;
            float t = (float)(now - w->anim_start) / dur;
            if (t > 1) t = 1;
            wm_dirty_window(w);
            switch (w->anim) {
            case 1: case 4:
                w->opacity = (uint8_t)(255.0f * ease_out(t));
                w->anim_dy = (int)((1.0f - ease_out(t)) * 14.0f);
                break;
            case 2:
                w->opacity = (uint8_t)(255.0f * (1.0f - t));
                w->anim_dy = (int)(t * 10.0f);
                break;
            case 3:
                w->opacity = (uint8_t)(255.0f * (1.0f - t));
                w->anim_dy = (int)(t * 40.0f);
                break;
            }
            wm_dirty_window(w);
            if (t >= 1) {
                if (w->anim == 2) w->destroyed = true;
                if (w->anim == 3) { w->state = WS_MINIMIZED; w->opacity = 0; }
                w->anim_dy = 0;
                w->anim = 0;
                if (w->state != WS_MINIMIZED && !w->destroyed) w->opacity = 255;
            } else {
                active = true;
            }
        }
        if (w->destroyed) {
            wm_dirty_window(w);
            list_remove(w);
            surface_free(w->front);
            surface_free(w->back);
            kfree(w);
            desktop_windows_changed();
        }
        w = next;
    }
    return active;
}

/* ------------------------------------------------------------------------
 * input
 * ---------------------------------------------------------------------- */

static window_t *window_at(int x, int y)
{
    window_t *hit = NULL;
    for (window_t *w = wm_list; w; w = w->next) {
        if (w->state == WS_MINIMIZED || w->close_requested || w->anim == 3) continue;
        rect_t f = wm_frame_rect(w);
        rect_t grab = (w->flags & WF_RESIZABLE) && w->state == WS_NORMAL ? R(f.x, f.y, f.w + 6, f.h + 6) : f;
        if (rect_has(grab, x, y)) hit = w;
    }
    return hit;
}

static int hit_test(window_t *w, int x, int y, int *edges)
{
    rect_t f = wm_frame_rect(w);
    *edges = 0;
    if ((w->flags & WF_RESIZABLE) && w->state == WS_NORMAL) {
        if (x >= f.x + f.w - 6) *edges |= 1;
        if (y >= f.y + f.h - 6) *edges |= 2;
        if (*edges) return HIT_RESIZE;
    }
    if (!rect_has(f, x, y)) return HIT_NONE;
    if (decorated(w) && y < f.y + TITLE_H) {
        for (int b = 0; b < 3; b++)
            if (has_button(w, b) && rect_has(title_button(w, b), x, y))
                return b == 0 ? HIT_CLOSE : b == 1 ? HIT_MAX : HIT_MIN;
        return HIT_TITLE;
    }
    return HIT_CLIENT;
}

static void send_mouse(window_t *w, int type, int button, int wheel)
{
    rect_t cr = client_rect(w);
    struct gui_event ev = { 0 };
    ev.type = type;
    ev.x = wm_mx - cr.x;
    ev.y = wm_my - cr.y;
    ev.button = button;
    ev.buttons = mbuttons;
    ev.wheel = wheel;
    ev.mods = wm_mods;
    ev.clicks = click_count;
    wm_post_event(w, &ev);
}

static void set_hover_button(window_t *w, int b)
{
    if (hover_win && hover_win != w && hover_win->hover_button) {
        hover_win->hover_button = 0;
        wm_dirty(R(hover_win->x, hover_win->y, hover_win->cw, TITLE_H));
    }
    if (w && w->hover_button != b) {
        w->hover_button = b;
        wm_dirty(R(w->x, w->y, w->cw, TITLE_H));
    }
}

static void handle_mouse(int nx, int ny, int buttons, int wheel)
{
    nx = CLAMP(nx, 0, SW - 1);
    ny = CLAMP(ny, 0, SH - 1);
    bool moved = nx != wm_mx || ny != wm_my;
    if (moved) {
        wm_dirty(cursor_rect());
        wm_mx = nx;
        wm_my = ny;
        wm_dirty(cursor_rect());
    }
    int pressed = buttons & ~mbuttons;
    int released = mbuttons & ~buttons;
    mbuttons = buttons;

    if (pressed & BTN_LEFT) {
        uint64_t now = uptime_ms();
        if (now - last_click_t < 400 && ABS_DIFF(nx, last_click_x) < 5 && ABS_DIFF(ny, last_click_y) < 5)
            click_count++;
        else
            click_count = 1;
        last_click_t = now;
        last_click_x = nx;
        last_click_y = ny;
    }

    /* window move / resize in progress */
    if (drag_mode && drag_win) {
        if (moved) {
            if (drag_mode == 1) {
                wm_dirty_window(drag_win);
                if (drag_win->state == WS_MAXIMIZED) {
                    /* dragging a maximized window restores it under the cursor */
                    int rw = drag_win->restore.w;
                    drag_win->state = WS_NORMAL;
                    set_request(drag_win, rw, drag_win->restore.h);
                    drag_ox = MIN(drag_ox, rw / 2);
                }
                drag_win->x = wm_mx - drag_ox;
                drag_win->y = MAX(0, wm_my - drag_oy);
                drag_win->y = MIN(drag_win->y, SH - desktop_taskbar_h() - 8);
                wm_dirty_window(drag_win);
            } else {
                int nw = drag_w0 + ((drag_edges & 1) ? wm_mx - drag_ox : 0);
                int nh = drag_h0 + ((drag_edges & 2) ? wm_my - drag_oy : 0);
                set_request(drag_win, MIN(nw, SW), MIN(nh, SH));
            }
        }
        if (released & BTN_LEFT) {
            drag_mode = 0;
            drag_win = NULL;
            set_cursor(0);
        }
        return;
    }

    /* a client that got a button press keeps receiving the mouse */
    if (capture_win) {
        window_t *w = capture_win;
        if (moved) send_mouse(w, EV_MOUSE_MOVE, 0, 0);
        for (int b = 1; b <= 4; b <<= 1)
            if (released & b) send_mouse(w, EV_MOUSE_UP, b, 0);
        if (!mbuttons) capture_win = NULL;
        return;
    }

    if (desktop_mouse_overlay(wm_mx, wm_my, buttons, pressed, released, wheel)) {
        set_hover_button(NULL, 0);
        if (hover_win) {
            send_mouse(hover_win, EV_MOUSE_LEAVE, 0, 0);
            hover_win = NULL;
        }
        set_cursor(0);
        return;
    }

    window_t *w = window_at(wm_mx, wm_my);
    if (hover_win && hover_win != w) {
        set_hover_button(NULL, 0);
        send_mouse(hover_win, EV_MOUSE_LEAVE, 0, 0);
        hover_win = NULL;
    }
    if (!w) {
        set_cursor(0);
        if (pressed & BTN_LEFT) wm_focus_locked(NULL);
        desktop_mouse_background(wm_mx, wm_my, buttons, pressed, released, click_count);
        return;
    }
    hover_win = w;
    int edges;
    int hit = hit_test(w, wm_mx, wm_my, &edges);
    set_hover_button(w, hit == HIT_CLOSE ? 1 : hit == HIT_MAX ? 2 : hit == HIT_MIN ? 3 : 0);
    set_cursor(hit == HIT_RESIZE ? (edges == 3 ? 1 : edges == 1 ? 2 : 3) : 0);

    if (pressed) {
        if (w != wm_focus_win || w->next) {
            wm_raise(w);
            wm_focus_locked(w);
        }
    }
    switch (hit) {
    case HIT_CLIENT:
        if (moved) send_mouse(w, EV_MOUSE_MOVE, 0, 0);
        for (int b = 1; b <= 4; b <<= 1)
            if (pressed & b) { send_mouse(w, EV_MOUSE_DOWN, b, 0); capture_win = w; }
        for (int b = 1; b <= 4; b <<= 1)
            if (released & b) send_mouse(w, EV_MOUSE_UP, b, 0);
        if (wheel) send_mouse(w, EV_MOUSE_WHEEL, 0, wheel);
        break;
    case HIT_TITLE:
        if (pressed & BTN_LEFT) {
            if (click_count >= 2 && (w->flags & WF_RESIZABLE)) {
                wm_toggle_maximize(w);
                click_count = 0;
            } else {
                drag_mode = 1;
                drag_win = w;
                drag_ox = wm_mx - w->x;
                drag_oy = wm_my - w->y;
            }
        }
        break;
    case HIT_RESIZE:
        if (pressed & BTN_LEFT) {
            drag_mode = 2;
            drag_win = w;
            drag_edges = edges;
            drag_ox = wm_mx;
            drag_oy = wm_my;
            drag_w0 = w->cw;
            drag_h0 = w->ch;
        }
        break;
    case HIT_CLOSE:
        if (released & BTN_LEFT) wm_request_close(w);
        break;
    case HIT_MAX:
        if (released & BTN_LEFT) wm_toggle_maximize(w);
        break;
    case HIT_MIN:
        if (released & BTN_LEFT) wm_minimize(w);
        break;
    }
}

static void deliver_key(int key, uint32_t ch, bool pressed)
{
    window_t *w = wm_focus_win;
    if (!w || w->close_requested) return;
    struct gui_event ev = { 0 };
    ev.type = pressed ? EV_KEY_DOWN : EV_KEY_UP;
    ev.key = key;
    ev.ch = ch;
    ev.mods = wm_mods;
    wm_post_event(w, &ev);
}

static void handle_key(int key, bool pressed)
{
    int bit = 0;
    switch (key) {
    case KEY_LSHIFT: case KEY_RSHIFT: bit = MOD_SHIFT; break;
    case KEY_LCTRL: case KEY_RCTRL: bit = MOD_CTRL; break;
    case KEY_LALT: bit = MOD_ALT; break;
    case KEY_RALT: bit = keymap_altgr() ? MOD_ALTGR : MOD_ALT; break;
    case KEY_LMETA: case KEY_RMETA: bit = MOD_SUPER; break;
    }
    if (bit) {
        if (pressed) wm_mods |= bit;
        else wm_mods &= ~bit;
        if (key == KEY_LMETA || key == KEY_RMETA) {
            if (pressed) super_alone = true;
            else if (super_alone) { desktop_toggle_start(); super_alone = false; }
        }
        if (bit == MOD_ALT && !pressed && alt_tabbing) {
            alt_tabbing = false;
            desktop_alt_release();
        }
        deliver_key(key, 0, pressed);
        return;
    }
    if (pressed) super_alone = false;
    if (key == KEY_CAPSLOCK) {
        if (pressed) wm_mods ^= MOD_CAPS;
        return;
    }

    /* AltGr on German keyboards arrives as Ctrl+Alt on some machines */
    int mods = wm_mods;
    uint32_t ch = 0;
    if (pressed) {
        int tmods = mods;
        if (keymap_altgr() && (mods & MOD_CTRL) && (mods & MOD_ALT)) tmods |= MOD_ALTGR;
        ch = (mods & (MOD_CTRL | MOD_ALT)) && !(tmods & MOD_ALTGR) ? 0 : keymap_translate(key, tmods);
    }

    if (login_active()) {
        login_key(key, ch, mods, pressed);
        return;
    }
    if (pressed) {
        if ((mods & MOD_ALT) && key == KEY_TAB) {
            desktop_alt_tab(!alt_tabbing, (mods & MOD_SHIFT) != 0);
            alt_tabbing = true;
            return;
        }
        if ((mods & MOD_ALT) && key == KEY_F4) {
            if (wm_focus_win) wm_request_close(wm_focus_win);
            return;
        }
        if ((mods & MOD_CTRL) && key == KEY_ESC) { desktop_toggle_start(); return; }
        if ((mods & MOD_CTRL) && (mods & MOD_ALT) && key == KEY_T) { app_launch("terminal"); return; }
        if ((mods & MOD_CTRL) && (mods & MOD_ALT) && key == KEY_DELETE) { app_launch("sysmon"); return; }
        if ((mods & MOD_SUPER) && key == KEY_D) {
            for (window_t *w = wm_list; w; w = w->next)
                if (!(w->flags & WF_NO_TASKBAR)) wm_minimize(w);
            return;
        }
        if ((mods & MOD_SUPER) && key == KEY_E) { app_launch("files"); return; }
        if ((mods & MOD_SUPER) && key == KEY_L) { login_start(true); return; }
        if ((mods & MOD_SUPER) && key == KEY_SPACE) {
            keymap_next();
            conf_set(&sysconf, "keyboard", keymap_current());
            sys_save_conf();
            desktop_notify(tr("Keyboard layout"), keymap_long(), ICON_SETTINGS);
            return;
        }
        if ((mods & MOD_SUPER) && (key == KEY_LEFT || key == KEY_RIGHT) && wm_focus_win) {
            window_t *w = wm_focus_win;
            int half = SW / 2, avail = SH - desktop_taskbar_h();
            wm_dirty_window(w);
            if (w->state == WS_MAXIMIZED) w->state = WS_NORMAL;
            if (w->flags & WF_RESIZABLE) {
                w->restore = R(w->x, w->y, w->cw, w->ch);
                w->x = key == KEY_LEFT ? 0 : half;
                w->y = 0;
                set_request(w, half, avail - title_h(w));
            } else {
                w->x = key == KEY_LEFT ? MAX(0, (half - w->cw) / 2) : half + MAX(0, (half - w->cw) / 2);
                w->y = MAX(0, (avail - w->ch - title_h(w)) / 2);
            }
            wm_dirty_window(w);
            return;
        }
        if ((mods & MOD_SUPER) && key == KEY_UP && wm_focus_win) {
            if (wm_focus_win->state != WS_MAXIMIZED) wm_toggle_maximize(wm_focus_win);
            return;
        }
        if ((mods & MOD_SUPER) && key == KEY_DOWN && wm_focus_win) {
            if (wm_focus_win->state == WS_MAXIMIZED) wm_toggle_maximize(wm_focus_win);
            else wm_minimize(wm_focus_win);
            return;
        }
        if (key == KEY_SYSRQ || (key == KEY_F12 && (mods & MOD_CTRL))) {
            char path[128];
            mutex_unlock(&wm_mtx);
            bool ok = wm_save_screenshot(path, sizeof(path));
            mutex_lock(&wm_mtx);
            desktop_notify(ok ? "Screenshot saved" : "Screenshot failed", ok ? path : "", ICON_IMAGE);
            return;
        }
    }
    if (desktop_key(key, ch, mods, pressed)) return;
    deliver_key(key, ch, pressed);
}

static void process_input(void)
{
    struct raw_input ev;
    static int acc_x, acc_y;
    while (input_pop(&ev)) {
        switch (ev.type) {
        case RAW_KEY:
            handle_key(ev.key, ev.pressed);
            break;
        case RAW_MOUSE_REL: {
            int dx = ev.x, dy = ev.y;
            int speed = ABS_DIFF(dx, 0) + ABS_DIFF(dy, 0);
            if (speed > 6) { dx = dx * 3 / 2; dy = dy * 3 / 2; }
            acc_x = CLAMP(wm_mx + dx, 0, SW - 1);
            acc_y = CLAMP(wm_my + dy, 0, SH - 1);
            handle_mouse(acc_x, acc_y, ev.buttons, -ev.wheel);
            break;
        }
        case RAW_MOUSE_ABS:
            handle_mouse((int)((int64_t)ev.x * (SW - 1) / 65535), (int)((int64_t)ev.y * (SH - 1) / 65535),
                         ev.buttons, -ev.wheel);
            break;
        }
    }
}

/* ------------------------------------------------------------------------
 * screenshots (BMP, 32-bit)
 * ---------------------------------------------------------------------- */

bool wm_save_screenshot(char *path_out, size_t n)
{
    struct datetime dt;
    rtc_now(&dt);
    char dir[VFS_PATH_MAX];
    session_path("Pictures", dir, sizeof(dir));
    sys_mkdir_p(dir);
    snprintf(path_out, n, "%s/Screenshot %04d-%02d-%02d %02d.%02d.%02d.bmp", dir,
             dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    size_t img = (size_t)SW * SH * 4;
    size_t total = 54 + img;
    uint8_t *buf = kmalloc(total);
    if (!buf) return false;
    memset(buf, 0, 54);
    buf[0] = 'B'; buf[1] = 'M';
    *(uint32_t *)(buf + 2) = (uint32_t)total;
    *(uint32_t *)(buf + 10) = 54;
    *(uint32_t *)(buf + 14) = 40;
    *(int32_t *)(buf + 18) = SW;
    *(int32_t *)(buf + 22) = -SH;           /* top-down */
    *(uint16_t *)(buf + 26) = 1;
    *(uint16_t *)(buf + 28) = 32;
    *(uint32_t *)(buf + 34) = (uint32_t)img;
    mutex_lock(&wm_mtx);
    for (int y = 0; y < SH; y++)
        memcpy(buf + 54 + (size_t)y * SW * 4, screen->px + (size_t)y * screen->stride, (size_t)SW * 4);
    mutex_unlock(&wm_mtx);
    int r = vfs_write_file(path_out, buf, total);
    kfree(buf);
    return r == 0;
}

/* ------------------------------------------------------------------------
 * compositor thread
 * ---------------------------------------------------------------------- */

static int wm_thread(void *arg)
{
    UNUSED(arg);
    uint64_t last_compose = 0;
    for (;;) {
        mutex_lock(&wm_mtx);
        process_input();
        uint64_t now = uptime_ms();
        bool anim = run_animations(now);
        anim |= desktop_tick(now);

        if (ndirty && now - last_compose < (uint64_t)frame_ms) {
            /* one frame per refresh of the monitor; come back when the next one is due */
            mutex_unlock(&wm_mtx);
            sched_sleep((uint64_t)frame_ms - (now - last_compose));
            continue;
        }
        rect_t work[MAX_DIRTY];
        int nwork = ndirty;
        memcpy(work, dirty, sizeof(rect_t) * (size_t)nwork);
        ndirty = 0;
        for (int i = 0; i < nwork; i++) compose(work[i]);
        for (int i = 0; i < nwork; i++) flush(work[i]);
        mutex_unlock(&wm_mtx);

        if (nwork) {
            last_compose = now;
            fps_count++;
        }
        if (now - fps_t0 >= 1000) {
            fps_value = fps_count;
            fps_count = 0;
            fps_t0 = now;
        }

        spin_lock(&wake_lock);
        if (!wake_pending && !ndirty) {
            uint64_t timeout = anim ? (uint64_t)frame_ms : 250;
            sched_wait(input_chan(), &wake_lock, timeout);
        }
        wake_pending = 0;
        spin_unlock(&wake_lock);
    }
    return 0;
}


void wm_set_refresh(uint32_t refresh_mhz)
{
    if (refresh_mhz < 24000) refresh_mhz = 60000;
    int ms = (int)(1000000u / refresh_mhz);
    frame_ms = CLAMP(ms, 2, 40);
}

/* the display changed (a graphics driver took over, or another mode was chosen): new framebuffer, new size */
bool wm_set_framebuffer(void *newfb, int w, int h, int pitch, bool swap, void (*present)(int, int, int, int))
{
    surface_t *ns = surface_new(w, h);
    if (!ns) return false;
    mutex_lock(&wm_mtx);
    surface_t *old = screen;
    int old_w = SW, old_h = SH;
    screen = ns;
    fb = newfb;
    fb_stride = pitch / 4;
    fb_swap = swap;
    present_hook = present;
    SW = w;
    SH = h;
    surface_free(old);
    wm_mx = CLAMP(wm_mx, 0, w - 1);
    wm_my = CLAMP(wm_my, 0, h - 1);
    if (w != old_w || h != old_h) desktop_resize(w, h);              /* same size: the wallpaper is still right */
    for (window_t *win = wm_list; win; win = win->next) {
        if (win->state == WS_MAXIMIZED) {
            win->x = 0;
            win->y = 0;
            set_request(win, SW, SH - desktop_taskbar_h() - title_h(win));
        } else {
            win->x = CLAMP(win->x, 80 - win->cw, SW - 80);
            win->y = CLAMP(win->y, 0, MAX(0, SH - desktop_taskbar_h() - 40));
        }
    }
    ndirty = 0;
    wm_dirty(R(0, 0, SW, SH));
    mutex_unlock(&wm_mtx);
    wm_kick();
    klog("wm: display is now %dx%d", w, h);
    return true;
}

void wm_init(void)
{
    struct boot_framebuffer *bfb = &boot_info->fb;
    struct gpu_fb gfb;
    if (gpu_early_display(&gfb)) {                       /* a graphics driver already switched the display */
        SW = gfb.w;
        SH = gfb.h;
        fb = gfb.vaddr;
        fb_stride = gfb.pitch / 4;
        fb_swap = gfb.swap;
    } else {
        SW = (int)bfb->width;
        SH = (int)bfb->height;
        fb = P2V(bfb->phys);
        fb_stride = (int)(bfb->pitch / 4);
        fb_swap = bfb->format == FB_FORMAT_RGBX;
    }
    screen = surface_new(SW, SH);
    if (!screen) panic("wm: cannot allocate the screen buffer");
    theme_init();
    corner_mask_init();
    cursors_init();
    wm_mx = SW / 2;
    wm_my = SH / 2;
    desktop_init(SW, SH);
    wm_dirty(R(0, 0, SW, SH));
    bootcon_enable(false);
    thread_create_ex("compositor", wm_thread, NULL, 1, -1, NULL, 64 * 1024);
    wm_ready = true;
    klog("wm: %dx%d compositor running%s", SW, SH, fb_swap ? " (RGB swap)" : "");
    /* who is using this computer? a login screen, or straight in when there is only a password-less account */
    mutex_lock(&wm_mtx);
    if (users_need_login()) {
        login_start(false);
    } else {
        const char *name = conf_get(&sysconf, "autologin", "");
        if (!*name && users_count()) name = users_get(0)->name;
        if (*name && session_login(name, "")) desktop_session_started();
        else login_start(false);
    }
    mutex_unlock(&wm_mtx);
    wm_kick();
}
