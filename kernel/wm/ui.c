/*
 * Immediate-mode widgets. An app feeds events into its ui_t, then paints;
 * widgets draw themselves and report interaction in the same call.
 */
#include <wm.h>

static uint64_t wid(rect_t r) { return ((uint64_t)(uint32_t)r.x << 40) ^ ((uint64_t)(uint32_t)r.y << 20) ^ (uint64_t)(uint32_t)(r.w * 7 + r.h); }
uint64_t ui_id(rect_t r) { return wid(r); }

void ui_feed(ui_t *u, const struct gui_event *ev)
{
    switch (ev->type) {
    case EV_MOUSE_MOVE:
        u->mx = ev->x;
        u->my = ev->y;
        break;
    case EV_MOUSE_DOWN:
        u->mx = ev->x;
        u->my = ev->y;
        if (ev->button == BTN_LEFT) {
            u->mdown = true;
            u->mpressed = true;
            u->dclicked = ev->clicks >= 2;
            u->focus = 0;           /* widgets re-claim focus when clicked */
        }
        if (ev->button == BTN_RIGHT) u->rclicked = true;
        break;
    case EV_MOUSE_UP:
        u->mx = ev->x;
        u->my = ev->y;
        if (ev->button == BTN_LEFT) {
            u->mdown = false;
            u->mreleased = true;
        }
        break;
    case EV_MOUSE_WHEEL:
        u->wheel += ev->wheel;
        break;
    case EV_MOUSE_LEAVE:
        u->mx = -10000;
        u->my = -10000;
        break;
    case EV_KEY_DOWN:
        u->has_key = true;
        u->key_used = false;
        u->key = *ev;
        break;
    case EV_BLUR:
        u->mdown = false;
        break;
    }
}

void ui_frame_end(ui_t *u)
{
    u->mpressed = false;
    u->mreleased = false;
    u->rclicked = false;
    u->dclicked = false;
    u->wheel = 0;
    u->has_key = false;
    if (!u->mdown) u->active = 0;
}

bool ui_hover(ui_t *u, rect_t r)
{
    return rect_has(r, u->mx, u->my) && rect_has(u->s->clip, u->mx, u->my);
}

static bool press_logic(ui_t *u, rect_t r, uint64_t id, bool *hover_out, bool *held_out)
{
    bool hover = ui_hover(u, r);
    if (hover && u->mpressed) u->active = id;
    bool held = u->active == id && u->mdown;
    bool clicked = u->mreleased && u->active == id && hover;
    /* a click usually changes state that was already painted this frame */
    if (clicked) u->want_repaint = true;
    if (hover_out) *hover_out = hover;
    if (held_out) *held_out = held;
    return clicked;
}

bool ui_button(ui_t *u, rect_t r, const char *label, int style)
{
    bool hover, held;
    bool clicked = press_logic(u, r, wid(r), &hover, &held);
    surface_t *s = u->s;
    color_t bg, fg = theme.text;
    switch (style) {
    case BTN_PRIMARY:
        bg = held ? color_darken(theme.accent, 25) : hover ? color_lighten(theme.accent, 18) : theme.accent;
        fg = 0xFFFFFFFFu;
        break;
    case BTN_DANGER:
        bg = held ? color_darken(theme.danger, 25) : hover ? color_lighten(theme.danger, 15) : theme.danger;
        fg = 0xFFFFFFFFu;
        break;
    case BTN_FLAT:
        bg = held ? ALPHA(0xFFFFFF, 30) : hover ? ALPHA(0xFFFFFF, 18) : 0;
        break;
    case BTN_SUBTLE:
        bg = held ? theme.surface2 : hover ? theme.surface2 : theme.surface;
        break;
    default:
        bg = held ? color_darken(theme.surface2, 8) : hover ? color_lighten(theme.surface2, 10) : theme.surface2;
        break;
    }
    if (C_A(bg)) gfx_round_rect(s, r.x, r.y, r.w, r.h, 8, bg);
    if (style == BTN_NORMAL) gfx_round_outline(s, r.x, r.y, r.w, r.h, 8, ALPHA(0xFFFFFF, hover ? 34 : 18));
    gfx_text_center(s, font_ui_md, R(r.x, r.y + (held ? 1 : 0), r.w, r.h), label, fg);
    return clicked;
}

bool ui_icon_button(ui_t *u, rect_t r, int icon, const char *tip)
{
    UNUSED(tip);
    bool hover, held;
    bool clicked = press_logic(u, r, wid(r), &hover, &held);
    if (hover || held) gfx_round_rect(u->s, r.x, r.y, r.w, r.h, 8, ALPHA(0xFFFFFF, held ? 30 : 18));
    int sz = MIN(r.w, r.h) - 12;
    icon_draw(u->s, icon, r.x + (r.w - sz) / 2, r.y + (r.h - sz) / 2, sz);
    return clicked;
}

bool ui_toggle(ui_t *u, rect_t r, bool *v)
{
    rect_t sw = R(r.x + r.w - 44, r.y + (r.h - 24) / 2, 44, 24);
    bool hover;
    bool clicked = press_logic(u, r, wid(r), &hover, NULL);
    if (clicked) *v = !*v;
    color_t bg = *v ? theme.accent : (hover ? theme.surface2 : theme.surface);
    gfx_round_rect(u->s, sw.x, sw.y, sw.w, sw.h, 12, bg);
    if (!*v) gfx_round_outline(u->s, sw.x, sw.y, sw.w, sw.h, 12, ALPHA(0xFFFFFF, 50));
    float kx = *v ? (float)(sw.x + sw.w - 12) : (float)(sw.x + 12);
    gfx_circle(u->s, kx, (float)sw.y + 12.0f, 8, *v ? 0xFFFFFFFFu : theme.text_dim);
    return clicked;
}

bool ui_checkbox(ui_t *u, rect_t r, const char *label, bool *v)
{
    bool hover;
    bool clicked = press_logic(u, r, wid(r), &hover, NULL);
    if (clicked) *v = !*v;
    rect_t b = R(r.x, r.y + (r.h - 20) / 2, 20, 20);
    if (*v) {
        gfx_round_rect(u->s, b.x, b.y, b.w, b.h, 5, theme.accent);
        gfx_line_w(u->s, (float)b.x + 5, (float)b.y + 10, (float)b.x + 9, (float)b.y + 14, 2, 0xFFFFFFFFu);
        gfx_line_w(u->s, (float)b.x + 9, (float)b.y + 14, (float)b.x + 15, (float)b.y + 6, 2, 0xFFFFFFFFu);
    } else {
        gfx_round_rect(u->s, b.x, b.y, b.w, b.h, 5, hover ? theme.surface2 : theme.surface);
        gfx_round_outline(u->s, b.x, b.y, b.w, b.h, 5, ALPHA(0xFFFFFF, 60));
    }
    gfx_text(u->s, font_ui, r.x + 30, r.y + (r.h - font_height(font_ui)) / 2, label, theme.text);
    return clicked;
}

bool ui_slider(ui_t *u, rect_t r, float *v, float lo, float hi)
{
    uint64_t id = wid(r);
    bool hover, held;
    press_logic(u, r, id, &hover, &held);
    bool changed = false;
    if (held) {
        float t = (float)(u->mx - r.x - 8) / (float)MAX(1, r.w - 16);
        t = CLAMP(t, 0.0f, 1.0f);
        float nv = lo + t * (hi - lo);
        if (nv != *v) { *v = nv; changed = true; }
    }
    if (hover && u->wheel) {
        *v = CLAMP(*v - (float)u->wheel * (hi - lo) / 20.0f, lo, hi);
        changed = true;
    }
    float t = (*v - lo) / (hi - lo);
    int ty = r.y + r.h / 2;
    gfx_round_rect(u->s, r.x + 8, ty - 2, r.w - 16, 4, 2, theme.surface2);
    int fx = r.x + 8 + (int)(t * (float)(r.w - 16));
    gfx_round_rect(u->s, r.x + 8, ty - 2, fx - r.x - 8, 4, 2, theme.accent);
    gfx_circle(u->s, (float)fx, (float)ty, held ? 9 : 8, 0xFFFFFFFFu);
    gfx_circle(u->s, (float)fx, (float)ty, 4, theme.accent);
    return changed;
}

static bool textbox_impl(ui_t *u, rect_t r, char *buf, size_t cap, const char *placeholder, bool mask);

bool ui_textbox(ui_t *u, rect_t r, char *buf, size_t cap, const char *placeholder)
{
    return textbox_impl(u, r, buf, cap, placeholder, false);
}

/* like ui_textbox, but the characters are shown as dots */
bool ui_password(ui_t *u, rect_t r, char *buf, size_t cap, const char *placeholder)
{
    return textbox_impl(u, r, buf, cap, placeholder, true);
}

static bool textbox_impl(ui_t *u, rect_t r, char *buf, size_t cap, const char *placeholder, bool mask)
{
    uint64_t id = wid(r);
    bool hover = ui_hover(u, r);
    if (hover && u->mpressed) u->focus = id;
    bool focused = u->focus == id;
    bool enter = false;
    if (focused && u->has_key && !u->key_used) {
        struct gui_event *k = &u->key;
        size_t l = strlen(buf);
        if (k->key == KEY_BACKSPACE) {
            if (l) buf[l - utf8_prev_len(buf, buf + l)] = 0;
            u->key_used = true;
        } else if (k->key == KEY_ENTER || k->key == KEY_KPENTER) {
            enter = true;
            u->key_used = true;
        } else if (k->ch >= 32 && k->ch != 127 && !(k->mods & MOD_CTRL)) {
            char enc[4];
            int n = utf8_encode(k->ch, enc);
            if (l + (size_t)n < cap) {
                memcpy(buf + l, enc, (size_t)n);
                buf[l + (size_t)n] = 0;
            }
            u->key_used = true;
        }
    }
    gfx_round_rect(u->s, r.x, r.y, r.w, r.h, 8, focused ? color_darken(theme.surface, 6) : theme.surface);
    gfx_round_outline(u->s, r.x, r.y, r.w, r.h, 8, focused ? theme.accent : ALPHA(0xFFFFFF, hover ? 50 : 26));
    surface_t sub = *u->s;
    sub.clip = rect_intersect(u->s->clip, R(r.x + 8, r.y, r.w - 16, r.h));
    int ty = r.y + (r.h - font_height(font_ui_lg)) / 2;
    char dots[96];
    const char *shown = buf;
    if (mask) {
        int n = 0;
        for (const char *p = buf; *p && n < (int)sizeof(dots) - 4; ) {
            utf8_next(&p);
            dots[n++] = '*';
        }
        dots[n] = 0;
        shown = dots;
    }
    int tw = font_text_width(font_ui_lg, shown);
    int tx = r.x + 10;
    if (tw > r.w - 24) tx -= tw - (r.w - 24);
    if (buf[0]) gfx_text(&sub, font_ui_lg, tx, ty, shown, theme.text);
    else if (placeholder) gfx_text(&sub, font_ui_lg, tx, ty, placeholder, theme.text_faint);
    if (focused) gfx_fill(&sub, tx + tw + 1, ty + 1, 2, font_height(font_ui_lg) - 2, theme.accent);
    return enter;
}

bool ui_tab(ui_t *u, rect_t r, const char *label, bool active)
{
    bool hover;
    bool clicked = press_logic(u, r, wid(r), &hover, NULL);
    if (active) gfx_round_rect(u->s, r.x, r.y, r.w, r.h, 8, ALPHA(theme.accent, 60));
    else if (hover) gfx_round_rect(u->s, r.x, r.y, r.w, r.h, 8, ALPHA(0xFFFFFF, 16));
    gfx_text_center(u->s, active ? font_ui_md : font_ui, r, label, active ? theme.text : theme.text_dim);
    return clicked;
}

void ui_progress(ui_t *u, rect_t r, float v, color_t c)
{
    v = CLAMP(v, 0.0f, 1.0f);
    gfx_round_rect(u->s, r.x, r.y, r.w, r.h, r.h / 2, theme.surface2);
    int w = (int)((float)r.w * v);
    if (w > 0) gfx_round_rect(u->s, r.x, r.y, MAX(w, r.h), r.h, r.h / 2, c);
}

void ui_panel(ui_t *u, rect_t r)
{
    gfx_round_rect(u->s, r.x, r.y, r.w, r.h, 12, theme.panel);
    gfx_round_outline(u->s, r.x, r.y, r.w, r.h, 12, ALPHA(0xFFFFFF, 14));
}

void ui_label(ui_t *u, int x, int y, const char *text, font_t *f, color_t c)
{
    gfx_text(u->s, f ? f : font_ui, x, y, text, c ? c : theme.text);
}

bool ui_list_item(ui_t *u, rect_t r, bool selected)
{
    bool hover;
    bool clicked = press_logic(u, r, wid(r), &hover, NULL);
    if (selected) gfx_round_rect(u->s, r.x, r.y, r.w, r.h, 6, ALPHA(theme.accent, 80));
    else if (hover) gfx_round_rect(u->s, r.x, r.y, r.w, r.h, 6, ALPHA(0xFFFFFF, 14));
    return clicked;
}

void ui_scrollbar(ui_t *u, rect_t r, int *scroll, int content, int view)
{
    if (content <= view) { *scroll = 0; return; }
    int maxs = content - view;
    *scroll = CLAMP(*scroll, 0, maxs);
    int th = MAX(30, r.h * view / content);
    int ty = r.y + (r.h - th) * *scroll / maxs;
    uint64_t id = wid(r);
    bool hover, held;
    press_logic(u, r, id, &hover, &held);
    if (held) {
        int ny = u->my - r.y - th / 2;
        *scroll = CLAMP(ny * maxs / MAX(1, r.h - th), 0, maxs);
        ty = r.y + (r.h - th) * *scroll / maxs;
    }
    gfx_round_rect(u->s, r.x + r.w / 2 - 3, ty, 6, th, 3, ALPHA(0xFFFFFF, held ? 110 : hover ? 80 : 45));
}
