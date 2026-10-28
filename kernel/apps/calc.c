/* Calculator with scientific functions and a history tape. */
#include <wm.h>
#include <tty.h>
#include <mm.h>

struct calc {
    char expr[128];
    char result[64];
    bool error;
    bool just_evaluated;
    char hist[12][160];
    int nhist;
    int pressed_key;
    uint64_t pressed_t;
};

static const char *keys[] = {
    "sin", "cos", "tan", "π", "C",
    "√", "x²", "^", "(", ")",
    "ln", "7", "8", "9", "÷",
    "log", "4", "5", "6", "×",
    "!", "1", "2", "3", "−",
    "±", "0", ".", "⌫", "+",
};
#define NKEYS 30

static void append(struct calc *c, const char *s)
{
    if (c->just_evaluated) {
        /* continue from the result when typing an operator, else start fresh */
        bool op = !strcmp(s, "+") || !strcmp(s, "−") || !strcmp(s, "×") || !strcmp(s, "÷") || !strcmp(s, "^");
        if (op && !c->error) strlcpy(c->expr, c->result, sizeof(c->expr));
        else c->expr[0] = 0;
        c->just_evaluated = false;
    }
    strlcat(c->expr, s, sizeof(c->expr));
}

static void live_eval(struct calc *c)
{
    double v;
    const char *err;
    if (c->expr[0] && expr_eval(c->expr, &v, &err)) {
        expr_format(v, c->result, sizeof(c->result));
        c->error = false;
    } else if (!c->expr[0]) {
        strcpy(c->result, "0");
        c->error = false;
    }
}

static void evaluate(struct calc *c)
{
    double v;
    const char *err;
    if (!c->expr[0]) return;
    if (expr_eval(c->expr, &v, &err)) {
        expr_format(v, c->result, sizeof(c->result));
        c->error = false;
        if (c->nhist == 12) { memmove(c->hist[0], c->hist[1], sizeof(c->hist[0]) * 11); c->nhist--; }
        snprintf(c->hist[c->nhist++], sizeof(c->hist[0]), "%s = %s", c->expr, c->result);
    } else {
        strlcpy(c->result, err ? err : "error", sizeof(c->result));
        c->error = true;
    }
    c->just_evaluated = true;
}

static void backspace(struct calc *c)
{
    size_t l = strlen(c->expr);
    if (l) c->expr[l - utf8_prev_len(c->expr, c->expr + l)] = 0;
    c->just_evaluated = false;
}

static void press(struct calc *c, int k)
{
    const char *s = keys[k];
    if (!strcmp(s, "C")) { c->expr[0] = 0; strcpy(c->result, "0"); c->error = false; c->just_evaluated = false; return; }
    if (!strcmp(s, "⌫")) { backspace(c); live_eval(c); return; }
    if (!strcmp(s, "x²")) { append(c, "^2"); live_eval(c); return; }
    if (!strcmp(s, "±")) {
        char t[140];
        snprintf(t, sizeof(t), "-(%s)", c->just_evaluated ? c->result : c->expr);
        strlcpy(c->expr, t, sizeof(c->expr));
        c->just_evaluated = false;
        live_eval(c);
        return;
    }
    if (!strcmp(s, "sin") || !strcmp(s, "cos") || !strcmp(s, "tan") || !strcmp(s, "ln") || !strcmp(s, "log") ||
        !strcmp(s, "√")) {
        append(c, s);
        if (strcmp(s, "√")) strlcat(c->expr, "(", sizeof(c->expr));
        return;
    }
    append(c, s);
    live_eval(c);
}

static void calc_paint(app_t *a, surface_t *s)
{
    struct calc *c = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, theme.bg);
    int kw = 360;
    /* display */
    gfx_round_rect(s, 12, 12, kw - 24, 110, 14, theme.panel);
    surface_t sub = *s;
    sub.clip = rect_intersect(s->clip, R(20, 12, kw - 40, 110));
    int ew = font_text_width(font_ui_lg, c->expr);
    gfx_text(&sub, font_ui_lg, MIN(kw - 28 - ew, 26), 28, c->expr[0] ? c->expr : " ", theme.text_dim);
    int rw = font_text_width(font_light, c->result);
    gfx_text(&sub, font_light, kw - 28 - rw, 62, c->result, c->error ? theme.danger : theme.text);

    /* keys */
    int cols = 5, bw = (kw - 24 - (cols - 1) * 8) / cols, bh = (H - 140 - 12 - 5 * 8) / 6;
    for (int i = 0; i < NKEYS; i++) {
        rect_t r = R(12 + (i % cols) * (bw + 8), 134 + (i / cols) * (bh + 8), bw, bh);
        const char *k = keys[i];
        int style = BTN_SUBTLE;
        if (isdigit((unsigned char)k[0]) || !strcmp(k, ".")) style = BTN_NORMAL;
        if (!strcmp(k, "C")) style = BTN_DANGER;
        bool flash = c->pressed_key == i && uptime_ms() - c->pressed_t < 120;
        if (ui_button(u, r, k, flash ? BTN_PRIMARY : style)) press(c, i);
    }

    /* history tape */
    if (W > kw + 40) {
        int hx = kw + 4;
        gfx_fill(s, hx - 4, 0, 1, H, ALPHA(0xFFFFFF, 12));
        gfx_text(s, font_ui_bold, hx + 12, 16, "History", theme.text);
        for (int i = 0; i < c->nhist; i++) {
            int y = 46 + (c->nhist - 1 - i) * 38;
            rect_t r = R(hx + 6, y, W - hx - 16, 34);
            if (ui_list_item(u, r, false)) {
                const char *eq = strstr(c->hist[i], " = ");
                if (eq) { strlcpy(c->expr, eq + 3, sizeof(c->expr)); c->just_evaluated = false; live_eval(c); }
            }
            surface_t hs = *s;
            hs.clip = rect_intersect(s->clip, r);
            gfx_text_ellipsis(&hs, font_ui, r.x + 8, r.y + 9, r.w - 16, c->hist[i], theme.text_dim);
        }
        if (!c->nhist) gfx_text(s, font_ui, hx + 12, 46, "Results appear here.", theme.text_faint);
        if (ui_button(u, R(hx + 8, H - 52, W - hx - 20, 40), "=", BTN_PRIMARY)) evaluate(c);
    }
}

static int calc_event(app_t *a, struct gui_event *ev)
{
    struct calc *c = a->data;
    if (ev->type != EV_KEY_DOWN) return 1;
    int k = ev->key;
    if (k == KEY_ENTER || k == KEY_KPENTER || ev->ch == '=') { evaluate(c); return 1; }
    if (k == KEY_BACKSPACE) { backspace(c); live_eval(c); return 1; }
    if (k == KEY_ESC || k == KEY_DELETE) { press(c, 4); return 1; }
    uint32_t ch = ev->ch;
    if (ch == '*') { append(c, "×"); live_eval(c); return 1; }
    if (ch == '/') { append(c, "÷"); live_eval(c); return 1; }
    if (ch == '-') { append(c, "−"); live_eval(c); return 1; }
    if (ch == ',') ch = '.';
    if ((ch >= '0' && ch <= '9') || ch == '.' || ch == '+' || ch == '(' || ch == ')' || ch == '^' || ch == '!' ||
        ch == '%' || (ch >= 'a' && ch <= 'z')) {
        char t[2] = { (char)ch, 0 };
        append(c, t);
        live_eval(c);
        for (int i = 0; i < NKEYS; i++) if (!strcmp(keys[i], t)) { c->pressed_key = i; c->pressed_t = uptime_ms(); }
    }
    return 1;
}

int calc_main(void *arg)
{
    UNUSED(arg);
    struct calc *c = kzalloc(sizeof(*c));
    strcpy(c->result, "0");
    c->pressed_key = -1;
    app_t a = { 0 };
    a.data = c;
    a.win = wm_create("Calculator", 620, 520, 0);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_CALC);
    a.on_paint = calc_paint;
    a.on_event = calc_event;
    int r = app_run(&a);
    kfree(c);
    return r;
}
