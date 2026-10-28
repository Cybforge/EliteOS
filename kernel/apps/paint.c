/* Paint: brush, eraser, line, rectangle, ellipse, fill, colour picker, undo, BMP save/open. */
#include <wm.h>
#include <sys.h>
#include <mm.h>
#include <vfs.h>
#include <image.h>

enum { T_BRUSH, T_ERASER, T_LINE, T_RECT, T_ELLIPSE, T_FILL, T_PICK, T_COUNT };
static const char *tool_names[T_COUNT] = { "Brush", "Eraser", "Line", "Rect", "Ellipse", "Fill", "Picker" };

static const uint32_t swatches[] = {
    0x000000, 0xFFFFFF, 0x7F7F7F, 0xC3C3C3, 0xFF5C7A, 0xFF9A40, 0xFFD24F, 0x35D69A,
    0x2BD4E6, 0x5B7CFF, 0x7C5CFF, 0xB06BFF, 0xFF7AC6, 0x8B5A2B, 0x1E3A5F, 0x0F6B3E,
};

#define UNDO 12

struct paint {
    surface_t *canvas;
    surface_t *undo[UNDO];
    int nundo;
    int tool;
    color_t color;
    float size;
    float hue;
    bool drawing;
    int lx, ly;             /* last point (canvas coords) */
    int sx, sy;             /* shape start */
    int cx, cy;             /* canvas origin in window */
    int mx, my;
    bool filled;
    char path[VFS_PATH_MAX];
    char status[96];
    int bar;                /* 1 save, 2 open */
    char input[VFS_PATH_MAX];
};

static void snapshot(struct paint *p)
{
    if (p->nundo == UNDO) { surface_free(p->undo[0]); memmove(&p->undo[0], &p->undo[1], sizeof(p->undo[0]) * (UNDO - 1)); p->nundo--; }
    surface_t *s = surface_new(p->canvas->w, p->canvas->h);
    if (!s) return;
    gfx_blit(s, 0, 0, p->canvas, 0, 0, s->w, s->h);
    p->undo[p->nundo++] = s;
}

static void undo(struct paint *p)
{
    if (!p->nundo) return;
    surface_t *s = p->undo[--p->nundo];
    gfx_blit(p->canvas, 0, 0, s, 0, 0, s->w, s->h);
    surface_free(s);
}

static void stamp_line(struct paint *p, int x0, int y0, int x1, int y1, color_t c)
{
    surface_reset_clip(p->canvas);
    if (p->size <= 1.5f) gfx_line(p->canvas, (float)x0, (float)y0, (float)x1, (float)y1, c);
    else gfx_line_w(p->canvas, (float)x0 + 0.5f, (float)y0 + 0.5f, (float)x1 + 0.5f, (float)y1 + 0.5f, p->size, c);
}

static void flood(struct paint *p, int x, int y, color_t c)
{
    surface_t *s = p->canvas;
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    uint32_t target = s->px[y * s->stride + x];
    if (target == (c | 0xFF000000u)) return;
    int cap = s->w * s->h;
    int *stack = kmalloc(sizeof(int) * 2 * (size_t)cap / 4 + 64);
    if (!stack) return;
    int sp = 0, maxsp = cap / 2;
    stack[sp++] = x; stack[sp++] = y;
    while (sp) {
        int yy = stack[--sp], xx = stack[--sp];
        uint32_t *row = s->px + (size_t)yy * s->stride;
        int l = xx, r = xx;
        while (l > 0 && row[l - 1] == target) l--;
        while (r < s->w - 1 && row[r + 1] == target) r++;
        for (int i = l; i <= r; i++) row[i] = c | 0xFF000000u;
        for (int d = -1; d <= 1; d += 2) {
            int ny = yy + d;
            if (ny < 0 || ny >= s->h) continue;
            uint32_t *nr = s->px + (size_t)ny * s->stride;
            for (int i = l; i <= r; i++) {
                if (nr[i] == target && (i == l || nr[i - 1] != target) && sp + 2 < maxsp) {
                    stack[sp++] = i; stack[sp++] = ny;
                }
            }
        }
    }
    kfree(stack);
}

static void draw_shape(struct paint *p, surface_t *s, int x0, int y0, int x1, int y1, int ox, int oy)
{
    int ax = MIN(x0, x1), ay = MIN(y0, y1), bw = ABS(x1 - x0), bh = ABS(y1 - y0);
    color_t c = p->color;
    float w = MAX(1.0f, p->size);
    switch (p->tool) {
    case T_LINE:
        gfx_line_w(s, (float)(x0 + ox), (float)(y0 + oy), (float)(x1 + ox), (float)(y1 + oy), w, c);
        break;
    case T_RECT:
        if (p->filled) gfx_fill(s, ax + ox, ay + oy, bw, bh, c);
        else {
            gfx_line_w(s, (float)(ax + ox), (float)(ay + oy), (float)(ax + bw + ox), (float)(ay + oy), w, c);
            gfx_line_w(s, (float)(ax + ox), (float)(ay + bh + oy), (float)(ax + bw + ox), (float)(ay + bh + oy), w, c);
            gfx_line_w(s, (float)(ax + ox), (float)(ay + oy), (float)(ax + ox), (float)(ay + bh + oy), w, c);
            gfx_line_w(s, (float)(ax + bw + ox), (float)(ay + oy), (float)(ax + bw + ox), (float)(ay + bh + oy), w, c);
        }
        break;
    case T_ELLIPSE: {
        float cx = (float)(ax + ox) + (float)bw / 2, cy = (float)(ay + oy) + (float)bh / 2;
        float rx = (float)bw / 2, ry = (float)bh / 2;
        if (rx < 1 || ry < 1) break;
        int steps = 96;
        float px = cx + rx, py = cy;
        for (int i = 1; i <= steps; i++) {
            float a = (float)i / (float)steps * 2 * (float)K_PI;
            float nx = cx + rx * (float)k_cos(a), ny = cy + ry * (float)k_sin(a);
            if (p->filled) gfx_triangle(s, cx, cy, px, py, nx, ny, c);
            gfx_line_w(s, px, py, nx, ny, w, c);
            px = nx;
            py = ny;
        }
        break;
    }
    }
}

/* ------------------------------------------------------------------------
 * BMP load/save (24/32-bit)
 * ---------------------------------------------------------------------- */

bool bmp_save(const char *path, surface_t *s)
{
    size_t img = (size_t)s->w * s->h * 4;
    uint8_t *b = kzalloc(54 + img);
    if (!b) return false;
    b[0] = 'B'; b[1] = 'M';
    *(uint32_t *)(b + 2) = (uint32_t)(54 + img);
    *(uint32_t *)(b + 10) = 54;
    *(uint32_t *)(b + 14) = 40;
    *(int32_t *)(b + 18) = s->w;
    *(int32_t *)(b + 22) = -s->h;
    *(uint16_t *)(b + 26) = 1;
    *(uint16_t *)(b + 28) = 32;
    *(uint32_t *)(b + 34) = (uint32_t)img;
    for (int y = 0; y < s->h; y++) memcpy(b + 54 + (size_t)y * s->w * 4, s->px + (size_t)y * s->stride, (size_t)s->w * 4);
    int r = vfs_write_file(path, b, 54 + img);
    kfree(b);
    return r == 0;
}

/* opens anything the image decoders understand (PNG, JPEG, BMP); transparent
 * pixels land on white, since the canvas has no alpha */
surface_t *bmp_load(const char *path)
{
    surface_t *s = image_load(path);
    if (!s) return NULL;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            color_t c = s->px[(size_t)y * s->stride + x];
            unsigned a = C_A(c);
            if (a == 255) continue;
            s->px[(size_t)y * s->stride + x] = RGB((C_R(c) * a + 255 * (255 - a)) / 255, (C_G(c) * a + 255 * (255 - a)) / 255,
                                                   (C_B(c) * a + 255 * (255 - a)) / 255);
        }
    return s;
}

/* ------------------------------------------------------------------------ */

#define TOOLBAR_W 190

static void paint_paint(app_t *a, surface_t *s)
{
    struct paint *p = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, HEX(0x1A1D30));
    /* side toolbar */
    gfx_fill(s, 0, 0, TOOLBAR_W, H, theme.panel);
    gfx_fill(s, TOOLBAR_W, 0, 1, H, ALPHA(0xFFFFFF, 14));
    gfx_text(s, font_ui_md, 16, 14, "Tools", theme.text_faint);
    for (int i = 0; i < T_COUNT; i++) {
        rect_t r = R(12 + (i % 2) * 84, 38 + (i / 2) * 38, 80, 34);
        if (ui_button(u, r, tool_names[i], p->tool == i ? BTN_PRIMARY : BTN_SUBTLE)) p->tool = i;
    }
    int y = 38 + 4 * 38 + 8;
    ui_checkbox(u, R(14, y, 160, 26), "Filled shapes", &p->filled);
    y += 38;
    gfx_text(s, font_ui_md, 16, y, "Size", theme.text_faint);
    char sz[16];
    snprintf(sz, sizeof(sz), "%d px", (int)p->size);
    gfx_text_right(s, font_ui, TOOLBAR_W - 16, y, sz, theme.text_dim);
    ui_slider(u, R(8, y + 18, TOOLBAR_W - 16, 26), &p->size, 1, 40);
    y += 56;
    gfx_text(s, font_ui_md, 16, y, "Colour", theme.text_faint);
    y += 22;
    for (int i = 0; i < 16; i++) {
        rect_t r = R(14 + (i % 4) * 42, y + (i / 4) * 34, 36, 28);
        bool sel = (p->color & 0xFFFFFF) == swatches[i];
        gfx_round_rect(s, r.x, r.y, r.w, r.h, 6, HEX(swatches[i]));
        gfx_round_outline(s, r.x, r.y, r.w, r.h, 6, sel ? 0xFFFFFFFFu : ALPHA(0xFFFFFF, 40));
        if (ui_hover(u, r) && u->mpressed) p->color = HEX(swatches[i]);
    }
    y += 4 * 34 + 8;
    /* hue strip */
    rect_t hs = R(14, y, TOOLBAR_W - 28, 18);
    for (int x = 0; x < hs.w; x++) gfx_fill(s, hs.x + x, hs.y, 1, hs.h, color_hsv((float)x * 360.0f / (float)hs.w, 0.85f, 1.0f));
    if (ui_hover(u, hs) && u->mdown) {
        p->hue = (float)(u->mx - hs.x) * 360.0f / (float)hs.w;
        p->color = color_hsv(p->hue, 0.85f, 1.0f);
    }
    y += 30;
    gfx_round_rect(s, 14, y, TOOLBAR_W - 28, 30, 8, p->color);
    gfx_round_outline(s, 14, y, TOOLBAR_W - 28, 30, 8, ALPHA(0xFFFFFF, 60));
    y += 44;
    if (ui_button(u, R(12, y, 80, 32), "Undo", BTN_NORMAL)) undo(p);
    if (ui_button(u, R(96, y, 80, 32), "Clear", BTN_NORMAL)) { snapshot(p); gfx_clear(p->canvas, 0xFFFFFFFFu); }
    y += 38;
    if (ui_button(u, R(12, y, 80, 32), "Open", BTN_NORMAL)) { p->bar = 2; strlcpy(p->input, p->path, sizeof(p->input)); }
    if (ui_button(u, R(96, y, 80, 32), "Save", BTN_PRIMARY)) { p->bar = 1; strlcpy(p->input, p->path, sizeof(p->input)); }

    /* canvas */
    int avail_w = W - TOOLBAR_W - 32, avail_h = H - 32 - (p->bar ? 50 : 0);
    p->cx = TOOLBAR_W + 16 + MAX(0, (avail_w - p->canvas->w) / 2);
    p->cy = 16 + MAX(0, (avail_h - p->canvas->h) / 2);
    gfx_shadow(s, p->cx, p->cy + 4, MIN(p->canvas->w, avail_w), MIN(p->canvas->h, avail_h), 2, 14, 120);
    surface_t sub = *s;
    sub.clip = rect_intersect(s->clip, R(TOOLBAR_W + 16, 16, avail_w, avail_h));
    gfx_blit(&sub, p->cx, p->cy, p->canvas, 0, 0, p->canvas->w, p->canvas->h);
    /* live shape preview */
    if (p->drawing && (p->tool == T_LINE || p->tool == T_RECT || p->tool == T_ELLIPSE))
        draw_shape(p, &sub, p->sx, p->sy, p->mx - p->cx, p->my - p->cy, p->cx, p->cy);
    /* brush outline */
    rect_t cr = R(p->cx, p->cy, p->canvas->w, p->canvas->h);
    if (rect_has(cr, p->mx, p->my) && (p->tool == T_BRUSH || p->tool == T_ERASER) && p->size > 3)
        gfx_ring(&sub, (float)p->mx + 0.5f, (float)p->my + 0.5f, p->size / 2, 1, ALPHA(0x7F7F7F, 200));

    if (p->bar) {
        int by = H - 50;
        gfx_fill(s, TOOLBAR_W + 1, by, W - TOOLBAR_W, 50, theme.panel);
        gfx_text(s, font_ui_md, TOOLBAR_W + 16, by + 16, p->bar == 1 ? "Save as" : "Open", theme.text_dim);
        rect_t ir = R(TOOLBAR_W + 90, by + 8, W - TOOLBAR_W - 300, 34);
        u->focus = ui_id(ir);
        bool enter = ui_textbox(u, ir, p->input, sizeof(p->input), NULL);
        if (ui_button(u, R(W - 200, by + 8, 90, 34), "Cancel", BTN_NORMAL)) p->bar = 0;
        if (ui_button(u, R(W - 104, by + 8, 90, 34), p->bar == 1 ? "Save" : "Open", BTN_PRIMARY) || enter) {
            if (p->bar == 1) {
                if (bmp_save(p->input, p->canvas)) { strlcpy(p->path, p->input, sizeof(p->path)); wm_notify("Picture saved", p->path, ICON_PAINT); }
            } else {
                surface_t *img = bmp_load(p->input);
                if (img) {
                    snapshot(p);
                    gfx_clear(p->canvas, 0xFFFFFFFFu);
                    gfx_blit(p->canvas, 0, 0, img, 0, 0, img->w, img->h);
                    surface_free(img);
                    strlcpy(p->path, p->input, sizeof(p->path));
                }
            }
            p->bar = 0;
        }
    }
}

static int paint_event(app_t *a, struct gui_event *ev)
{
    struct paint *p = a->data;
    int x = ev->x - p->cx, y = ev->y - p->cy;
    color_t c = p->tool == T_ERASER ? 0xFFFFFFFFu : p->color;
    switch (ev->type) {
    case EV_MOUSE_MOVE:
        p->mx = ev->x;
        p->my = ev->y;
        if (p->drawing && (p->tool == T_BRUSH || p->tool == T_ERASER)) {
            stamp_line(p, p->lx, p->ly, x, y, c);
            p->lx = x;
            p->ly = y;
        }
        return 1;
    case EV_MOUSE_DOWN:
        if (ev->button != BTN_LEFT || ev->x < TOOLBAR_W || p->bar) return 1;
        if (x < 0 || y < 0 || x >= p->canvas->w || y >= p->canvas->h) return 1;
        if (p->tool == T_PICK) { p->color = p->canvas->px[y * p->canvas->stride + x] | 0xFF000000u; p->tool = T_BRUSH; return 1; }
        snapshot(p);
        if (p->tool == T_FILL) { flood(p, x, y, p->color); return 1; }
        p->drawing = true;
        p->lx = p->sx = x;
        p->ly = p->sy = y;
        if (p->tool == T_BRUSH || p->tool == T_ERASER) stamp_line(p, x, y, x, y, c);
        return 1;
    case EV_MOUSE_UP:
        if (p->drawing && (p->tool == T_LINE || p->tool == T_RECT || p->tool == T_ELLIPSE)) {
            surface_reset_clip(p->canvas);
            draw_shape(p, p->canvas, p->sx, p->sy, x, y, 0, 0);
        }
        p->drawing = false;
        return 1;
    case EV_KEY_DOWN:
        if ((ev->mods & MOD_CTRL) && ev->key == KEY_Z) undo(p);
        if ((ev->mods & MOD_CTRL) && ev->key == KEY_S) { p->bar = 1; strlcpy(p->input, p->path, sizeof(p->input)); }
        if (ev->key == KEY_ESC) p->bar = 0;
        return 1;
    }
    return 1;
}

int paint_main(void *arg)
{
    struct paint *p = kzalloc(sizeof(*p));
    p->canvas = surface_new(760, 520);
    gfx_clear(p->canvas, 0xFFFFFFFFu);
    p->color = HEX(0x7C5CFF);
    p->size = 6;
    session_path("Pictures/drawing.bmp", p->path, sizeof(p->path));
    if (arg) {
        surface_t *img = bmp_load(arg);
        if (img) {
            gfx_blit(p->canvas, 0, 0, img, 0, 0, img->w, img->h);
            surface_free(img);
            strlcpy(p->path, arg, sizeof(p->path));
        }
        kfree(arg);
    }
    app_t a = { 0 };
    a.data = p;
    a.win = wm_create("Paint", 1000, 650, WF_RESIZABLE);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_PAINT);
    wm_set_min_size(a.win, 640, 640);
    a.on_paint = paint_paint;
    a.on_event = paint_event;
    int r = app_run(&a);
    for (int i = 0; i < p->nundo; i++) surface_free(p->undo[i]);
    surface_free(p->canvas);
    kfree(p);
    return r;
}
