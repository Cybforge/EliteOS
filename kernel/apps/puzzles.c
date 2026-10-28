/* Minesweeper and 2048. */
#include <wm.h>
#include <audio.h>
#include <input.h>
#include <mm.h>
#include <dev.h>

/* ========================================================================
 * Minesweeper
 * ====================================================================== */

#define MW_MAX 30
#define MH_MAX 16

struct mcell { bool mine, open, flag; uint8_t n; };

struct mines {
    struct mcell c[MH_MAX][MW_MAX];
    int w, h, count, level;
    bool placed, over, won;
    int opened, flags;
    uint64_t t0, t_end;
    int boom_x, boom_y;
};

static const struct { const char *name; int w, h, m; } mlevels[3] = {
    { "Beginner", 9, 9, 10 }, { "Intermediate", 16, 16, 40 }, { "Expert", 30, 16, 99 },
};

static void mines_reset(struct mines *g, int level)
{
    memset(g, 0, sizeof(*g));
    g->level = level;
    g->w = mlevels[level].w;
    g->h = mlevels[level].h;
    g->count = mlevels[level].m;
    g->boom_x = -1;
}

static void mines_place(struct mines *g, int sx, int sy)
{
    ksrand((uint32_t)uptime_ms() * 2654435761u);
    int placed = 0;
    while (placed < g->count) {
        int x = (int)(krand() % (unsigned)g->w), y = (int)(krand() % (unsigned)g->h);
        if (g->c[y][x].mine || (ABS(x - sx) <= 1 && ABS(y - sy) <= 1)) continue;    /* first click is always safe */
        g->c[y][x].mine = true;
        placed++;
    }
    for (int y = 0; y < g->h; y++)
        for (int x = 0; x < g->w; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, yy = y + dy;
                    if (xx >= 0 && yy >= 0 && xx < g->w && yy < g->h && g->c[yy][xx].mine) n++;
                }
            g->c[y][x].n = (uint8_t)n;
        }
    g->placed = true;
    g->t0 = uptime_ms();
}

static void mines_open(struct mines *g, int x, int y)
{
    /* iterative flood fill over empty cells */
    static int stack[MW_MAX * MH_MAX * 2][2];
    int sp = 0;
    stack[sp][0] = x; stack[sp][1] = y; sp++;
    while (sp) {
        sp--;
        int cx = stack[sp][0], cy = stack[sp][1];
        struct mcell *c = &g->c[cy][cx];
        if (c->open || c->flag) continue;
        c->open = true;
        g->opened++;
        if (c->mine) {
            g->over = true;
            g->boom_x = cx;
            g->boom_y = cy;
            g->t_end = uptime_ms();
            audio_sound(SND_ERROR);
            return;
        }
        if (c->n) continue;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int xx = cx + dx, yy = cy + dy;
                if (xx >= 0 && yy >= 0 && xx < g->w && yy < g->h && !g->c[yy][xx].open && sp < MW_MAX * MH_MAX * 2) {
                    stack[sp][0] = xx; stack[sp][1] = yy; sp++;
                }
            }
    }
    if (g->opened == g->w * g->h - g->count && !g->over) {
        g->over = g->won = true;
        g->t_end = uptime_ms();
        audio_sound(SND_SUCCESS);
    }
}

static void mines_click(struct mines *g, int x, int y, bool right)
{
    if (g->over) return;
    struct mcell *c = &g->c[y][x];
    if (right) {
        if (!c->open) {
            c->flag = !c->flag;
            g->flags += c->flag ? 1 : -1;
            audio_sound(SND_CLICK);
        }
        return;
    }
    if (!g->placed) mines_place(g, x, y);
    if (c->flag) return;
    if (c->open && c->n) {
        /* chord: open all neighbours when enough flags are set */
        int f = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int xx = x + dx, yy = y + dy;
                if (xx >= 0 && yy >= 0 && xx < g->w && yy < g->h && g->c[yy][xx].flag) f++;
            }
        if (f != c->n) return;
        for (int dy = -1; dy <= 1 && !g->over; dy++)
            for (int dx = -1; dx <= 1 && !g->over; dx++) {
                int xx = x + dx, yy = y + dy;
                if (xx >= 0 && yy >= 0 && xx < g->w && yy < g->h) mines_open(g, xx, yy);
            }
        return;
    }
    mines_open(g, x, y);
}

static void draw_mine(surface_t *s, float cx, float cy, float r, color_t c)
{
    for (int k = 0; k < 4; k++) {
        float a = (float)k * 3.14159265f / 4.0f;
        float dx = (float)k_cos(a) * r * 1.35f, dy = (float)k_sin(a) * r * 1.35f;
        gfx_line_w(s, cx - dx, cy - dy, cx + dx, cy + dy, MAX(1.5f, r * 0.28f), c);
    }
    gfx_circle(s, cx, cy, r, c);
    gfx_circle(s, cx - r * 0.3f, cy - r * 0.3f, r * 0.25f, 0xFFFFFFFFu);
}

static void mines_paint(app_t *a, surface_t *s)
{
    struct mines *g = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, theme.bg);
    /* header */
    gfx_fill(s, 0, 0, W, 56, theme.panel);
    for (int i = 0; i < 3; i++) {
        rect_t r = R(14 + i * 120, 12, 112, 32);
        if (ui_list_item(u, r, g->level == i)) mines_reset(g, i);
        gfx_text_center(s, font_ui_md, r, mlevels[i].name, g->level == i ? theme.text : theme.text_dim);
    }
    char b[48];
    snprintf(b, sizeof(b), "%d", g->count - g->flags);
    int rx = W - 16;
    uint64_t secs = g->placed ? ((g->over ? g->t_end : uptime_ms()) - g->t0) / 1000 : 0;
    char tb[16];
    snprintf(tb, sizeof(tb), "%lu s", secs);
    int tw = font_text_width(font_ui_bold, tb);
    gfx_text(s, font_ui_bold, rx - tw, 19, tb, theme.text);
    rx -= tw + 24;
    int bw = font_text_width(font_ui_bold, b);
    gfx_text(s, font_ui_bold, rx - bw, 19, b, theme.warning);
    draw_mine(s, (float)(rx - bw - 16), 28, 6, theme.warning);

    /* field */
    int cs = MIN((W - 32) / g->w, (H - 56 - 60) / g->h);
    cs = CLAMP(cs, 18, 40);
    int fx = (W - cs * g->w) / 2, fy = 56 + 16;
    static const uint32_t ncol[9] = { 0, 0x60A5FA, 0x4ADE80, 0xF87171, 0xC084FC, 0xFB923C, 0x2DD4BF, 0xE5E7EB, 0x9CA3AF };
    for (int y = 0; y < g->h; y++)
        for (int x = 0; x < g->w; x++) {
            struct mcell *c = &g->c[y][x];
            int px = fx + x * cs, py = fy + y * cs;
            rect_t r = R(px, py, cs, cs);
            bool show_mine = c->mine && g->over && !g->won;
            if (c->open || show_mine) {
                bool boom = x == g->boom_x && y == g->boom_y;
                gfx_round_rect(s, px + 1, py + 1, cs - 2, cs - 2, 4, boom ? HEX(0xB91C1C) : HEX(0x1E2236));
                if (c->mine) draw_mine(s, (float)px + cs / 2.0f, (float)py + cs / 2.0f, cs * 0.22f, boom ? 0xFFFFFFFFu : HEX(0xE5E7EB));
                else if (c->n) {
                    char nb[2] = { (char)('0' + c->n), 0 };
                    gfx_text_center(s, font_bold_lg, r, nb, HEX(ncol[c->n]));
                }
            } else {
                bool hov = ui_hover(u, r) && !g->over;
                gfx_round_rect_grad(s, px + 1, py + 1, cs - 2, cs - 2, 4, hov ? HEX(0x5B6390) : HEX(0x454C72),
                                    hov ? HEX(0x434A73) : HEX(0x363C5E));
                if (c->flag) {
                    bool wrong = g->over && !c->mine;
                    float cx = (float)px + cs * 0.42f, top = (float)py + cs * 0.22f;
                    gfx_line_w(s, cx, top, cx, (float)py + cs * 0.78f, MAX(1.5f, cs * 0.06f), HEX(0xE5E7EB));
                    gfx_triangle(s, cx, top, cx + cs * 0.34f, top + cs * 0.14f, cx, top + cs * 0.28f,
                                 wrong ? HEX(0x9CA3AF) : HEX(0xF43F5E));
                }
            }
            if (ui_hover(u, r) && (u->mreleased || u->rclicked)) mines_click(g, x, y, u->rclicked);
        }
    int by = fy + g->h * cs + 14;
    if (g->over) {
        const char *msg = g->won ? "Cleared! Well done." : "Boom. Try again?";
        gfx_text(s, font_ui_bold, fx, by + 8, msg, g->won ? theme.success : theme.danger);
    } else {
        gfx_text(s, font_ui, fx, by + 8, "Left click opens, right click flags, click a number to open around it", theme.text_faint);
    }
    if (ui_button(u, R(fx + cs * g->w - 110, by, 110, 34), "New game", BTN_PRIMARY)) mines_reset(g, g->level);
}

static int mines_event(app_t *a, struct gui_event *ev)
{
    struct mines *g = a->data;
    if (ev->type == EV_KEY_DOWN && ev->key == KEY_F2) { mines_reset(g, g->level); return 1; }
    return ev->type == EV_TIMER && g->placed && !g->over;
}

int mines_main(void *arg)
{
    UNUSED(arg);
    struct mines *g = kzalloc(sizeof(*g));
    mines_reset(g, 1);
    app_t a = { 0 };
    a.data = g;
    a.win = wm_create("Minesweeper", 680, 700, WF_RESIZABLE);
    if (!a.win) { kfree(g); return 1; }
    wm_set_icon(a.win, ICON_MINES);
    wm_set_min_size(a.win, 420, 400);
    wm_set_timer(a.win, 500);
    a.on_paint = mines_paint;
    a.on_event = mines_event;
    int r = app_run(&a);
    kfree(g);
    return r;
}

/* ========================================================================
 * 2048
 * ====================================================================== */

struct mover { int v, fr, fc, tr, tc; };

struct g2048 {
    int grid[4][4];
    int score, best;
    bool over, won, keep_going;
    struct mover mv[16];
    int nmv;
    uint64_t anim_t0;
    int new_r, new_c;
    bool merged[4][4];
    int drag_x, drag_y;
    bool dragging;
};

static void add_tile(struct g2048 *g)
{
    int free_cells[16][2], n = 0;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (!g->grid[r][c]) { free_cells[n][0] = r; free_cells[n][1] = c; n++; }
    if (!n) return;
    int k = (int)(krand() % (unsigned)n);
    g->grid[free_cells[k][0]][free_cells[k][1]] = krand() % 10 ? 2 : 4;
    g->new_r = free_cells[k][0];
    g->new_c = free_cells[k][1];
}

static void g2048_reset(struct g2048 *g)
{
    int best = g->best;
    memset(g, 0, sizeof(*g));
    g->best = best;
    ksrand((uint32_t)uptime_ms() * 2246822519u);
    add_tile(g);
    add_tile(g);
    g->anim_t0 = uptime_ms();
}

static bool can_move(struct g2048 *g)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            if (!g->grid[r][c]) return true;
            if (c < 3 && g->grid[r][c] == g->grid[r][c + 1]) return true;
            if (r < 3 && g->grid[r][c] == g->grid[r + 1][c]) return true;
        }
    return false;
}

/* dir: 0 left, 1 right, 2 up, 3 down */
static void g2048_move(struct g2048 *g, int dir)
{
    if (g->over || (g->won && !g->keep_going)) return;
    int ng[4][4] = { { 0 } };
    bool moved = false;
    g->nmv = 0;
    memset(g->merged, 0, sizeof(g->merged));
    int gained = 0;
    for (int line = 0; line < 4; line++) {
        /* cells of this line, from the edge the tiles slide towards */
        int cells[4][2];
        for (int i = 0; i < 4; i++) {
            int k = dir == 1 || dir == 3 ? 3 - i : i;
            cells[i][0] = dir <= 1 ? line : k;
            cells[i][1] = dir <= 1 ? k : line;
        }
        int out = 0, last = -1;
        for (int i = 0; i < 4; i++) {
            int r = cells[i][0], c = cells[i][1];
            int v = g->grid[r][c];
            if (!v) continue;
            int tr, tc;
            if (last >= 0 && ng[cells[last][0]][cells[last][1]] == v && !g->merged[cells[last][0]][cells[last][1]]) {
                tr = cells[last][0];
                tc = cells[last][1];
                ng[tr][tc] = v * 2;
                g->merged[tr][tc] = true;
                gained += v * 2;
            } else {
                tr = cells[out][0];
                tc = cells[out][1];
                ng[tr][tc] = v;
                last = out;
                out++;
            }
            if (tr != r || tc != c) moved = true;
            g->mv[g->nmv++] = (struct mover){ v, r, c, tr, tc };
        }
    }
    if (!moved) { g->nmv = 0; return; }
    memcpy(g->grid, ng, sizeof(ng));
    g->score += gained;
    if (g->score > g->best) g->best = g->score;
    add_tile(g);
    g->anim_t0 = uptime_ms();
    audio_sound(gained ? SND_POP : SND_CLICK);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (g->grid[r][c] == 2048 && !g->won) { g->won = true; audio_sound(SND_SUCCESS); }
    if (!can_move(g)) { g->over = true; audio_sound(SND_ERROR); }
}

static void tile_colors(int v, color_t *bg, color_t *fg)
{
    static const struct { int v; uint32_t bg, fg; } t[] = {
        { 2, 0xEEE4DA, 0x776E65 }, { 4, 0xEDE0C8, 0x776E65 }, { 8, 0xF2B179, 0xF9F6F2 }, { 16, 0xF59563, 0xF9F6F2 },
        { 32, 0xF67C5F, 0xF9F6F2 }, { 64, 0xF65E3B, 0xF9F6F2 }, { 128, 0xEDCF72, 0xF9F6F2 }, { 256, 0xEDCC61, 0xF9F6F2 },
        { 512, 0xEDC850, 0xF9F6F2 }, { 1024, 0xEDC53F, 0xF9F6F2 }, { 2048, 0xEDC22E, 0xF9F6F2 },
    };
    *bg = HEX(0x3C3A32);
    *fg = HEX(0xF9F6F2);
    for (unsigned i = 0; i < ARRAY_SIZE(t); i++)
        if (t[i].v == v) { *bg = HEX(t[i].bg); *fg = HEX(t[i].fg); }
}

static void draw_tile(surface_t *s, int v, float x, float y, float size, float scale)
{
    color_t bg, fg;
    tile_colors(v, &bg, &fg);
    float sz = size * scale;
    int ix = (int)(x + (size - sz) / 2), iy = (int)(y + (size - sz) / 2);
    gfx_round_rect(s, ix, iy, (int)sz, (int)sz, (int)(sz * 0.08f), bg);
    if (v >= 128) gfx_round_outline(s, ix, iy, (int)sz, (int)sz, (int)(sz * 0.08f), ALPHA(0xFFFFFF, 60));
    char b[12];
    snprintf(b, sizeof(b), "%d", v);
    font_t *f = v < 1000 && sz > 80 ? font_huge : font_light;
    if (scale < 0.6f) return;
    gfx_text_center(s, f, R(ix, iy - (f == font_huge ? 4 : 0), (int)sz, (int)sz), b, fg);
}

static void g2048_paint(app_t *a, surface_t *s)
{
    struct g2048 *g = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, HEX(0x1A1C2C));
    /* header */
    gfx_text(s, font_huge, 24, 6, "2048", HEX(0xEDC22E));
    char b[32];
    for (int k = 0; k < 2; k++) {
        rect_t r = R(W - 24 - (2 - k) * 110 + 10, 18, 100, 56);
        gfx_round_rect(s, r.x, r.y, r.w, r.h, 10, HEX(0x2C2F48));
        gfx_text_center(s, font_ui, R(r.x, r.y + 6, r.w, 16), k ? "BEST" : "SCORE", theme.text_faint);
        snprintf(b, sizeof(b), "%d", k ? g->best : g->score);
        gfx_text_center(s, font_bold_lg, R(r.x, r.y + 24, r.w, 26), b, theme.text);
    }
    /* board */
    int bs = MIN(W - 48, H - 150);
    int bx = (W - bs) / 2, by = 96;
    float gap = (float)bs * 0.03f, cell = ((float)bs - gap * 5) / 4.0f;
    gfx_round_rect(s, bx, by, bs, bs, 12, HEX(0x2C2F48));
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            gfx_round_rect(s, (int)((float)bx + gap + (float)c * (cell + gap)), (int)((float)by + gap + (float)r * (cell + gap)),
                           (int)cell, (int)cell, (int)(cell * 0.08f), HEX(0x3A3E5C));
    float t = (float)(uptime_ms() - g->anim_t0) / 110.0f;
    if (t < 1.0f && g->nmv) {
        float e = 1 - (1 - t) * (1 - t);
        for (int i = 0; i < g->nmv; i++) {
            struct mover *m = &g->mv[i];
            float cx = (float)m->fc + (float)(m->tc - m->fc) * e, cy = (float)m->fr + (float)(m->tr - m->fr) * e;
            draw_tile(s, m->v, (float)bx + gap + cx * (cell + gap), (float)by + gap + cy * (cell + gap), cell, 1.0f);
        }
        u->want_repaint = true;
    } else {
        float t2 = (float)(uptime_ms() - g->anim_t0 - 110) / 140.0f;
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                int v = g->grid[r][c];
                if (!v) continue;
                float scale = 1.0f;
                if (t2 < 1.0f) {
                    if (r == g->new_r && c == g->new_c) scale = t2 < 0 ? 0 : t2;
                    else if (g->merged[r][c]) scale = 1.0f + 0.12f * (float)k_sin((double)(t2 < 0 ? 0 : t2) * 3.14159);
                    u->want_repaint = true;
                }
                if (scale <= 0) continue;
                draw_tile(s, v, (float)bx + gap + (float)c * (cell + gap), (float)by + gap + (float)r * (cell + gap), cell, scale);
            }
    }
    if (g->over || (g->won && !g->keep_going)) {
        gfx_round_rect(s, bx, by, bs, bs, 12, ALPHA(0x1A1C2C, 170));
        gfx_text_center(s, font_light, R(bx, by + bs / 2 - 60, bs, 40), g->won ? "You made 2048!" : "Game over", theme.text);
        if (g->won && !g->over && ui_button(u, R(bx + bs / 2 - 160, by + bs / 2, 150, 40), "Keep going", BTN_NORMAL))
            g->keep_going = true;
        if (ui_button(u, R(bx + bs / 2 + (g->won && !g->over ? 10 : -75), by + bs / 2, 150, 40), "New game", BTN_PRIMARY))
            g2048_reset(g);
    }
    gfx_text_center(s, font_ui, R(0, by + bs + 14, W, 20), "Arrow keys, WASD or drag with the mouse", theme.text_faint);
    /* mouse swipes */
    if (u->mpressed && rect_has(R(bx, by, bs, bs), u->mx, u->my)) { g->dragging = true; g->drag_x = u->mx; g->drag_y = u->my; }
    if (u->mreleased && g->dragging) {
        g->dragging = false;
        int dx = u->mx - g->drag_x, dy = u->my - g->drag_y;
        if (ABS(dx) > 30 || ABS(dy) > 30) {
            if (ABS(dx) > ABS(dy)) g2048_move(g, dx < 0 ? 0 : 1);
            else g2048_move(g, dy < 0 ? 2 : 3);
            u->want_repaint = true;
        }
    }
}

static int g2048_event(app_t *a, struct gui_event *ev)
{
    struct g2048 *g = a->data;
    if (ev->type != EV_KEY_DOWN) return 0;
    switch (ev->key) {
    case KEY_LEFT: case KEY_A: g2048_move(g, 0); return 1;
    case KEY_RIGHT: case KEY_D: g2048_move(g, 1); return 1;
    case KEY_UP: case KEY_W: g2048_move(g, 2); return 1;
    case KEY_DOWN: case KEY_S: g2048_move(g, 3); return 1;
    case KEY_F2: g2048_reset(g); return 1;
    }
    return 0;
}

int g2048_main(void *arg)
{
    UNUSED(arg);
    struct g2048 *g = kzalloc(sizeof(*g));
    g2048_reset(g);
    app_t a = { 0 };
    a.data = g;
    a.win = wm_create("2048", 520, 640, WF_RESIZABLE);
    if (!a.win) { kfree(g); return 1; }
    wm_set_icon(a.win, ICON_2048);
    wm_set_min_size(a.win, 400, 520);
    a.on_paint = g2048_paint;
    a.on_event = g2048_event;
    int r = app_run(&a);
    kfree(g);
    return r;
}
