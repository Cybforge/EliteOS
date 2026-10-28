/* Games: Blocks (falling-blocks puzzle) and Snake. */
#include <wm.h>
#include <audio.h>
#include <mm.h>

/* ========================================================================
 * Blocks
 * ====================================================================== */

#define BW 10
#define BH 20

static const uint16_t shapes[7][4] = {
    { 0x0F00, 0x2222, 0x00F0, 0x4444 },   /* I */
    { 0x8E00, 0x6440, 0x0E20, 0x44C0 },   /* J */
    { 0x2E00, 0x4460, 0x0E80, 0xC440 },   /* L */
    { 0x6600, 0x6600, 0x6600, 0x6600 },   /* O */
    { 0x6C00, 0x4620, 0x06C0, 0x8C40 },   /* S */
    { 0x4E00, 0x4640, 0x0E40, 0x4C40 },   /* T */
    { 0xC600, 0x2640, 0x0C60, 0x4C80 },   /* Z */
};
static const uint32_t piece_col[7] = { 0x2BD4E6, 0x5B7CFF, 0xFF9A40, 0xFFD24F, 0x35D69A, 0xB06BFF, 0xFF5C7A };

struct blocks {
    uint8_t grid[BH][BW];       /* 0 empty, else piece+1 */
    int cur, rot, px, py;
    int next, hold;
    bool held;
    int score, lines, level;
    bool over, paused;
    uint64_t last_drop;
    int bag[7], bag_n;
    int clear_rows[4], nclear;
    uint64_t clear_t;
    int best;
};

static bool cell(int shape, int rot, int x, int y)
{
    return shapes[shape][rot] & (0x8000 >> (y * 4 + x));
}

static bool fits(struct blocks *b, int shape, int rot, int px, int py)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            if (!cell(shape, rot, x, y)) continue;
            int gx = px + x, gy = py + y;
            if (gx < 0 || gx >= BW || gy >= BH) return false;
            if (gy >= 0 && b->grid[gy][gx]) return false;
        }
    return true;
}

static int next_piece(struct blocks *b)
{
    if (!b->bag_n) {
        for (int i = 0; i < 7; i++) b->bag[i] = i;
        for (int i = 6; i > 0; i--) {
            int j = (int)(krand() % (uint32_t)(i + 1));
            int t = b->bag[i]; b->bag[i] = b->bag[j]; b->bag[j] = t;
        }
        b->bag_n = 7;
    }
    return b->bag[--b->bag_n];
}

static void spawn(struct blocks *b)
{
    b->cur = b->next;
    b->next = next_piece(b);
    b->rot = 0;
    b->px = 3;
    b->py = -1;
    b->held = false;
    if (!fits(b, b->cur, b->rot, b->px, b->py)) {
        b->over = true;
        audio_sound(SND_ERROR);
        if (b->score > b->best) b->best = b->score;
    }
}

static void reset_blocks(struct blocks *b)
{
    int best = b->best;
    memset(b, 0, sizeof(*b));
    b->best = best;
    b->hold = -1;
    b->level = 1;
    ksrand(uptime_ms() * 2654435761u);
    b->next = next_piece(b);
    spawn(b);
    b->last_drop = uptime_ms();
}

static void lock_piece(struct blocks *b)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (cell(b->cur, b->rot, x, y) && b->py + y >= 0) b->grid[b->py + y][b->px + x] = (uint8_t)(b->cur + 1);
    b->nclear = 0;
    for (int y = 0; y < BH; y++) {
        bool full = true;
        for (int x = 0; x < BW; x++) if (!b->grid[y][x]) full = false;
        if (full) b->clear_rows[b->nclear++] = y;
    }
    if (b->nclear) {
        static const int pts[] = { 0, 100, 300, 500, 800 };
        b->score += pts[b->nclear] * b->level;
        b->lines += b->nclear;
        b->level = 1 + b->lines / 10;
        b->clear_t = uptime_ms();
        audio_sound(SND_SUCCESS);
    }
    spawn(b);
}

static void finish_clear(struct blocks *b)
{
    for (int i = 0; i < b->nclear; i++) {
        int row = b->clear_rows[i];
        memmove(&b->grid[1][0], &b->grid[0][0], (size_t)row * BW);
        memset(b->grid[0], 0, BW);
    }
    b->nclear = 0;
}

static void draw_cell(surface_t *s, int x, int y, int sz, uint32_t col, int alpha)
{
    color_t c = HEX(col);
    if (alpha < 255) {
        gfx_round_outline(s, x + 1, y + 1, sz - 2, sz - 2, 4, ALPHA(c, alpha));
        gfx_round_rect(s, x + 2, y + 2, sz - 4, sz - 4, 4, ALPHA(c, alpha / 5));
        return;
    }
    gfx_round_rect_grad(s, x + 1, y + 1, sz - 2, sz - 2, 4, color_lighten(c, 30), color_darken(c, 30));
    gfx_fill(s, x + 4, y + 3, sz - 8, 2, ALPHA(0xFFFFFF, 90));
}

static void draw_mini(surface_t *s, int shape, int x, int y, int sz)
{
    if (shape < 0) return;
    for (int yy = 0; yy < 4; yy++)
        for (int xx = 0; xx < 4; xx++)
            if (cell(shape, 0, xx, yy)) draw_cell(s, x + xx * sz, y + yy * sz, sz, piece_col[shape], 255);
}

static void blocks_paint(app_t *a, surface_t *s)
{
    struct blocks *b = a->data;
    int W = s->w, H = s->h;
    gfx_vgradient(s, 0, 0, W, H, HEX(0x171A33), HEX(0x0C0E1C));
    int sz = MIN((H - 40) / BH, (W - 300) / BW);
    int gw = sz * BW, gh = sz * BH;
    int gx = (W - gw) / 2 - 60, gy = (H - gh) / 2;
    gfx_round_rect(s, gx - 8, gy - 8, gw + 16, gh + 16, 12, ALPHA(0x000000, 90));
    for (int y = 0; y < BH; y++)
        for (int x = 0; x < BW; x++)
            gfx_fill(s, gx + x * sz + sz / 2, gy + y * sz + sz / 2, 1, 1, ALPHA(0xFFFFFF, 40));
    uint64_t now = uptime_ms();
    bool flashing = b->nclear && now - b->clear_t < 260;
    for (int y = 0; y < BH; y++) {
        bool clearing = false;
        for (int i = 0; i < b->nclear; i++) if (b->clear_rows[i] == y) clearing = true;
        for (int x = 0; x < BW; x++) {
            if (!b->grid[y][x]) continue;
            if (clearing && flashing) {
                int a2 = (int)(255 - (now - b->clear_t) * 255 / 260);
                gfx_round_rect(s, gx + x * sz + 1, gy + y * sz + 1, sz - 2, sz - 2, 4, ALPHA(0xFFFFFF, MAX(a2, 40)));
            } else {
                draw_cell(s, gx + x * sz, gy + y * sz, sz, piece_col[b->grid[y][x] - 1], 255);
            }
        }
    }
    if (!b->over && !b->nclear) {
        int gyost = b->py;
        while (fits(b, b->cur, b->rot, b->px, gyost + 1)) gyost++;
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                if (!cell(b->cur, b->rot, x, y)) continue;
                if (gyost + y >= 0) draw_cell(s, gx + (b->px + x) * sz, gy + (gyost + y) * sz, sz, piece_col[b->cur], 120);
                if (b->py + y >= 0) draw_cell(s, gx + (b->px + x) * sz, gy + (b->py + y) * sz, sz, piece_col[b->cur], 255);
            }
    }
    /* side panel */
    int px = gx + gw + 32;
    int ms = MAX(14, sz * 2 / 3);
    gfx_text(s, font_ui_md, px, gy, "NEXT", theme.text_dim);
    gfx_round_rect(s, px, gy + 22, ms * 4 + 16, ms * 3 + 16, 10, ALPHA(0xFFFFFF, 10));
    draw_mini(s, b->next, px + 8, gy + 30, ms);
    gfx_text(s, font_ui_md, px, gy + 40 + ms * 3 + 20, "HOLD  (C)", theme.text_dim);
    gfx_round_rect(s, px, gy + 62 + ms * 3 + 20, ms * 4 + 16, ms * 3 + 16, 10, ALPHA(0xFFFFFF, 10));
    draw_mini(s, b->hold, px + 8, gy + 70 + ms * 3 + 20, ms);
    char buf[32];
    int ty = gy + 110 + ms * 6 + 40;
    gfx_text(s, font_ui, px, ty, "SCORE", theme.text_dim);
    snprintf(buf, sizeof(buf), "%d", b->score);
    gfx_text(s, font_bold_lg, px, ty + 18, buf, theme.text);
    gfx_text(s, font_ui, px, ty + 52, "LINES", theme.text_dim);
    snprintf(buf, sizeof(buf), "%d", b->lines);
    gfx_text(s, font_ui_bold, px, ty + 70, buf, theme.text);
    gfx_text(s, font_ui, px + 80, ty + 52, "LEVEL", theme.text_dim);
    snprintf(buf, sizeof(buf), "%d", b->level);
    gfx_text(s, font_ui_bold, px + 80, ty + 70, buf, theme.text);
    snprintf(buf, sizeof(buf), "Best %d", b->best);
    gfx_text(s, font_ui, px, ty + 100, buf, theme.text_faint);
    gfx_text(s, font_ui, 16, H - 26, "arrows: move  ·  up: rotate  ·  space: drop  ·  P: pause", theme.text_faint);

    if (b->over || b->paused) {
        gfx_fill(s, gx - 8, gy - 8, gw + 16, gh + 16, ALPHA(0x05060F, 180));
        gfx_text_center(s, font_light, R(gx, gy + gh / 2 - 50, gw, 40), b->over ? "Game over" : "Paused", 0xFFFFFFFFu);
        gfx_text_center(s, font_ui, R(gx, gy + gh / 2, gw, 20), b->over ? "Press Enter to play again" : "Press P to continue",
                        theme.text_dim);
    }
}

static int blocks_event(app_t *a, struct gui_event *ev)
{
    struct blocks *b = a->data;
    uint64_t now = uptime_ms();
    if (ev->type == EV_TIMER) {
        if (b->nclear && now - b->clear_t >= 260) finish_clear(b);
        if (b->over || b->paused || b->nclear) return b->nclear != 0;
        uint64_t interval = (uint64_t)MAX(80, 800 - (b->level - 1) * 70);
        if (now - b->last_drop >= interval) {
            b->last_drop = now;
            if (fits(b, b->cur, b->rot, b->px, b->py + 1)) b->py++;
            else lock_piece(b);
            return 1;
        }
        return 0;
    }
    if (ev->type != EV_KEY_DOWN) return 0;
    if (b->over) { if (ev->key == KEY_ENTER || ev->key == KEY_SPACE) reset_blocks(b); return 1; }
    if (ev->key == KEY_P || ev->key == KEY_ESC) { b->paused = !b->paused; return 1; }
    if (b->paused || b->nclear) return 0;
    switch (ev->key) {
    case KEY_LEFT: if (fits(b, b->cur, b->rot, b->px - 1, b->py)) b->px--; break;
    case KEY_RIGHT: if (fits(b, b->cur, b->rot, b->px + 1, b->py)) b->px++; break;
    case KEY_DOWN:
        if (fits(b, b->cur, b->rot, b->px, b->py + 1)) { b->py++; b->score++; }
        else lock_piece(b);
        b->last_drop = now;
        break;
    case KEY_UP: case KEY_X: {
        int r = (b->rot + 1) % 4;
        static const int kicks[] = { 0, -1, 1, -2, 2 };
        for (int k = 0; k < 5; k++)
            if (fits(b, b->cur, r, b->px + kicks[k], b->py)) { b->rot = r; b->px += kicks[k]; break; }
        break;
    }
    case KEY_SPACE:
        while (fits(b, b->cur, b->rot, b->px, b->py + 1)) { b->py++; b->score += 2; }
        lock_piece(b);
        b->last_drop = now;
        break;
    case KEY_C:
        if (!b->held) {
            int h = b->hold;
            b->hold = b->cur;
            if (h < 0) spawn(b);
            else { b->cur = h; b->rot = 0; b->px = 3; b->py = -1; }
            b->held = true;
        }
        break;
    }
    return 1;
}

int tetris_main(void *arg)
{
    UNUSED(arg);
    struct blocks *b = kzalloc(sizeof(*b));
    reset_blocks(b);
    app_t a = { 0 };
    a.data = b;
    a.win = wm_create("Blocks", 560, 640, 0);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_TETRIS);
    wm_set_timer(a.win, 30);
    a.on_paint = blocks_paint;
    a.on_event = blocks_event;
    int r = app_run(&a);
    kfree(b);
    return r;
}

/* ========================================================================
 * Snake
 * ====================================================================== */

#define SW_ 26
#define SH_ 18
#define SMAX (SW_ * SH_)

struct snake {
    int x[SMAX], y[SMAX];
    int len;
    int dir, ndir;          /* 0 right 1 down 2 left 3 up */
    int fx, fy;
    int score, best;
    bool over, started;
    uint64_t last;
};

static void place_food(struct snake *s)
{
    for (;;) {
        int fx = (int)(krand() % SW_), fy = (int)(krand() % SH_);
        bool hit = false;
        for (int i = 0; i < s->len; i++) if (s->x[i] == fx && s->y[i] == fy) hit = true;
        if (!hit) { s->fx = fx; s->fy = fy; return; }
    }
}

static void reset_snake(struct snake *s)
{
    int best = s->best;
    memset(s, 0, sizeof(*s));
    s->best = best;
    s->len = 4;
    for (int i = 0; i < s->len; i++) { s->x[i] = 8 - i; s->y[i] = SH_ / 2; }
    ksrand(uptime_ms() * 7919);
    place_food(s);
    s->last = uptime_ms();
}

static void snake_paint(app_t *a, surface_t *sf)
{
    struct snake *s = a->data;
    int W = sf->w, H = sf->h;
    gfx_vgradient(sf, 0, 0, W, H, HEX(0x10241C), HEX(0x0A1410));
    int cs = MIN((W - 40) / SW_, (H - 90) / SH_);
    int gx = (W - cs * SW_) / 2, gy = 60;
    gfx_round_rect(sf, gx - 6, gy - 6, cs * SW_ + 12, cs * SH_ + 12, 12, ALPHA(0x000000, 80));
    for (int y = 0; y < SH_; y++)
        for (int x = 0; x < SW_; x++)
            if ((x + y) % 2) gfx_fill(sf, gx + x * cs, gy + y * cs, cs, cs, ALPHA(0xFFFFFF, 6));
    /* food with glow */
    float fcx = (float)(gx + s->fx * cs) + (float)cs / 2, fcy = (float)(gy + s->fy * cs) + (float)cs / 2;
    gfx_circle(sf, fcx, fcy, (float)cs * 0.8f, ALPHA(0xFF5C7A, 40));
    gfx_circle(sf, fcx, fcy, (float)cs * 0.38f, HEX(0xFF5C7A));
    for (int i = s->len - 1; i >= 0; i--) {
        float t = (float)i / (float)MAX(1, s->len);
        color_t c = color_lerp(HEX(0x7CFFB2), HEX(0x1E9E6A), (int)(t * 256));
        int pad = i == 0 ? 1 : 2;
        gfx_round_rect(sf, gx + s->x[i] * cs + pad, gy + s->y[i] * cs + pad, cs - 2 * pad, cs - 2 * pad, cs / 3, c);
        if (i > 0) {
            /* bridge to the previous segment for a continuous body */
            int dx = s->x[i - 1] - s->x[i], dy = s->y[i - 1] - s->y[i];
            if (ABS(dx) + ABS(dy) == 1)
                gfx_fill(sf, gx + s->x[i] * cs + pad + (dx > 0 ? cs / 2 : dx < 0 ? -cs / 2 : 0),
                         gy + s->y[i] * cs + pad + (dy > 0 ? cs / 2 : dy < 0 ? -cs / 2 : 0), cs - 2 * pad, cs - 2 * pad, c);
        }
    }
    /* eyes */
    float hx = (float)(gx + s->x[0] * cs) + (float)cs / 2, hy = (float)(gy + s->y[0] * cs) + (float)cs / 2;
    float ex = s->dir == 0 ? 3 : s->dir == 2 ? -3 : 0, ey = s->dir == 1 ? 3 : s->dir == 3 ? -3 : 0;
    float ox = ey != 0 ? 4 : 0, oy = ex != 0 ? 4 : 0;
    gfx_circle(sf, hx + ex + ox, hy + ey + oy, 2.2f, HEX(0x0A1410));
    gfx_circle(sf, hx + ex - ox, hy + ey - oy, 2.2f, HEX(0x0A1410));

    char b[48];
    snprintf(b, sizeof(b), "Score %d", s->score);
    gfx_text(sf, font_bold_lg, gx, 18, b, theme.text);
    snprintf(b, sizeof(b), "Best %d", s->best);
    gfx_text_right(sf, font_ui_md, gx + cs * SW_, 24, b, theme.text_dim);
    if (!s->started || s->over) {
        gfx_fill(sf, gx - 6, gy - 6, cs * SW_ + 12, cs * SH_ + 12, ALPHA(0x05060F, 150));
        gfx_text_center(sf, font_light, R(gx, gy + cs * SH_ / 2 - 50, cs * SW_, 40), s->over ? "Game over" : "Snake",
                        0xFFFFFFFFu);
        gfx_text_center(sf, font_ui, R(gx, gy + cs * SH_ / 2, cs * SW_, 20), "Use the arrow keys  ·  Enter to start",
                        theme.text_dim);
    }
}

static int snake_event(app_t *a, struct gui_event *ev)
{
    struct snake *s = a->data;
    if (ev->type == EV_KEY_DOWN) {
        if (!s->started || s->over) {
            if (ev->key == KEY_ENTER || ev->key == KEY_SPACE) { if (s->over) reset_snake(s); s->started = true; }
            return 1;
        }
        int nd = s->ndir;
        if (ev->key == KEY_RIGHT) nd = 0;
        if (ev->key == KEY_DOWN) nd = 1;
        if (ev->key == KEY_LEFT) nd = 2;
        if (ev->key == KEY_UP) nd = 3;
        if ((nd + 2) % 4 != s->dir) s->ndir = nd;
        return 0;
    }
    if (ev->type != EV_TIMER || !s->started || s->over) return 0;
    uint64_t now = uptime_ms();
    uint64_t speed = (uint64_t)MAX(55, 140 - s->score * 2);
    if (now - s->last < speed) return 0;
    s->last = now;
    s->dir = s->ndir;
    int nx = s->x[0] + (s->dir == 0 ? 1 : s->dir == 2 ? -1 : 0);
    int ny = s->y[0] + (s->dir == 1 ? 1 : s->dir == 3 ? -1 : 0);
    if (nx < 0 || ny < 0 || nx >= SW_ || ny >= SH_) { s->over = true; }
    for (int i = 0; i < s->len - 1 && !s->over; i++) if (s->x[i] == nx && s->y[i] == ny) s->over = true;
    if (s->over) { if (s->score > s->best) s->best = s->score; audio_sound(SND_ERROR); return 1; }
    bool eat = nx == s->fx && ny == s->fy;
    if (eat && s->len < SMAX) s->len++;
    for (int i = s->len - 1; i > 0; i--) { s->x[i] = s->x[i - 1]; s->y[i] = s->y[i - 1]; }
    s->x[0] = nx;
    s->y[0] = ny;
    if (eat) { s->score++; place_food(s); audio_sound(SND_POP); }
    return 1;
}

int snake_main(void *arg)
{
    UNUSED(arg);
    struct snake *s = kzalloc(sizeof(*s));
    reset_snake(s);
    app_t a = { 0 };
    a.data = s;
    a.win = wm_create("Snake", 640, 520, 0);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_SNAKE);
    wm_set_timer(a.win, 20);
    a.on_paint = snake_paint;
    a.on_event = snake_event;
    int r = app_run(&a);
    kfree(s);
    return r;
}
