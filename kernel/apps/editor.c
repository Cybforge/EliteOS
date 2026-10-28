/*
 * Text editor: line numbers, C syntax highlighting, selection, clipboard,
 * undo, search, auto-indent.
 */
#include <wm.h>
#include <sys.h>
#include <vfs.h>
#include <mm.h>
#include <tty.h>

#define MAXLINES 20000

struct line {
    char *s;
    int len, cap;
};

struct snapshot {
    char *text;
    int cy, cx;
};

struct editor {
    struct line *lines;
    int n;
    int cy, cx;             /* cursor: line, byte offset */
    int sy, sx;             /* selection anchor (-1 = none) */
    int top, left;          /* scroll: first visible line, horizontal px */
    char path[VFS_PATH_MAX];
    bool modified;
    bool is_c;
    int bar;                /* 0 none, 1 save as, 2 open, 3 find */
    char input[VFS_PATH_MAX];
    char find[64];
    char status[96];
    uint64_t status_t;
    struct snapshot undo[24];
    int nundo;
    uint64_t last_edit_t;
    bool dragging;
    int cw, lh;
};

static void line_ins(struct line *l, int at, const char *s, int n)
{
    if (l->len + n + 1 > l->cap) {
        int cap = MAX(l->cap * 2, l->len + n + 16);
        char *ns = krealloc(l->s, (size_t)cap);
        if (!ns) return;
        l->s = ns;
        l->cap = cap;
    }
    memmove(l->s + at + n, l->s + at, (size_t)(l->len - at));
    memcpy(l->s + at, s, (size_t)n);
    l->len += n;
    l->s[l->len] = 0;
}

static void line_del(struct line *l, int at, int n)
{
    memmove(l->s + at, l->s + at + n, (size_t)(l->len - at - n));
    l->len -= n;
    l->s[l->len] = 0;
}

static void insert_line(struct editor *e, int at)
{
    if (e->n >= MAXLINES) return;
    memmove(&e->lines[at + 1], &e->lines[at], sizeof(struct line) * (size_t)(e->n - at));
    e->lines[at].s = kzalloc(16);
    e->lines[at].len = 0;
    e->lines[at].cap = 16;
    e->n++;
}

static void remove_line(struct editor *e, int at)
{
    kfree(e->lines[at].s);
    memmove(&e->lines[at], &e->lines[at + 1], sizeof(struct line) * (size_t)(e->n - at - 1));
    e->n--;
}

static void clear_all(struct editor *e)
{
    for (int i = 0; i < e->n; i++) kfree(e->lines[i].s);
    e->n = 0;
}

static void set_text(struct editor *e, const char *t)
{
    clear_all(e);
    insert_line(e, 0);
    int cur = 0;
    for (const char *p = t; *p; p++) {
        if (*p == '\n') { insert_line(e, ++cur); continue; }
        if (*p == '\r') continue;
        if (*p == '\t') { line_ins(&e->lines[cur], e->lines[cur].len, "    ", 4); continue; }
        line_ins(&e->lines[cur], e->lines[cur].len, p, 1);
    }
    e->cy = e->cx = 0;
    e->sy = -1;
    e->top = 0;
}

static char *get_text(struct editor *e, size_t *len)
{
    size_t total = 0;
    for (int i = 0; i < e->n; i++) total += (size_t)e->lines[i].len + 1;
    char *b = kmalloc(total + 1);
    size_t o = 0;
    for (int i = 0; i < e->n; i++) {
        memcpy(b + o, e->lines[i].s, (size_t)e->lines[i].len);
        o += (size_t)e->lines[i].len;
        if (i + 1 < e->n) b[o++] = '\n';
    }
    b[o] = 0;
    *len = o;
    return b;
}

static void status(struct editor *e, const char *s)
{
    strlcpy(e->status, s, sizeof(e->status));
    e->status_t = uptime_ms();
}

static void push_undo(struct editor *e, bool force)
{
    uint64_t now = uptime_ms();
    if (!force && now - e->last_edit_t < 800 && e->nundo) { e->last_edit_t = now; return; }
    e->last_edit_t = now;
    if (e->nundo == 24) { kfree(e->undo[0].text); memmove(&e->undo[0], &e->undo[1], sizeof(e->undo[0]) * 23); e->nundo--; }
    size_t l;
    e->undo[e->nundo].text = get_text(e, &l);
    e->undo[e->nundo].cy = e->cy;
    e->undo[e->nundo].cx = e->cx;
    e->nundo++;
}

static void do_undo(struct editor *e)
{
    if (!e->nundo) { status(e, "Nothing to undo"); return; }
    struct snapshot *s = &e->undo[--e->nundo];
    set_text(e, s->text);
    kfree(s->text);
    e->cy = MIN(s->cy, e->n - 1);
    e->cx = MIN(s->cx, e->lines[e->cy].len);
    e->modified = true;
}

static bool ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}

static void load(struct editor *e, const char *path)
{
    size_t n;
    char *d = vfs_read_file(path, &n);
    set_text(e, d ? d : "");
    kfree(d);
    strlcpy(e->path, path, sizeof(e->path));
    e->modified = false;
    e->is_c = ends_with(path, ".c") || ends_with(path, ".h") || ends_with(path, ".cpp") || ends_with(path, ".S");
}

static bool save(struct editor *e)
{
    if (!e->path[0]) { e->bar = 1; session_path("Documents/untitled.txt", e->input, sizeof(e->input)); return false; }
    size_t len;
    char *t = get_text(e, &len);
    int r = vfs_write_file(e->path, t, len);
    kfree(t);
    if (r) { status(e, "Could not save the file"); return false; }
    e->modified = false;
    char m[140];
    snprintf(m, sizeof(m), "Saved %s", e->path);
    status(e, m);
    return true;
}

/* ------------------------------------------------------------------------
 * selection
 * ---------------------------------------------------------------------- */

static bool has_sel(struct editor *e) { return e->sy >= 0 && (e->sy != e->cy || e->sx != e->cx); }

static void sel_range(struct editor *e, int *y0, int *x0, int *y1, int *x1)
{
    if (e->sy < e->cy || (e->sy == e->cy && e->sx < e->cx)) { *y0 = e->sy; *x0 = e->sx; *y1 = e->cy; *x1 = e->cx; }
    else { *y0 = e->cy; *x0 = e->cx; *y1 = e->sy; *x1 = e->sx; }
}

static char *sel_text(struct editor *e, size_t *len)
{
    int y0, x0, y1, x1;
    sel_range(e, &y0, &x0, &y1, &x1);
    size_t cap = 16;
    for (int y = y0; y <= y1; y++) cap += (size_t)e->lines[y].len + 1;
    char *b = kmalloc(cap);
    size_t o = 0;
    for (int y = y0; y <= y1; y++) {
        int a = y == y0 ? x0 : 0, z = y == y1 ? x1 : e->lines[y].len;
        memcpy(b + o, e->lines[y].s + a, (size_t)(z - a));
        o += (size_t)(z - a);
        if (y != y1) b[o++] = '\n';
    }
    b[o] = 0;
    *len = o;
    return b;
}

static void delete_sel(struct editor *e)
{
    int y0, x0, y1, x1;
    sel_range(e, &y0, &x0, &y1, &x1);
    struct line *a = &e->lines[y0];
    struct line *b = &e->lines[y1];
    char *tail = strdup(b->s + x1);
    int tl = b->len - x1;
    a->len = x0;
    a->s[x0] = 0;
    line_ins(a, x0, tail, tl);
    kfree(tail);
    for (int y = y1; y > y0; y--) remove_line(e, y);
    e->cy = y0;
    e->cx = x0;
    e->sy = -1;
    e->modified = true;
}

static void insert_text(struct editor *e, const char *t)
{
    if (has_sel(e)) delete_sel(e);
    e->sy = -1;
    for (const char *p = t; *p; p++) {
        if (*p == '\r') continue;
        if (*p == '\n') {
            struct line *l = &e->lines[e->cy];
            insert_line(e, e->cy + 1);
            l = &e->lines[e->cy];
            line_ins(&e->lines[e->cy + 1], 0, l->s + e->cx, l->len - e->cx);
            l->len = e->cx;
            l->s[l->len] = 0;
            e->cy++;
            e->cx = 0;
            continue;
        }
        const char *ins = *p == '\t' ? "    " : p;
        int n = *p == '\t' ? 4 : 1;
        line_ins(&e->lines[e->cy], e->cx, ins, n);
        e->cx += n;
    }
    e->modified = true;
}

/* ------------------------------------------------------------------------
 * syntax colouring for C
 * ---------------------------------------------------------------------- */

static const char *kw[] = {
    "int", "char", "void", "if", "else", "for", "while", "do", "return", "struct", "static", "const",
    "unsigned", "signed", "long", "short", "float", "double", "switch", "case", "break", "continue",
    "default", "sizeof", "typedef", "enum", "union", "volatile", "extern", "inline", "bool", "true",
    "false", "NULL", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int64_t", "size_t", "goto",
};

static bool is_kw(const char *s, int n)
{
    for (unsigned i = 0; i < ARRAY_SIZE(kw); i++)
        if ((int)strlen(kw[i]) == n && !strncmp(kw[i], s, (size_t)n)) return true;
    return false;
}

static color_t token_color(struct editor *e, struct line *l, int i, bool *in_comment, int *span)
{
    const char *s = l->s;
    *span = 1;
    if (!e->is_c) return theme.text;
    if (*in_comment) {
        if (s[i] == '*' && s[i + 1] == '/') { *span = 2; *in_comment = false; }
        return HEX(0x6A7396);
    }
    if (s[i] == '/' && s[i + 1] == '/') { *span = l->len - i; return HEX(0x6A7396); }
    if (s[i] == '/' && s[i + 1] == '*') { *in_comment = true; *span = 2; return HEX(0x6A7396); }
    if (s[i] == '#') { *span = l->len - i; return HEX(0xFF7AC6); }
    if (s[i] == '"' || s[i] == '\'') {
        char q = s[i];
        int j = i + 1;
        while (j < l->len && s[j] != q) { if (s[j] == '\\') j++; j++; }
        *span = MIN(j + 1, l->len) - i;
        return HEX(0x7EE0A8);
    }
    if (isdigit(s[i]) && (i == 0 || !isalnum(s[i - 1]))) {
        int j = i;
        while (j < l->len && (isalnum(s[j]) || s[j] == '.')) j++;
        *span = j - i;
        return HEX(0xFFB86C);
    }
    if (isalpha(s[i]) || s[i] == '_') {
        int j = i;
        while (j < l->len && (isalnum(s[j]) || s[j] == '_')) j++;
        *span = j - i;
        if (is_kw(s + i, j - i)) return HEX(0xB08CFF);
        if (j < l->len && s[j] == '(') return HEX(0x6CC4FF);
        return theme.text;
    }
    return HEX(0xC5CAE6);
}

/* ------------------------------------------------------------------------
 * view
 * ---------------------------------------------------------------------- */

#define GUTTER 56
#define TOOL_H 48
#define STATUS_H 28

static int text_x(struct editor *e, struct line *l, int cx)
{
    return font_text_width_n(font_mono, l->s, cx) - e->left + 0 * e->cw;
}

static void pos_from_point(struct editor *e, int mx, int my, int *cy, int *cx)
{
    int y = e->top + (my - TOOL_H - 6) / e->lh;
    y = CLAMP(y, 0, e->n - 1);
    struct line *l = &e->lines[y];
    int x = mx - GUTTER - 10 + e->left;
    int best = 0;
    int acc = 0;
    const char *p = l->s;
    while (*p) {
        const char *q = p;
        int w = font_char_width(font_mono, utf8_next(&q));
        if (acc + w / 2 > x) break;
        acc += w;
        p = q;
        best = (int)(p - l->s);
    }
    *cy = y;
    *cx = best;
}

static void ensure_visible(struct editor *e, int view_lines, int view_w)
{
    if (e->cy < e->top) e->top = e->cy;
    if (e->cy >= e->top + view_lines) e->top = e->cy - view_lines + 1;
    int x = font_text_width_n(font_mono, e->lines[e->cy].s, e->cx);
    if (x - e->left > view_w - 30) e->left = x - view_w + 60;
    if (x - e->left < 0) e->left = MAX(0, x - 40);
}

static void ed_paint(app_t *a, surface_t *s)
{
    struct editor *e = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, HEX(0x13162A));

    /* toolbar */
    gfx_fill(s, 0, 0, W, TOOL_H, theme.panel);
    gfx_fill(s, 0, TOOL_H - 1, W, 1, ALPHA(0xFFFFFF, 14));
    if (ui_button(u, R(10, 8, 64, 32), "New", BTN_FLAT)) {
        push_undo(e, true);
        set_text(e, "");
        e->path[0] = 0;
        e->modified = false;
    }
    if (ui_button(u, R(78, 8, 64, 32), "Open", BTN_FLAT)) { e->bar = 2; strlcpy(e->input, e->path[0] ? e->path : session_home(), sizeof(e->input)); }
    if (ui_button(u, R(146, 8, 64, 32), "Save", BTN_FLAT)) save(e);
    if (ui_button(u, R(214, 8, 80, 32), "Save as", BTN_FLAT)) { e->bar = 1; if (e->path[0]) strlcpy(e->input, e->path, sizeof(e->input)); else session_path("Documents/untitled.txt", e->input, sizeof(e->input)); }
    if (ui_button(u, R(298, 8, 64, 32), "Find", BTN_FLAT)) { e->bar = 3; }
    char title[160];
    snprintf(title, sizeof(title), "%s%s", e->path[0] ? vfs_basename(e->path) : "Untitled", e->modified ? "  ●" : "");
    gfx_text_right(s, font_ui_md, W - 16, 16, title, e->modified ? theme.warning : theme.text_dim);

    int bar_h = e->bar ? 48 : 0;
    int view_top = TOOL_H + 6;
    int view_h = H - TOOL_H - STATUS_H - bar_h - 6;
    int view_lines = MAX(1, view_h / e->lh);
    int view_w = W - GUTTER - 20;

    if (u->wheel && ui_hover(u, R(0, TOOL_H, W, view_h))) {
        e->top = CLAMP(e->top - u->wheel * 3, 0, MAX(0, e->n - view_lines));
    }

    /* gutter */
    gfx_fill(s, 0, TOOL_H, GUTTER, view_h + 6, HEX(0x101325));
    surface_t sub = *s;
    sub.clip = rect_intersect(s->clip, R(GUTTER + 10, TOOL_H, W - GUTTER - 10, view_h + 6));
    int sy0 = 0, sx0 = 0, sy1 = -1, sx1 = 0;
    if (has_sel(e)) sel_range(e, &sy0, &sx0, &sy1, &sx1);
    bool in_comment = false;
    /* comment state from lines above the view (cheap scan) */
    if (e->is_c)
        for (int y = MAX(0, e->top - 200); y < e->top; y++) {
            struct line *l = &e->lines[y];
            for (int i = 0; i + 1 < l->len; i++) {
                if (!in_comment && l->s[i] == '/' && l->s[i + 1] == '/') break;
                if (!in_comment && l->s[i] == '/' && l->s[i + 1] == '*') { in_comment = true; i++; }
                else if (in_comment && l->s[i] == '*' && l->s[i + 1] == '/') { in_comment = false; i++; }
            }
        }
    for (int r = 0; r < view_lines && e->top + r < e->n; r++) {
        int y = e->top + r;
        struct line *l = &e->lines[y];
        int py = view_top + r * e->lh;
        if (y == e->cy) gfx_fill(s, GUTTER, py - 1, W - GUTTER, e->lh, ALPHA(0xFFFFFF, 8));
        char num[12];
        snprintf(num, sizeof(num), "%d", y + 1);
        gfx_text_right(s, font_mono, GUTTER - 10, py, num, y == e->cy ? theme.text : HEX(0x4A5170));
        /* selection */
        if (y >= sy0 && y <= sy1) {
            int a0 = y == sy0 ? sx0 : 0, a1 = y == sy1 ? sx1 : l->len;
            int x0 = GUTTER + 10 + text_x(e, l, a0), x1 = GUTTER + 10 + text_x(e, l, a1) + (y != sy1 ? e->cw / 2 : 0);
            gfx_fill(&sub, x0, py - 1, x1 - x0, e->lh, ALPHA(theme.accent, 110));
        }
        /* find highlight */
        if (e->find[0]) {
            const char *m = l->s;
            while ((m = strstr_ci(m, e->find))) {
                int mx0 = GUTTER + 10 + text_x(e, l, (int)(m - l->s));
                int mx1 = GUTTER + 10 + text_x(e, l, (int)(m - l->s) + (int)strlen(e->find));
                gfx_round_rect(&sub, mx0, py - 1, mx1 - mx0, e->lh, 3, ALPHA(theme.warning, 70));
                m += strlen(e->find);
            }
        }
        /* text */
        int x = GUTTER + 10 - e->left;
        for (int i = 0; i < l->len;) {
            int span;
            color_t c = token_color(e, l, i, &in_comment, &span);
            span = MAX(1, MIN(span, l->len - i));
            x = gfx_text_n(&sub, font_mono, x, py, l->s + i, span, c);
            i += span;
        }
    }
    /* cursor */
    if (e->cy >= e->top && e->cy < e->top + view_lines && !e->bar) {
        int cx = GUTTER + 10 + text_x(e, &e->lines[e->cy], e->cx);
        gfx_fill(&sub, cx, view_top + (e->cy - e->top) * e->lh - 1, 2, e->lh, theme.accent2);
    }
    /* scrollbar */
    int sc = e->top;
    ui_scrollbar(u, R(W - 12, TOOL_H + 4, 10, view_h), &sc, e->n + view_lines - 1, view_lines);
    e->top = sc;

    /* input bar */
    if (e->bar) {
        int by = H - STATUS_H - bar_h;
        gfx_fill(s, 0, by, W, bar_h, theme.panel);
        gfx_fill(s, 0, by, W, 1, ALPHA(0xFFFFFF, 14));
        const char *lbl = e->bar == 1 ? "Save as" : e->bar == 2 ? "Open" : "Find";
        gfx_text(s, font_ui_md, 16, by + 16, lbl, theme.text_dim);
        rect_t ir = R(80, by + 7, W - 290, 34);
        u->focus = ui_id(ir);
        char *buf = e->bar == 3 ? e->find : e->input;
        size_t cap = e->bar == 3 ? sizeof(e->find) : sizeof(e->input);
        bool enter = ui_textbox(u, ir, buf, cap, NULL);
        if (ui_button(u, R(W - 200, by + 7, 90, 34), "Cancel", BTN_NORMAL)) { e->bar = 0; e->find[0] = 0; }
        const char *ok = e->bar == 3 ? "Next" : e->bar == 1 ? "Save" : "Open";
        if (ui_button(u, R(W - 104, by + 7, 90, 34), ok, BTN_PRIMARY) || enter) {
            if (e->bar == 1) {
                strlcpy(e->path, e->input, sizeof(e->path));
                if (save(e)) e->bar = 0;
                e->is_c = ends_with(e->path, ".c") || ends_with(e->path, ".h");
            } else if (e->bar == 2) {
                struct vfs_stat st;
                if (!vfs_stat(e->input, &st) && st.type == VN_FILE) {
                    load(e, e->input);
                    e->bar = 0;
                } else status(e, "No such file");
            } else if (e->find[0]) {
                /* next match after the cursor */
                bool found = false;
                for (int k = 0; k <= e->n && !found; k++) {
                    int y = (e->cy + k) % e->n;
                    const char *st = e->lines[y].s + (k == 0 ? MIN(e->cx + 1, e->lines[y].len) : 0);
                    const char *m = strstr_ci(st, e->find);
                    if (m) {
                        e->cy = y;
                        e->sy = y;
                        e->sx = (int)(m - e->lines[y].s);
                        e->cx = e->sx + (int)strlen(e->find);
                        found = true;
                    }
                }
                if (!found) status(e, "Not found");
                ensure_visible(e, view_lines, view_w);
            }
        }
    }

    /* status bar */
    int sy = H - STATUS_H;
    gfx_fill(s, 0, sy, W, STATUS_H, theme.panel);
    char st[160];
    if (e->status[0] && uptime_ms() - e->status_t < 3000) strlcpy(st, e->status, sizeof(st));
    else snprintf(st, sizeof(st), "%s", e->path[0] ? e->path : "Not saved yet");
    gfx_text(s, font_ui, 12, sy + 6, st, theme.text_dim);
    int col = 1;
    for (int i = 0; i < e->cx; i++) if (((uint8_t)e->lines[e->cy].s[i] & 0xC0) != 0x80) col++;
    snprintf(st, sizeof(st), "Ln %d, Col %d   ·   %d lines   ·   %s   ·   UTF-8", e->cy + 1, col, e->n,
             e->is_c ? "C" : "Plain text");
    gfx_text_right(s, font_ui, W - 14, sy + 6, st, theme.text_faint);

    /* mouse in the text area */
    rect_t text_area = R(GUTTER, TOOL_H, W - GUTTER - 14, view_h + 6);
    if (u->mpressed && ui_hover(u, text_area) && !e->bar) {
        pos_from_point(e, u->mx, u->my, &e->cy, &e->cx);
        e->sy = e->cy;
        e->sx = e->cx;
        e->dragging = true;
        if (u->dclicked) {
            /* select word */
            struct line *l = &e->lines[e->cy];
            int a0 = e->cx, a1 = e->cx;
            while (a0 > 0 && (isalnum(l->s[a0 - 1]) || l->s[a0 - 1] == '_')) a0--;
            while (a1 < l->len && (isalnum(l->s[a1]) || l->s[a1] == '_')) a1++;
            e->sx = a0;
            e->cx = a1;
            e->dragging = false;
        }
        u->want_repaint = true;
    } else if (e->dragging && u->mdown) {
        pos_from_point(e, u->mx, u->my, &e->cy, &e->cx);
    }
    if (!u->mdown) e->dragging = false;
}

static void move_word(struct editor *e, int dir)
{
    struct line *l = &e->lines[e->cy];
    if (dir < 0) {
        if (e->cx == 0 && e->cy > 0) { e->cy--; e->cx = e->lines[e->cy].len; return; }
        while (e->cx > 0 && !isalnum(l->s[e->cx - 1])) e->cx--;
        while (e->cx > 0 && isalnum(l->s[e->cx - 1])) e->cx--;
    } else {
        if (e->cx == l->len && e->cy + 1 < e->n) { e->cy++; e->cx = 0; return; }
        while (e->cx < l->len && !isalnum(l->s[e->cx])) e->cx++;
        while (e->cx < l->len && isalnum(l->s[e->cx])) e->cx++;
    }
}

static int ed_event(app_t *a, struct gui_event *ev)
{
    struct editor *e = a->data;
    if (ev->type != EV_KEY_DOWN) return ev->type != EV_TIMER;
    if (e->bar) {
        if (ev->key == KEY_ESC) { e->bar = 0; e->find[0] = 0; }
        return 1;
    }
    bool ctrl = ev->mods & MOD_CTRL, shift = ev->mods & MOD_SHIFT;
    int view_lines = MAX(1, (a->win->ch - TOOL_H - STATUS_H - 6) / e->lh);
    struct line *l = &e->lines[e->cy];
    bool nav = true;
    int oy = e->cy, ox = e->cx;
    switch (ev->key) {
    case KEY_LEFT:
        if (ctrl) move_word(e, -1);
        else if (e->cx > 0) e->cx -= utf8_prev_len(l->s, l->s + e->cx);
        else if (e->cy > 0) { e->cy--; e->cx = e->lines[e->cy].len; }
        break;
    case KEY_RIGHT:
        if (ctrl) move_word(e, 1);
        else if (e->cx < l->len) { const char *p = l->s + e->cx; utf8_next(&p); e->cx = (int)(p - l->s); }
        else if (e->cy + 1 < e->n) { e->cy++; e->cx = 0; }
        break;
    case KEY_UP: if (e->cy > 0) e->cy--; break;
    case KEY_DOWN: if (e->cy + 1 < e->n) e->cy++; break;
    case KEY_PAGEUP: e->cy = MAX(0, e->cy - view_lines); break;
    case KEY_PAGEDOWN: e->cy = MIN(e->n - 1, e->cy + view_lines); break;
    case KEY_HOME:
        if (ctrl) e->cy = 0;
        { int ind = 0; while (ind < e->lines[e->cy].len && e->lines[e->cy].s[ind] == ' ') ind++; e->cx = e->cx == ind ? 0 : ind; }
        break;
    case KEY_END: if (ctrl) e->cy = e->n - 1; e->cx = e->lines[e->cy].len; break;
    default: nav = false;
    }
    if (nav) {
        if (e->cy != oy) {
            /* keep the byte offset sane on the new line */
            e->cx = MIN(ox, e->lines[e->cy].len);
            if (ev->key == KEY_HOME || ev->key == KEY_END) e->cx = ev->key == KEY_END ? e->lines[e->cy].len : e->cx;
        }
        if (shift) { if (e->sy < 0) { e->sy = oy; e->sx = ox; } }
        else e->sy = -1;
        ensure_visible(e, view_lines, a->win->cw - GUTTER - 20);
        return 1;
    }
    if (ctrl) {
        size_t n;
        switch (ev->key) {
        case KEY_S: save(e); break;
        case KEY_O: e->bar = 2; strlcpy(e->input, e->path[0] ? e->path : session_home(), sizeof(e->input)); break;
        case KEY_F: e->bar = 3; break;
        case KEY_Z: do_undo(e); break;
        case KEY_A: e->sy = 0; e->sx = 0; e->cy = e->n - 1; e->cx = e->lines[e->cy].len; break;
        case KEY_C: case KEY_X:
            if (has_sel(e)) {
                char *t = sel_text(e, &n);
                clipboard_set(t, n);
                kfree(t);
                if (ev->key == KEY_X) { push_undo(e, true); delete_sel(e); }
            }
            break;
        case KEY_V: {
            char *c = clipboard_get();
            if (c) { push_undo(e, true); insert_text(e, c); kfree(c); }
            break;
        }
        }
        ensure_visible(e, view_lines, a->win->cw - GUTTER - 20);
        return 1;
    }
    if (ev->key == KEY_ESC) { e->sy = -1; return 1; }
    push_undo(e, false);
    if (ev->key == KEY_BACKSPACE) {
        if (has_sel(e)) delete_sel(e);
        else if (e->cx > 0) {
            int n = utf8_prev_len(l->s, l->s + e->cx);
            /* delete a whole indent step */
            if (n == 1 && e->cx >= 4 && !strncmp(l->s + e->cx - 4, "    ", 4)) {
                bool all_space = true;
                for (int i = 0; i < e->cx; i++) if (l->s[i] != ' ') all_space = false;
                if (all_space) n = 4;
            }
            line_del(l, e->cx - n, n);
            e->cx -= n;
        } else if (e->cy > 0) {
            struct line *p = &e->lines[e->cy - 1];
            int pl = p->len;
            line_ins(p, pl, l->s, l->len);
            remove_line(e, e->cy);
            e->cy--;
            e->cx = pl;
        }
        e->modified = true;
    } else if (ev->key == KEY_DELETE) {
        if (has_sel(e)) delete_sel(e);
        else if (e->cx < l->len) { const char *p = l->s + e->cx; utf8_next(&p); line_del(l, e->cx, (int)(p - (l->s + e->cx))); }
        else if (e->cy + 1 < e->n) { struct line *nx = &e->lines[e->cy + 1]; line_ins(l, l->len, nx->s, nx->len); remove_line(e, e->cy + 1); }
        e->modified = true;
    } else if (ev->key == KEY_ENTER || ev->key == KEY_KPENTER) {
        int ind = 0;
        while (ind < l->len && l->s[ind] == ' ') ind++;
        bool brace = e->cx > 0 && l->s[e->cx - 1] == '{';
        insert_text(e, "\n");
        char sp[64];
        int n = MIN(ind + (brace ? 4 : 0), 60);
        memset(sp, ' ', (size_t)n);
        sp[n] = 0;
        insert_text(e, sp);
    } else if (ev->key == KEY_TAB) {
        insert_text(e, "    ");
    } else if (ev->ch >= 32) {
        char enc[5] = { 0 };
        utf8_encode(ev->ch, enc);
        insert_text(e, enc);
    }
    ensure_visible(e, view_lines, a->win->cw - GUTTER - 20);
    return 1;
}

static void ed_close(app_t *a)
{
    struct editor *e = a->data;
    if (e->modified && e->path[0]) save(e);
    a->quit = true;
}

int editor_main(void *arg)
{
    struct editor *e = kzalloc(sizeof(*e));
    e->lines = kmalloc(sizeof(struct line) * MAXLINES);
    e->sy = -1;
    e->cw = font_char_width(font_mono, 'M');
    e->lh = font_height(font_mono) + 3;
    if (arg) {
        load(e, arg);
        kfree(arg);
    } else {
        set_text(e, "");
    }
    app_t a = { 0 };
    a.data = e;
    char title[96];
    snprintf(title, sizeof(title), "%s - Text Editor", e->path[0] ? vfs_basename(e->path) : "Untitled");
    a.win = wm_create(title, 860, 600, WF_RESIZABLE);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_EDITOR);
    wm_set_min_size(a.win, 480, 300);
    a.on_paint = ed_paint;
    a.on_event = ed_event;
    a.on_close = ed_close;
    int r = app_run(&a);
    clear_all(e);
    for (int i = 0; i < e->nundo; i++) kfree(e->undo[i].text);
    kfree(e->lines);
    kfree(e);
    return r;
}
