/* Clock: analogue + digital clock, stopwatch, countdown timer, calendar. */
#include <wm.h>
#include <mm.h>

struct clockapp {
    int tab;
    bool sw_running;
    uint64_t sw_start, sw_acc;
    uint64_t laps[8];
    int nlaps;
    bool tm_running;
    uint64_t tm_end, tm_total;
    int tm_min;
    bool tm_done;
    int cal_off;        /* months from now */
};

static const char *months[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                "September", "October", "November", "December" };
static const char *wdays[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

static void analog(surface_t *s, float cx, float cy, float r, struct datetime *dt, uint64_t ms)
{
    gfx_circle(s, cx, cy + 6, r + 6, ALPHA(0x000000, 70));
    gfx_circle(s, cx, cy, r, HEX(0x1C2038));
    gfx_ring(s, cx, cy, r - 1, 2, ALPHA(0xFFFFFF, 30));
    for (int i = 0; i < 60; i++) {
        float a = (float)i / 60.0f * 2.0f * (float)K_PI;
        float sn = (float)k_sin(a), cs = (float)k_cos(a);
        bool big = i % 5 == 0;
        float r0 = r - (big ? 18 : 9), r1 = r - 6;
        gfx_line_w(s, cx + sn * r0, cy - cs * r0, cx + sn * r1, cy - cs * r1, big ? 3.0f : 1.2f,
                   big ? theme.text : ALPHA(0xFFFFFF, 90));
    }
    float sec = (float)dt->second + (float)(ms % 1000) / 1000.0f;
    float mn = (float)dt->minute + sec / 60.0f;
    float hr = (float)(dt->hour % 12) + mn / 60.0f;
    float ah = hr / 12.0f * 2 * (float)K_PI, am = mn / 60.0f * 2 * (float)K_PI, as = sec / 60.0f * 2 * (float)K_PI;
    gfx_line_w(s, cx, cy, cx + (float)k_sin(ah) * r * 0.5f, cy - (float)k_cos(ah) * r * 0.5f, 7, theme.text);
    gfx_line_w(s, cx, cy, cx + (float)k_sin(am) * r * 0.75f, cy - (float)k_cos(am) * r * 0.75f, 4.5f, theme.text);
    gfx_line_w(s, cx - (float)k_sin(as) * r * 0.15f, cy + (float)k_cos(as) * r * 0.15f, cx + (float)k_sin(as) * r * 0.85f,
               cy - (float)k_cos(as) * r * 0.85f, 2, theme.accent);
    gfx_circle(s, cx, cy, 7, theme.accent);
    gfx_circle(s, cx, cy, 3, HEX(0x1C2038));
}

static void fmt_dur(uint64_t ms, char *b, size_t n, bool cs)
{
    uint64_t s = ms / 1000;
    if (cs) snprintf(b, n, "%02lu:%02lu.%02lu", s / 60, s % 60, (ms % 1000) / 10);
    else snprintf(b, n, "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
}

static void clock_paint(app_t *a, surface_t *s)
{
    struct clockapp *c = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, theme.bg);
    static const char *tabs[] = { "Clock", "Stopwatch", "Timer", "Calendar" };
    for (int i = 0; i < 4; i++)
        if (ui_tab(u, R(16 + i * 116, 12, 110, 32), tabs[i], c->tab == i)) c->tab = i;
    gfx_fill(s, 0, 54, W, 1, ALPHA(0xFFFFFF, 12));
    struct datetime dt;
    rtc_now(&dt);
    uint64_t now = uptime_ms();
    char b[64];
    int top = 60;
    switch (c->tab) {
    case 0: {
        float r = (float)MIN(W / 2 - 40, H - top - 150) / 2.0f + 30;
        r = MIN(r, 150.0f);
        analog(s, (float)W / 2, (float)top + 30 + r, r, &dt, now);
        snprintf(b, sizeof(b), "%02d:%02d:%02d", dt.hour, dt.minute, dt.second);
        gfx_text_center(s, font_huge, R(0, top + 40 + (int)(2 * r), W, 80), b, theme.text);
        snprintf(b, sizeof(b), "%s, %d %s %d", wdays[dt.weekday], dt.day, months[dt.month - 1], dt.year);
        gfx_text_center(s, font_ui_lg, R(0, top + 118 + (int)(2 * r), W, 24), b, theme.text_dim);
        break;
    }
    case 1: {
        uint64_t el = c->sw_acc + (c->sw_running ? now - c->sw_start : 0);
        fmt_dur(el, b, sizeof(b), true);
        gfx_text_center(s, font_huge, R(0, top + 50, W, 90), b, theme.text);
        int bx = W / 2 - 170;
        if (ui_button(u, R(bx, top + 170, 160, 44), c->sw_running ? "Stop" : "Start", c->sw_running ? BTN_DANGER : BTN_PRIMARY)) {
            if (c->sw_running) c->sw_acc += now - c->sw_start;
            else c->sw_start = now;
            c->sw_running = !c->sw_running;
        }
        if (ui_button(u, R(bx + 180, top + 170, 160, 44), c->sw_running ? "Lap" : "Reset", BTN_NORMAL)) {
            if (c->sw_running) {
                if (c->nlaps == 8) { memmove(c->laps, c->laps + 1, sizeof(c->laps[0]) * 7); c->nlaps--; }
                c->laps[c->nlaps++] = el;
            } else { c->sw_acc = 0; c->nlaps = 0; }
        }
        for (int i = 0; i < c->nlaps; i++) {
            char l[48], d[24];
            fmt_dur(c->laps[i], d, sizeof(d), true);
            snprintf(l, sizeof(l), "Lap %d", i + 1);
            int y = top + 236 + (c->nlaps - 1 - i) * 28;
            gfx_text(s, font_ui, W / 2 - 140, y, l, theme.text_dim);
            gfx_text_right(s, font_ui_md, W / 2 + 140, y, d, theme.text);
        }
        break;
    }
    case 2: {
        uint64_t left = 0;
        if (c->tm_running) {
            left = c->tm_end > now ? c->tm_end - now : 0;
            if (!left && !c->tm_done) {
                c->tm_done = true;
                c->tm_running = false;
                wm_notify("Timer finished", "Your countdown is over.", ICON_CLOCK);
            }
        } else {
            left = (uint64_t)c->tm_min * 60000;
        }
        float frac = c->tm_running && c->tm_total ? (float)left / (float)c->tm_total : 1.0f;
        float cx = (float)W / 2, cy = (float)top + 150, r = 110;
        gfx_ring(s, cx, cy, r, 10, theme.surface2);
        gfx_arc(s, cx, cy, r, 10, 0, frac * 2 * (float)K_PI, c->tm_done ? theme.success : theme.accent);
        fmt_dur(left + 999, b, sizeof(b), false);
        gfx_text_center(s, font_light, R(0, (int)cy - 20, W, 40), b + 3, theme.text);
        int by = (int)(cy + r + 30);
        if (!c->tm_running) {
            static const int presets[] = { 1, 3, 5, 10, 25 };
            for (int i = 0; i < 5; i++) {
                char p[16];
                snprintf(p, sizeof(p), "%d min", presets[i]);
                if (ui_button(u, R(W / 2 - 250 + i * 102, by, 94, 36), p, c->tm_min == presets[i] ? BTN_PRIMARY : BTN_SUBTLE))
                    c->tm_min = presets[i];
            }
            if (ui_button(u, R(W / 2 - 80, by + 50, 160, 42), "Start", BTN_PRIMARY)) {
                c->tm_total = (uint64_t)c->tm_min * 60000;
                c->tm_end = now + c->tm_total;
                c->tm_running = true;
                c->tm_done = false;
            }
        } else if (ui_button(u, R(W / 2 - 80, by + 20, 160, 42), "Cancel", BTN_DANGER)) {
            c->tm_running = false;
        }
        break;
    }
    case 3: {
        int y = dt.year, m = dt.month + c->cal_off;
        while (m > 12) { m -= 12; y++; }
        while (m < 1) { m += 12; y--; }
        if (ui_button(u, R(24, top + 12, 40, 34), "‹", BTN_SUBTLE)) c->cal_off--;
        if (ui_button(u, R(W - 64, top + 12, 40, 34), "›", BTN_SUBTLE)) c->cal_off++;
        snprintf(b, sizeof(b), "%s %d", months[m - 1], y);
        gfx_text_center(s, font_light, R(0, top + 8, W, 40), b, theme.text);
        static const int md[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        int days = md[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0));
        /* Zeller-style weekday of the 1st (0 = Monday) */
        int yy = y, mm = m;
        if (mm < 3) { mm += 12; yy--; }
        int h = (1 + 13 * (mm + 1) / 5 + yy + yy / 4 - yy / 100 + yy / 400) % 7;   /* 0 = Saturday */
        int first = (h + 5) % 7;
        int cw = (W - 48) / 7, chh = (H - top - 110) / 6;
        static const char *wd[] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
        for (int i = 0; i < 7; i++)
            gfx_text_center(s, font_ui_md, R(24 + i * cw, top + 62, cw, 20), wd[i], i >= 5 ? theme.accent2 : theme.text_dim);
        for (int d = 1; d <= days; d++) {
            int idx = first + d - 1;
            rect_t r = R(24 + (idx % 7) * cw, top + 90 + (idx / 7) * chh, cw - 4, chh - 4);
            bool today = c->cal_off == 0 && d == dt.day;
            if (today) gfx_round_rect(s, r.x, r.y, r.w, r.h, 10, theme.accent);
            else if (ui_hover(u, r)) gfx_round_rect(s, r.x, r.y, r.w, r.h, 10, ALPHA(0xFFFFFF, 16));
            char n[4];
            snprintf(n, sizeof(n), "%d", d);
            gfx_text_center(s, font_ui_bold, r, n, today ? 0xFFFFFFFFu : (idx % 7 >= 5 ? theme.accent2 : theme.text));
        }
        break;
    }
    }
}

static int clock_event(app_t *a, struct gui_event *ev)
{
    struct clockapp *c = a->data;
    if (ev->type == EV_TIMER) return c->tab != 3;
    return 1;
}

int clock_main(void *arg)
{
    UNUSED(arg);
    struct clockapp *c = kzalloc(sizeof(*c));
    c->tm_min = 5;
    app_t a = { 0 };
    a.data = c;
    a.win = wm_create("Clock", 560, 560, 0);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_CLOCK);
    wm_set_timer(a.win, 50);
    a.on_paint = clock_paint;
    a.on_event = clock_event;
    int r = app_run(&a);
    kfree(c);
    return r;
}
