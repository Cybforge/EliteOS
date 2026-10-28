/*
 * Mandelbrot explorer. Rows are rendered in parallel on all cores with
 * smooth (continuous) colouring; click to zoom in, right-click to zoom out,
 * drag to pan, wheel to zoom at the cursor.
 */
#include <wm.h>
#include <mm.h>
#include <cpu.h>
#include <parallel.h>

struct mandel {
    double cx, cy, scale;       /* centre and units per pixel */
    int maxit;
    int palette;
    uint32_t lut[1024];
    surface_t *img;
    bool need;
    uint64_t ms;
    int drag_x, drag_y;
    bool dragging, moved;
    int W, H;
};

static color_t gradient(const float *pos, const uint32_t *cols, int n, float t)
{
    for (int i = 0; i < n - 1; i++) {
        if (t <= pos[i + 1]) {
            float f = (t - pos[i]) / (pos[i + 1] - pos[i]);
            return color_lerp(HEX(cols[i]), HEX(cols[i + 1]), (int)(f * 256.0f));
        }
    }
    return HEX(cols[n - 1]);
}

static void build_palette(struct mandel *m)
{
    static const float p0[] = { 0.0f, 0.16f, 0.42f, 0.6425f, 0.8575f, 1.0f };
    static const uint32_t c0[] = { 0x000764, 0x206BCB, 0xEDFFFF, 0xFFAA00, 0x000200, 0x000764 };
    static const float p1[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    static const uint32_t c1[] = { 0x0B0826, 0x7C5CFF, 0x00D4FF, 0xE8FBFF, 0x0B0826 };
    static const uint32_t c2[] = { 0x100000, 0xB3200E, 0xFF9B21, 0xFFF6C2, 0x100000 };
    for (int i = 0; i < 1024; i++) {
        float t = (float)i / 1024.0f;
        if (m->palette == 0) m->lut[i] = gradient(p0, c0, 6, t);
        else if (m->palette == 1) m->lut[i] = gradient(p1, c1, 5, t);
        else m->lut[i] = gradient(p1, c2, 5, t);
    }
}

struct row_ctx {
    struct mandel *m;
};

static void render_row(int y, void *arg)
{
    struct mandel *m = ((struct row_ctx *)arg)->m;
    uint32_t *line = m->img->px + (size_t)y * m->img->stride;
    double ci = m->cy + ((double)y - m->H / 2.0) * m->scale;
    int maxit = m->maxit;
    for (int x = 0; x < m->W; x++) {
        double cr = m->cx + ((double)x - m->W / 2.0) * m->scale;
        /* skip the main cardioid and period-2 bulb */
        double q = (cr - 0.25) * (cr - 0.25) + ci * ci;
        if (q * (q + (cr - 0.25)) <= 0.25 * ci * ci || (cr + 1) * (cr + 1) + ci * ci <= 0.0625) {
            line[x] = 0xFF000000u;
            continue;
        }
        double zr = 0, zi = 0, zr2 = 0, zi2 = 0;
        int it = 0;
        while (it < maxit && zr2 + zi2 <= 256.0) {
            zi = 2 * zr * zi + ci;
            zr = zr2 - zi2 + cr;
            zr2 = zr * zr;
            zi2 = zi * zi;
            it++;
        }
        if (it >= maxit) { line[x] = 0xFF000000u; continue; }
        /* smooth iteration count */
        double log_zn = k_log(zr2 + zi2) / 2;
        double nu = k_log(log_zn / 0.6931471805599453) / 0.6931471805599453;
        double smooth = (double)it + 1 - nu;
        int idx = (int)(k_sqrt(smooth) * 44.0) & 1023;
        line[x] = m->lut[idx];
    }
}

static void render(struct mandel *m, surface_t *s)
{
    if (!m->img || m->img->w != s->w || m->img->h != s->h) {
        surface_free(m->img);
        m->img = surface_new(s->w, s->h);
        m->need = true;
    }
    m->W = s->w;
    m->H = s->h;
    if (m->need) {
        uint64_t t0 = uptime_ms();
        struct row_ctx c = { m };
        parallel_for(m->H, render_row, &c);
        m->ms = uptime_ms() - t0;
        m->need = false;
    }
    gfx_blit(s, 0, 0, m->img, 0, 0, m->W, m->H);
}

static void md_paint(app_t *a, surface_t *s)
{
    struct mandel *m = a->data;
    render(m, s);
    char l[128];
    gfx_round_rect(s, 12, 12, 300, 70, 10, ALPHA(0x000000, 130));
    snprintf(l, sizeof(l), "Rendered in %lu ms on %d cores", m->ms, parallel_workers());
    gfx_text(s, font_ui_bold, 24, 20, l, 0xFFFFFFFFu);
    double zoom = 3.0 / (m->scale * (double)m->W);
    snprintf(l, sizeof(l), "zoom %.1fx  ·  %d iterations", zoom, m->maxit);
    gfx_text(s, font_ui, 24, 42, l, HEX(0xC8CCF0));
    snprintf(l, sizeof(l), "%.8f %+.8fi", m->cx, -m->cy);
    gfx_text(s, font_ui, 24, 60, l, HEX(0x8A90B8));
    gfx_text_right(s, font_ui, s->w - 14, s->h - 24,
                   "click: zoom in  ·  right click: out  ·  drag: pan  ·  +/-: iterations  ·  P: palette  ·  R: reset",
                   ALPHA(0xFFFFFF, 170));
}

static void zoom_at(struct mandel *m, int x, int y, double f)
{
    double px = m->cx + ((double)x - m->W / 2.0) * m->scale;
    double py = m->cy + ((double)y - m->H / 2.0) * m->scale;
    m->scale *= f;
    m->cx = px - ((double)x - m->W / 2.0) * m->scale;
    m->cy = py - ((double)y - m->H / 2.0) * m->scale;
    m->need = true;
}

static void reset(struct mandel *m, int W)
{
    m->cx = -0.6;
    m->cy = 0;
    m->scale = 3.4 / (double)MAX(W, 1);
    m->maxit = 256;
    m->need = true;
}

static int md_event(app_t *a, struct gui_event *ev)
{
    struct mandel *m = a->data;
    switch (ev->type) {
    case EV_MOUSE_DOWN:
        if (ev->button == BTN_LEFT) { m->dragging = true; m->moved = false; m->drag_x = ev->x; m->drag_y = ev->y; }
        if (ev->button == BTN_RIGHT) zoom_at(m, ev->x, ev->y, 2.0);
        return 1;
    case EV_MOUSE_MOVE:
        if (m->dragging && (ABS(ev->x - m->drag_x) > 3 || ABS(ev->y - m->drag_y) > 3 || m->moved)) {
            m->cx -= (double)(ev->x - m->drag_x) * m->scale;
            m->cy -= (double)(ev->y - m->drag_y) * m->scale;
            m->drag_x = ev->x;
            m->drag_y = ev->y;
            m->moved = true;
            m->need = true;
            return 1;
        }
        return 0;
    case EV_MOUSE_UP:
        if (ev->button == BTN_LEFT) {
            if (m->dragging && !m->moved) {
                zoom_at(m, ev->x, ev->y, 0.4);
                if (m->maxit < 4000) m->maxit += 48;
            }
            m->dragging = false;
        }
        return 1;
    case EV_MOUSE_WHEEL:
        zoom_at(m, ev->x, ev->y, ev->wheel > 0 ? 0.7 : 1.0 / 0.7);
        return 1;
    case EV_KEY_DOWN:
        if (ev->ch == '+' || ev->key == KEY_KPPLUS) { m->maxit = MIN(8000, m->maxit * 2); m->need = true; }
        if (ev->ch == '-' || ev->key == KEY_KPMINUS) { m->maxit = MAX(32, m->maxit / 2); m->need = true; }
        if (ev->key == KEY_P) { m->palette = (m->palette + 1) % 3; build_palette(m); m->need = true; }
        if (ev->key == KEY_R) reset(m, m->W);
        return 1;
    case EV_RESIZE:
        m->need = true;
        return 1;
    }
    return 0;
}

int mandel_main(void *arg)
{
    UNUSED(arg);
    struct mandel *m = kzalloc(sizeof(*m));
    build_palette(m);
    reset(m, 780);
    app_t a = { 0 };
    a.data = m;
    a.win = wm_create("Mandelbrot", 780, 520, WF_RESIZABLE);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_FRACTAL);
    a.on_paint = md_paint;
    a.on_event = md_event;
    int r = app_run(&a);
    surface_free(m->img);
    kfree(m);
    return r;
}
