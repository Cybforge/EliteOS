/*
 * The login and lock screen: a full-screen overlay above everything. It
 * takes all keyboard and mouse input while it is up. The password check
 * runs on its own thread (PBKDF2 is meant to be slow).
 */
#include <wm.h>
#include <sched.h>
#include <sys.h>
#include "wm_internal.h"

static bool active;
static bool locking;                 /* locking a running session: only its owner can come back */
static int sel;                      /* selected account */
static char pw[64];
static int pwlen;
static char err[80];
static uint64_t shake_t0;
static volatile int busy;            /* 0 idle, 1 checking, 2 ok, 3 wrong */
static char busy_user[32], busy_pass[64];
static int hover;
static uint64_t anim_t0;

enum { H_NONE, H_SIGNIN, H_RESTART, H_POWER, H_KBD, H_USER0 = 100 };

bool login_active(void) { return active; }

static bool needs_password(void)
{
    const struct account *a = users_get(sel);
    return a && a->hash[0];
}

void login_start(bool lock)
{
    locking = lock;
    active = true;
    pw[0] = 0;
    pwlen = 0;
    err[0] = 0;
    busy = 0;
    anim_t0 = uptime_ms();
    sel = 0;
    if (lock && session.active)
        for (int i = 0; i < users_count(); i++)
            if (!strcmp(users_get(i)->name, session.user)) sel = i;
    wm_dirty(R(0, 0, wm_screen_w(), wm_screen_h()));
}

static int login_worker(void *arg)
{
    UNUSED(arg);
    bool ok = session_login(busy_user, busy_pass);
    memset(busy_pass, 0, sizeof(busy_pass));
    busy = ok ? 2 : 3;
    wm_kick();
    return 0;
}

static void submit(void)
{
    const struct account *a = users_get(sel);
    if (!a || busy) return;
    if (locking && session.active && !strcmp(session.user, a->name)) {
        /* unlocking: no new session, just check the password */
        strlcpy(busy_user, a->name, sizeof(busy_user));
    } else {
        strlcpy(busy_user, a->name, sizeof(busy_user));
    }
    strlcpy(busy_pass, pw, sizeof(busy_pass));
    err[0] = 0;
    busy = 1;
    thread_create("login", login_worker, NULL);
    wm_dirty(R(0, 0, wm_screen_w(), wm_screen_h()));
}

/* called from the compositor tick: finish a finished check */
bool login_tick(void)
{
    if (!active) return false;
    if (busy == 2) {
        busy = 0;
        active = false;
        pw[0] = 0;
        pwlen = 0;
        wm_dirty(R(0, 0, wm_screen_w(), wm_screen_h()));
        desktop_session_started();
        return true;
    }
    if (busy == 3) {
        busy = 0;
        snprintf(err, sizeof(err), "%s", tr("Wrong password"));
        shake_t0 = uptime_ms();
        pw[0] = 0;
        pwlen = 0;
        wm_dirty(R(0, 0, wm_screen_w(), wm_screen_h()));
    }
    if (shake_t0 && uptime_ms() - shake_t0 < 400) wm_dirty(R(0, 0, wm_screen_w(), wm_screen_h()));
    return busy == 1 || (shake_t0 && uptime_ms() - shake_t0 < 400);
}

static color_t avatar_color(const char *name)
{
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    return color_hsv((float)(h % 360), 0.55f, 0.85f);
}

static void avatar(surface_t *s, int cx, int cy, int r, const char *name, const char *label)
{
    gfx_circle(s, (float)cx, (float)cy, (float)r, avatar_color(name));
    char ini[8] = "?";
    const char *p = label[0] ? label : name;
    uint32_t cp = utf8_next(&p);
    ini[utf8_encode(cp >= 'a' && cp <= 'z' ? cp - 32 : cp, ini)] = 0;
    gfx_text_center(s, r > 30 ? font_huge : font_ui_bold, R(cx - r, cy - r, 2 * r, 2 * r), ini, 0xFFFFFFFFu);
}

static rect_t card_rect(void)
{
    int W = wm_screen_w(), H = wm_screen_h();
    return R(W / 2 - 190, H / 2 - 150, 380, 330);
}

static rect_t field_rect(rect_t c) { return R(c.x + 40, c.y + 190, 300, 44); }
static rect_t button_rect(rect_t c) { return R(c.x + 40, c.y + 244, 300, 44); }
static rect_t user_chip(int i, int n)
{
    int W = wm_screen_w(), H = wm_screen_h();
    return R(W / 2 - n * 60 + i * 120 + 10, H - 140, 100, 90);
}

void login_draw(surface_t *s, rect_t clip, surface_t *blur)
{
    if (!active) return;
    int W = wm_screen_w(), H = wm_screen_h();
    for (int y = clip.y; y < clip.y + clip.h; y++)
        memcpy(s->px + (size_t)y * s->stride + clip.x, blur->px + (size_t)y * blur->stride + clip.x, (size_t)clip.w * 4);
    gfx_fill_r(s, clip, ALPHA(0x050612, 120));

    struct datetime dt;
    rtc_now(&dt);
    char t[16], d[48];
    snprintf(t, sizeof(t), "%02d:%02d", dt.hour, dt.minute);
    snprintf(d, sizeof(d), "%02d.%02d.%04d", dt.day, dt.month, dt.year);
    gfx_text_center(s, font_huge, R(0, 40, W, 90), t, 0xFFFFFFFFu);
    gfx_text_center(s, font_ui_lg, R(0, 128, W, 30), d, theme.text_dim);

    rect_t c = card_rect();
    int shake = 0;
    if (shake_t0 && uptime_ms() - shake_t0 < 400) {
        int ph = (int)((uptime_ms() - shake_t0) / 40);
        shake = (ph % 2 ? 1 : -1) * (int)(14 - ph) ;
    }
    c.x += shake;
    gfx_shadow(s, c.x, c.y + 8, c.w, c.h, 18, 36, 130);
    gfx_round_rect(s, c.x, c.y, c.w, c.h, 18, ALPHA(0x14172B, 215));
    gfx_round_outline(s, c.x, c.y, c.w, c.h, 18, ALPHA(0xFFFFFF, 34));

    const struct account *a = users_get(sel);
    if (!a) {
        gfx_text_center(s, font_ui_lg, c, tr("No user accounts"), theme.text_dim);
    } else {
        avatar(s, c.x + c.w / 2, c.y + 70, 44, a->name, a->fullname);
        gfx_text_center(s, font_ui_lg, R(c.x, c.y + 124, c.w, 28), a->fullname, theme.text);
        char u[48];
        snprintf(u, sizeof(u), "@%s", a->name);
        gfx_text_center(s, font_ui, R(c.x, c.y + 152, c.w, 22), u, theme.text_faint);
        if (needs_password()) {
            rect_t f = field_rect(c);
            gfx_round_rect(s, f.x, f.y, f.w, f.h, 22, ALPHA(0xFFFFFF, 22));
            gfx_round_outline(s, f.x, f.y, f.w, f.h, 22, err[0] ? theme.danger : theme.accent);
            if (!pwlen) {
                gfx_text(s, font_ui_lg, f.x + 20, f.y + 10, tr("Password"), theme.text_faint);
            } else {
                for (int i = 0; i < pwlen && i < 24; i++)
                    gfx_circle(s, (float)(f.x + 26 + i * 12), (float)(f.y + f.h / 2), 4.0f, theme.text);
            }
            if ((uptime_ms() / 500) % 2 == 0 && !busy) {
                int cx = f.x + 20 + MIN(pwlen, 24) * 12 + (pwlen ? 0 : 0);
                gfx_fill(s, cx + (pwlen ? 0 : 0), f.y + 10, 2, f.h - 20, theme.accent);
            }
        }
        rect_t b = button_rect(c);
        if (!needs_password()) b.y -= 54;
        gfx_round_rect(s, b.x, b.y, b.w, b.h, 22, hover == H_SIGNIN ? color_lighten(theme.accent, 20) : theme.accent);
        gfx_text_center(s, font_ui_lg, b, busy == 1 ? tr("Checking...") : locking ? tr("Unlock") : tr("Sign in"), 0xFFFFFFFFu);
        if (err[0]) gfx_text_center(s, font_ui, R(c.x, c.y + 296, c.w, 22), err, theme.danger);
    }

    /* the other accounts */
    int n = users_count();
    if (n > 1) {
        for (int i = 0; i < n; i++) {
            rect_t r = user_chip(i, n);
            const struct account *u = users_get(i);
            if (i == sel || hover == H_USER0 + i) gfx_round_rect(s, r.x, r.y, r.w, r.h, 12, ALPHA(0xFFFFFF, i == sel ? 36 : 20));
            avatar(s, r.x + r.w / 2, r.y + 32, 22, u->name, u->fullname);
            gfx_text_center(s, font_ui, R(r.x, r.y + 60, r.w, 22), u->name, theme.text);
        }
    }

    /* keyboard layout and power */
    rect_t kb = R(30, H - 56, 70, 34);
    gfx_round_rect(s, kb.x, kb.y, kb.w, kb.h, 8, ALPHA(0xFFFFFF, hover == H_KBD ? 36 : 18));
    gfx_text_center(s, font_ui_md, kb, keymap_short(), theme.text);
    rect_t rs = R(W - 120, H - 60, 44, 44), pw_ = R(W - 66, H - 60, 44, 44);
    gfx_round_rect(s, rs.x, rs.y, rs.w, rs.h, 10, ALPHA(0xFFFFFF, hover == H_RESTART ? 36 : 18));
    gfx_round_rect(s, pw_.x, pw_.y, pw_.w, pw_.h, 10, ALPHA(0xFFFFFF, hover == H_POWER ? 36 : 18));
    icon_draw(s, ICON_RESTART, rs.x + 11, rs.y + 11, 22);
    icon_draw(s, ICON_POWER, pw_.x + 11, pw_.y + 11, 22);
}

bool login_mouse(int x, int y, int pressed)
{
    if (!active) return false;
    int W = wm_screen_w(), H = wm_screen_h();
    rect_t c = card_rect();
    int old = hover;
    hover = H_NONE;
    rect_t b = button_rect(c);
    if (!needs_password()) b.y -= 54;
    if (rect_has(b, x, y)) hover = H_SIGNIN;
    if (rect_has(R(30, H - 56, 70, 34), x, y)) hover = H_KBD;
    if (rect_has(R(W - 120, H - 60, 44, 44), x, y)) hover = H_RESTART;
    if (rect_has(R(W - 66, H - 60, 44, 44), x, y)) hover = H_POWER;
    int n = users_count();
    if (n > 1)
        for (int i = 0; i < n; i++)
            if (rect_has(user_chip(i, n), x, y)) hover = H_USER0 + i;
    if (hover != old) wm_dirty(R(0, 0, W, H));
    if (pressed & BTN_LEFT) {
        if (hover == H_SIGNIN) submit();
        else if (hover == H_KBD) { keymap_next(); wm_dirty(R(0, 0, W, H)); }
        else if (hover == H_RESTART) app_launch("@reboot");
        else if (hover == H_POWER) app_launch("@poweroff");
        else if (hover >= H_USER0 && !(locking && session.active)) {
            sel = hover - H_USER0;
            pw[0] = 0;
            pwlen = 0;
            err[0] = 0;
            wm_dirty(R(0, 0, W, H));
        }
    }
    return true;
}

bool login_key(int key, uint32_t ch, int mods, bool pressed)
{
    UNUSED(mods);
    if (!active) return false;
    if (!pressed || busy) return true;
    if (key == KEY_ENTER || key == KEY_KPENTER) {
        submit();
    } else if (key == KEY_BACKSPACE) {
        if (pwlen) pw[--pwlen] = 0;
        err[0] = 0;
    } else if (key == KEY_ESC) {
        pw[0] = 0;
        pwlen = 0;
    } else if (key == KEY_TAB && users_count() > 1 && !(locking && session.active)) {
        sel = (sel + 1) % users_count();
        pw[0] = 0;
        pwlen = 0;
        err[0] = 0;
    } else if (ch >= 32 && ch != 127 && pwlen < 60) {
        char enc[4];
        int n = utf8_encode(ch, enc);
        if (pwlen + n < (int)sizeof(pw) - 1) {
            memcpy(pw + pwlen, enc, (size_t)n);
            pwlen += n;
            pw[pwlen] = 0;
        }
        err[0] = 0;
    }
    wm_dirty(R(0, 0, wm_screen_w(), wm_screen_h()));
    return true;
}
