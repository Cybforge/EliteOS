/*
 * Terminal emulator: scrollback, UTF-8, ANSI escape sequences (16 colours
 * and 24-bit true colour), mouse selection with clipboard, translucent
 * background. The shell runs on its own thread and talks through a tty.
 */
#include <wm.h>
#include <tty.h>
#include <sched.h>
#include <mm.h>

#define TCOLS 200
#define TLINES 1200
#define DEF_FG 0xFFD7DBEEu
#define DEF_BG 0x00000000u      /* alpha 0 = default background */

struct tcell {
    uint32_t ch;
    uint32_t fg;
    uint32_t bg;
};

struct term {
    spinlock_t lock;
    struct tcell *buf;          /* TLINES x TCOLS ring */
    uint64_t total;             /* lines ever created */
    uint64_t cline;             /* cursor line (absolute) */
    int cx;
    int cols, rows;
    int cw, chh;                /* cell size */
    uint32_t fg, bg;
    bool bold;
    int scroll;                 /* lines scrolled back by the user */
    /* escape parser */
    int esc;                    /* 0 none, 1 got ESC, 2 in CSI */
    char csi[32];
    int csilen;
    uint8_t utf[4];
    int utflen, utfneed;
    /* selection */
    bool selecting, has_sel;
    uint64_t sl0, sl1;
    int sc0, sc1;
    volatile bool repaint_posted;
    bool win_gone;
    bool cursor_on;
    window_t *win;
    struct tty tty;
    uint8_t opacity;
};

static const uint32_t ansi16[16] = {
    0xFF1C1F2E, 0xFFFF5C7A, 0xFF35D69A, 0xFFFFC233, 0xFF5B9BFF, 0xFFB06BFF, 0xFF2BD4E6, 0xFFD7DBEE,
    0xFF646B90, 0xFFFF8AA0, 0xFF6BF0B8, 0xFFFFDA70, 0xFF8AB8FF, 0xFFCD9CFF, 0xFF70ECF7, 0xFFFFFFFF,
};

static struct tcell *line_ptr(struct term *t, uint64_t l)
{
    return &t->buf[(l % TLINES) * TCOLS];
}

static void clear_line(struct term *t, uint64_t l)
{
    struct tcell *c = line_ptr(t, l);
    for (int i = 0; i < TCOLS; i++) { c[i].ch = ' '; c[i].fg = DEF_FG; c[i].bg = DEF_BG; }
}

static uint64_t screen_top(struct term *t)
{
    return t->total > (uint64_t)t->rows ? t->total - (uint64_t)t->rows : 0;
}

static void new_line(struct term *t)
{
    if (t->cline + 1 >= t->total) {
        clear_line(t, t->total);
        t->total++;
    }
    t->cline++;
    t->cx = 0;
}

static void put_char(struct term *t, uint32_t ch)
{
    if (t->cx >= t->cols) new_line(t);
    struct tcell *c = &line_ptr(t, t->cline)[t->cx];
    c->ch = ch;
    c->fg = t->bold && t->fg == DEF_FG ? 0xFFFFFFFFu : t->fg;
    c->bg = t->bg;
    t->cx++;
}

static int csi_arg(struct term *t, int idx, int def)
{
    int n = 0, v = -1, cur = 0;
    bool have = false;
    for (int i = 0; i <= t->csilen; i++) {
        char ch = i < t->csilen ? t->csi[i] : ';';
        if (ch == ';') {
            if (n == idx) return have ? v : def;
            n++;
            cur = 0;
            have = false;
            v = -1;
        } else if (ch >= '0' && ch <= '9') {
            cur = cur * 10 + (ch - '0');
            v = cur;
            have = true;
        }
    }
    return def;
}

static int csi_count(struct term *t)
{
    int n = 1;
    for (int i = 0; i < t->csilen; i++) if (t->csi[i] == ';') n++;
    return n;
}

static void sgr(struct term *t)
{
    int n = csi_count(t);
    if (t->csilen == 0) { t->fg = DEF_FG; t->bg = DEF_BG; t->bold = false; return; }
    for (int i = 0; i < n; i++) {
        int a = csi_arg(t, i, 0);
        if (a == 0) { t->fg = DEF_FG; t->bg = DEF_BG; t->bold = false; }
        else if (a == 1) t->bold = true;
        else if (a == 22) t->bold = false;
        else if (a >= 30 && a <= 37) t->fg = ansi16[a - 30 + (t->bold ? 8 : 0)];
        else if (a == 39) t->fg = DEF_FG;
        else if (a >= 40 && a <= 47) t->bg = ansi16[a - 40];
        else if (a == 49) t->bg = DEF_BG;
        else if (a >= 90 && a <= 97) t->fg = ansi16[a - 90 + 8];
        else if (a >= 100 && a <= 107) t->bg = ansi16[a - 100 + 8];
        else if ((a == 38 || a == 48) && csi_arg(t, i + 1, 0) == 2) {
            uint32_t c = RGB(csi_arg(t, i + 2, 0) & 255, csi_arg(t, i + 3, 0) & 255, csi_arg(t, i + 4, 0) & 255);
            if (a == 38) t->fg = c; else t->bg = c;
            i += 4;
        } else if ((a == 38 || a == 48) && csi_arg(t, i + 1, 0) == 5) {
            int idx = csi_arg(t, i + 2, 0);
            uint32_t c;
            if (idx < 16) c = ansi16[idx];
            else if (idx < 232) { idx -= 16; c = RGB((idx / 36) * 51, ((idx / 6) % 6) * 51, (idx % 6) * 51); }
            else { int g = 8 + (idx - 232) * 10; c = RGB(g, g, g); }
            if (a == 38) t->fg = c; else t->bg = c;
            i += 2;
        }
    }
}

static void do_csi(struct term *t, char final)
{
    uint64_t top = screen_top(t);
    switch (final) {
    case 'm': sgr(t); break;
    case 'J':
        if (csi_arg(t, 0, 0) == 2 || csi_arg(t, 0, 0) == 3) {
            /* push the screen into scrollback and start fresh */
            for (int i = 0; i < t->rows; i++) { clear_line(t, t->total); t->total++; }
            t->cline = screen_top(t);
            t->cx = 0;
        } else {
            for (uint64_t l = t->cline + 1; l < t->total; l++) clear_line(t, l);
            struct tcell *c = line_ptr(t, t->cline);
            for (int i = t->cx; i < TCOLS; i++) { c[i].ch = ' '; c[i].bg = DEF_BG; }
        }
        break;
    case 'K': {
        struct tcell *c = line_ptr(t, t->cline);
        int mode = csi_arg(t, 0, 0);
        int from = mode == 0 ? t->cx : 0, to = mode == 1 ? t->cx + 1 : TCOLS;
        for (int i = from; i < to && i < TCOLS; i++) { c[i].ch = ' '; c[i].fg = DEF_FG; c[i].bg = DEF_BG; }
        break;
    }
    case 'H': case 'f': {
        int r = csi_arg(t, 0, 1) - 1, c = csi_arg(t, 1, 1) - 1;
        r = CLAMP(r, 0, t->rows - 1);
        while (top + (uint64_t)r >= t->total) { clear_line(t, t->total); t->total++; }
        t->cline = top + (uint64_t)r;
        t->cx = CLAMP(c, 0, t->cols - 1);
        break;
    }
    case 'A': { int n = csi_arg(t, 0, 1); while (n-- && t->cline > top) t->cline--; break; }
    case 'B': { int n = csi_arg(t, 0, 1); while (n-- && t->cline + 1 < t->total) t->cline++; break; }
    case 'C': t->cx = MIN(t->cols - 1, t->cx + csi_arg(t, 0, 1)); break;
    case 'D': t->cx = MAX(0, t->cx - csi_arg(t, 0, 1)); break;
    case 'G': t->cx = CLAMP(csi_arg(t, 0, 1) - 1, 0, t->cols - 1); break;
    case 'l': case 'h':
        if (t->csilen >= 3 && !strncmp(t->csi, "?25", 3)) t->cursor_on = final == 'h';
        break;
    }
}

static void term_feed(struct term *t, uint32_t ch)
{
    if (t->esc == 1) {
        t->esc = ch == '[' ? 2 : 0;
        t->csilen = 0;
        return;
    }
    if (t->esc == 2) {
        if ((ch >= '0' && ch <= '9') || ch == ';' || ch == '?') {
            if (t->csilen < (int)sizeof(t->csi) - 1) t->csi[t->csilen++] = (char)ch;
        } else {
            t->csi[t->csilen] = 0;
            do_csi(t, (char)ch);
            t->esc = 0;
        }
        return;
    }
    switch (ch) {
    case 27: t->esc = 1; break;
    case '\n': new_line(t); break;
    case '\r': t->cx = 0; break;
    case '\b': if (t->cx > 0) t->cx--; break;
    case '\t': do put_char(t, ' '); while (t->cx % 8); break;
    case 7: break;
    default:
        if (ch >= 32) put_char(t, ch);
    }
}

static void term_write(void *ctx, const char *s, size_t n)
{
    struct term *t = ctx;
    spin_lock(&t->lock);
    for (size_t i = 0; i < n; i++) {
        uint8_t b = (uint8_t)s[i];
        if (t->utfneed) {
            t->utf[t->utflen++] = b;
            if (t->utflen == t->utfneed) {
                const char *p = (const char *)t->utf;
                term_feed(t, utf8_next(&p));
                t->utfneed = 0;
            }
            continue;
        }
        if (b >= 0xC0) {
            t->utf[0] = b;
            t->utflen = 1;
            t->utfneed = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : 2;
            continue;
        }
        term_feed(t, b);
    }
    t->scroll = 0;
    if (!t->repaint_posted && !t->win_gone) {
        t->repaint_posted = true;
        struct gui_event ev = { 0 };
        ev.type = EV_TIMER;
        wm_post_event(t->win, &ev);
    }
    spin_unlock(&t->lock);
}

static void term_hangup(void *ctx)
{
    struct term *t = ctx;
    spin_lock(&t->lock);
    if (!t->win_gone) {
        struct gui_event ev = { 0 };
        ev.type = EV_CLOSE;
        wm_post_event(t->win, &ev);
    }
    spin_unlock(&t->lock);
}

static void term_release(void *ctx)
{
    struct term *t = ctx;
    kfree(t->buf);
    kfree(t);
}

/* ------------------------------------------------------------------------
 * painting
 * ---------------------------------------------------------------------- */

#define PAD 8

static bool in_sel(struct term *t, uint64_t l, int c)
{
    if (!t->has_sel && !t->selecting) return false;
    uint64_t a = t->sl0, b = t->sl1;
    int ca = t->sc0, cb = t->sc1;
    if (a > b || (a == b && ca > cb)) { uint64_t x = a; a = b; b = x; int y = ca; ca = cb; cb = y; }
    if (l < a || l > b) return false;
    if (l == a && c < ca) return false;
    if (l == b && c >= cb) return false;
    return true;
}

static void term_paint(struct term *t, surface_t *s)
{
    uint32_t bgc = ALPHA(0x0E1120, t->opacity);
    memset32(s->px, bgc, (size_t)s->stride * s->h);
    spin_lock(&t->lock);
    t->repaint_posted = false;
    uint64_t top = screen_top(t);
    uint64_t view = top > (uint64_t)t->scroll ? top - (uint64_t)t->scroll : 0;
    int asc_off = 1;
    for (int r = 0; r < t->rows; r++) {
        uint64_t l = view + (uint64_t)r;
        if (l >= t->total) break;
        if (t->total - l > TLINES) continue;
        struct tcell *line = line_ptr(t, l);
        int y = PAD + r * t->chh;
        for (int c = 0; c < t->cols; c++) {
            struct tcell *cell = &line[c];
            int x = PAD + c * t->cw;
            bool sel = in_sel(t, l, c);
            if (sel) gfx_fill(s, x, y, t->cw, t->chh, ALPHA(theme.accent, 120));
            else if (C_A(cell->bg)) gfx_fill(s, x, y, t->cw, t->chh, cell->bg);
            if (cell->ch != ' ' && cell->ch) {
                uint32_t ch = cell->ch;
                /* box drawing and block characters fill the whole cell */
                if (ch == 0x2588) gfx_fill(s, x, y, t->cw, t->chh, cell->fg);
                else if (ch == 0x2580) gfx_fill(s, x, y, t->cw, t->chh / 2, cell->fg);
                else if (ch == 0x2584) gfx_fill(s, x, y + t->chh / 2, t->cw, t->chh - t->chh / 2, cell->fg);
                else gfx_char(s, font_mono, x, y + asc_off, ch, cell->fg);
            }
        }
    }
    /* cursor */
    if (t->cursor_on && !t->scroll && t->cline >= top) {
        int r = (int)(t->cline - top);
        int x = PAD + MIN(t->cx, t->cols - 1) * t->cw, y = PAD + r * t->chh;
        bool focused = true;
        if (focused) gfx_fill(s, x, y + 1, 2, t->chh - 2, theme.accent2);
    }
    /* scrollback indicator */
    if (t->scroll) {
        uint64_t lines = MIN(t->total, (uint64_t)TLINES);
        int h = s->h - 8;
        int th = MAX(20, (int)((uint64_t)h * (uint64_t)t->rows / MAX(lines, 1)));
        int ty = 4 + (int)((uint64_t)(h - th) * (view - (t->total - lines)) / MAX(lines - (uint64_t)t->rows, 1));
        gfx_round_rect(s, s->w - 8, ty, 4, th, 2, ALPHA(0xFFFFFF, 90));
    }
    spin_unlock(&t->lock);
}

static void term_resize(struct term *t, int w, int h)
{
    spin_lock(&t->lock);
    t->cols = CLAMP((w - 2 * PAD) / t->cw, 10, TCOLS);
    t->rows = CLAMP((h - 2 * PAD) / t->chh, 3, TLINES / 2);
    while (t->total < (uint64_t)t->rows) {
        /* keep content at the top when the window is taller than the text */
        clear_line(t, t->total);
        t->total++;
    }
    if (t->cline < screen_top(t)) t->cline = screen_top(t);
    t->tty.cols = t->cols;
    t->tty.rows = t->rows;
    spin_unlock(&t->lock);
}

static void cell_at(struct term *t, int x, int y, uint64_t *l, int *c)
{
    uint64_t top = screen_top(t);
    uint64_t view = top > (uint64_t)t->scroll ? top - (uint64_t)t->scroll : 0;
    int r = CLAMP((y - PAD) / t->chh, 0, t->rows - 1);
    *c = CLAMP((x - PAD + t->cw / 2) / t->cw, 0, t->cols);
    *l = view + (uint64_t)r;
}

static void copy_selection(struct term *t)
{
    uint64_t a = t->sl0, b = t->sl1;
    int ca = t->sc0, cb = t->sc1;
    if (a > b || (a == b && ca > cb)) { uint64_t x = a; a = b; b = x; int y = ca; ca = cb; cb = y; }
    size_t cap = (size_t)(b - a + 1) * (TCOLS * 4 + 1) + 1;
    char *out = kmalloc(cap);
    if (!out) return;
    size_t n = 0;
    for (uint64_t l = a; l <= b; l++) {
        struct tcell *line = line_ptr(t, l);
        int from = l == a ? ca : 0, to = l == b ? cb : t->cols;
        int last = to;
        while (last > from && line[last - 1].ch == ' ') last--;
        for (int c = from; c < last; c++) n += (size_t)utf8_encode(line[c].ch, out + n);
        if (l != b) out[n++] = '\n';
    }
    clipboard_set(out, n);
    kfree(out);
}

int terminal_main(void *arg)
{
    const char *start_cmd = arg;
    struct term *t = kzalloc(sizeof(*t));
    if (!t) return 1;
    t->buf = kmalloc(sizeof(struct tcell) * TCOLS * TLINES);
    if (!t->buf) { kfree(t); return 1; }
    spin_init(&t->lock, "term");
    t->cw = font_char_width(font_mono, 'M');
    t->chh = font_height(font_mono) + 1;
    t->fg = DEF_FG;
    t->bg = DEF_BG;
    t->cursor_on = true;
    t->opacity = 242;
    clear_line(t, 0);
    t->total = 1;
    int W = t->cw * 92 + 2 * PAD, H = t->chh * 28 + 2 * PAD;
    t->win = wm_create("Terminal", W, H, WF_RESIZABLE | WF_TRANSLUCENT);
    if (!t->win) { kfree(t->buf); kfree(t); return 1; }
    wm_set_icon(t->win, ICON_TERMINAL);
    wm_set_min_size(t->win, 320, 160);
    term_resize(t, W, H);
    tty_init(&t->tty, term_write, t);
    t->tty.hangup = term_hangup;
    t->tty.release = term_release;
    tty_get(&t->tty);                 /* one reference for the shell */
    t->tty.cols = t->cols;
    t->tty.rows = t->rows;
    if (start_cmd) {
        strlcpy(t->tty.line, start_cmd, sizeof(t->tty.line));
        kfree((void *)start_cmd);
    }
    thread_t *sh = thread_create_ex("shell", shell_main, &t->tty, 0, -1, NULL, 64 * 1024);
    UNUSED(sh);

    bool dirty = true;
    struct gui_event ev;
    for (;;) {
        if (dirty) {
            surface_t *s = wm_begin(t->win);
            term_paint(t, s);
            wm_present(t->win);
            dirty = false;
        }
        if (!wm_wait_event(t->win, &ev, dirty ? 0 : -1)) continue;
        if (ev.type == EV_CLOSE) break;
        switch (ev.type) {
        case EV_RESIZE:
            term_resize(t, ev.x, ev.y);
            dirty = true;
            break;
        case EV_TIMER:
            dirty = true;
            break;
        case EV_KEY_DOWN:
            if ((ev.mods & MOD_CTRL) && (ev.mods & MOD_SHIFT) && ev.key == KEY_V) {
                char *c = clipboard_get();
                if (c) { tty_paste(&t->tty, c); kfree(c); }
                break;
            }
            if ((ev.mods & MOD_CTRL) && (ev.mods & MOD_SHIFT) && ev.key == KEY_C) {
                if (t->has_sel) copy_selection(t);
                break;
            }
            if ((ev.mods & MOD_SHIFT) && ev.key == KEY_PAGEUP) {
                spin_lock(&t->lock);
                t->scroll = (int)MIN((uint64_t)(t->scroll + t->rows / 2), screen_top(t) - MIN(screen_top(t), t->total > TLINES ? t->total - TLINES : 0));
                spin_unlock(&t->lock);
                dirty = true;
                break;
            }
            if ((ev.mods & MOD_SHIFT) && ev.key == KEY_PAGEDOWN) {
                t->scroll = MAX(0, t->scroll - t->rows / 2);
                dirty = true;
                break;
            }
            if (t->scroll) { t->scroll = 0; dirty = true; }
            if (t->has_sel) { t->has_sel = false; dirty = true; }
            tty_input(&t->tty, &ev);
            break;
        case EV_MOUSE_WHEEL: {
            spin_lock(&t->lock);
            uint64_t top = screen_top(t);
            uint64_t oldest = t->total > TLINES ? t->total - TLINES : 0;
            int maxs = (int)(top - MIN(top, oldest));
            t->scroll = CLAMP(t->scroll - ev.wheel * 3, 0, maxs);
            spin_unlock(&t->lock);
            dirty = true;
            break;
        }
        case EV_MOUSE_DOWN:
            if (ev.button == BTN_LEFT) {
                cell_at(t, ev.x, ev.y, &t->sl0, &t->sc0);
                t->sl1 = t->sl0;
                t->sc1 = t->sc0;
                t->selecting = true;
                t->has_sel = false;
                dirty = true;
            } else if (ev.button == BTN_MIDDLE || ev.button == BTN_RIGHT) {
                char *c = clipboard_get();
                if (c) { tty_paste(&t->tty, c); kfree(c); }
            }
            break;
        case EV_MOUSE_MOVE:
            if (t->selecting) {
                cell_at(t, ev.x, ev.y, &t->sl1, &t->sc1);
                dirty = true;
            }
            break;
        case EV_MOUSE_UP:
            if (t->selecting && ev.button == BTN_LEFT) {
                t->selecting = false;
                cell_at(t, ev.x, ev.y, &t->sl1, &t->sc1);
                t->has_sel = t->sl0 != t->sl1 || t->sc0 != t->sc1;
                if (t->has_sel) copy_selection(t);
                dirty = true;
            }
            break;
        }
    }
    spin_lock(&t->lock);
    t->win_gone = true;
    spin_unlock(&t->lock);
    tty_close(&t->tty);
    wm_destroy(t->win);
    tty_put(&t->tty);
    return 0;
}
