/*
 * Settings: personalization, language and region, users, network, display,
 * sound, apps and system. Everything that is changed is stored: per-user
 * choices in the user's settings file, system-wide ones in
 * /etc/zenith/system.conf.
 */
#include <wm.h>
#include <mm.h>
#include <sched.h>
#include <vfs.h>
#include <cpu.h>
#include <net.h>
#include <audio.h>
#include <sys.h>
#include <bootinfo.h>
#include "../wm/wm_internal.h"

extern struct bootinfo *boot_info;
void wallpaper_thumb(surface_t *s, int style);

enum { P_PERSONAL, P_REGION, P_USERS, P_NETWORK, P_DISPLAY, P_SOUND, P_APPS, P_SYSTEM, P_COUNT };

struct settings {
    int page;
    surface_t *thumbs[8];
    /* region */
    struct lang_info langs[16];
    int nlangs;
    struct kbd_info kbds[16];
    int nkbds;
    char tz_filter[32];
    int tz_scroll;
    /* users */
    int user_sel;
    int ustate;                          /* 0 list, 1 add, 2 change password */
    char u_name[32], u_full[48], u_pass[40], u_pass2[40], u_old[40];
    bool u_admin;
    char u_msg[96];
    bool u_msg_ok;
    /* system */
    char host[64];
    bool host_init;
    char host_msg[64];
    /* scrolling of long pages */
    int scroll;
    int pscroll;                         /* vertical scroll of the whole page */
    int content_h;
    bool tz_init;
};

static void section(surface_t *s, int x, int y, const char *title, const char *sub)
{
    gfx_text(s, font_bold_lg, x, y, tr(title), theme.text);
    if (sub) gfx_text(s, font_ui, x, y + 30, tr(sub), theme.text_dim);
}

static void note(surface_t *s, int x, int y, const char *text, color_t c)
{
    gfx_text(s, font_ui, x, y, text, c);
}

/* ---------------------------------------------------------------- personalization */

static int page_personal(struct settings *st, ui_t *u, surface_t *s, int x, int y)
{
    section(s, x, y, N_("Wallpaper"), N_("Generated live by the compositor"));
    y += 64;
    int tw = 150, th = 94;
    for (int i = 0; i < wallpaper_count; i++) {
        rect_t r = R(x + (i % 3) * (tw + 16), y + (i / 3) * (th + 36), tw, th);
        if (!st->thumbs[i]) {
            st->thumbs[i] = surface_new(tw, th);
            wallpaper_thumb(st->thumbs[i], i);
        }
        gfx_blit(s, r.x, r.y, st->thumbs[i], 0, 0, tw, th);
        bool sel = wallpaper_style == i;
        gfx_round_outline(s, r.x - 2, r.y - 2, r.w + 4, r.h + 4, 8, sel ? theme.accent : ALPHA(0xFFFFFF, ui_hover(u, r) ? 60 : 20));
        if (sel) gfx_round_outline(s, r.x - 3, r.y - 3, r.w + 6, r.h + 6, 9, theme.accent);
        gfx_text(s, font_ui, r.x, r.y + th + 8, tr(wallpaper_names[i]), sel ? theme.text : theme.text_dim);
        if (ui_hover(u, r) && u->mreleased && !sel) {
            wallpaper_style = i;
            app_launch("@wallpaper");
        }
    }
    y += 2 * (th + 36) + 10;
    section(s, x, y, N_("Accent colour"), NULL);
    y += 40;
    for (int i = 0; i < accent_count; i++) {
        rect_t r = R(x + i * 52, y, 40, 40);
        gfx_circle(s, (float)r.x + 20, (float)r.y + 20, 18, accent_choices[i]);
        if (accent_index == i) gfx_ring(s, (float)r.x + 20, (float)r.y + 20, 22, 2.5f, theme.text);
        if (ui_hover(u, r) && u->mreleased) {
            theme_set_accent(i);
            wm_invalidate_all();
        }
    }
    y += 64;
    bool tp = ui_transparency, an = ui_animations;
    gfx_text(s, font_ui_md, x, y + 8, tr("Transparency effects"), theme.text);
    if (ui_toggle(u, R(x + 300, y, 60, 36), &tp)) { ui_transparency = tp; wm_invalidate_all(); }
    y += 44;
    gfx_text(s, font_ui_md, x, y + 8, tr("Animations"), theme.text);
    if (ui_toggle(u, R(x + 300, y, 60, 36), &an)) ui_animations = an;
    return y;
}

/* ---------------------------------------------------------------- language and region */

static void apply_language(const char *id)
{
    if (!i18n_set(id)) return;
    conf_set(&sysconf, "language", id);
    sys_save_conf();
    launchers_reload();
    wm_invalidate_all();
}

static void apply_keyboard(const char *id)
{
    if (!keymap_set(id)) return;
    conf_set(&sysconf, "keyboard", id);
    sys_save_conf();
    wm_invalidate_all();
}

static void apply_zone(const char *name)
{
    if (!tz_set(name)) return;
    conf_set(&sysconf, "timezone", name);
    sys_save_conf();
    wm_invalidate_all();
}

static int page_region(struct settings *st, ui_t *u, surface_t *s, int x, int y, int W, int H)
{
    section(s, x, y, N_("Language & region"), N_("Language, keyboard, time zone and clock"));
    y += 70;
    int colw = (W - x - 40) / 2;

    /* language */
    gfx_text(s, font_ui_bold, x, y, tr("Language"), theme.text);
    if (!st->nlangs) st->nlangs = i18n_languages(st->langs, 16);
    for (int i = 0; i < st->nlangs; i++) {
        rect_t r = R(x, y + 26 + i * 36, colw - 10, 32);
        if (ui_list_item(u, r, !strcmp(st->langs[i].id, i18n_current()))) apply_language(st->langs[i].id);
        gfx_text(s, font_ui_md, r.x + 12, r.y + 7, st->langs[i].name, theme.text);
    }
    int ly = y + 26 + st->nlangs * 36;

    /* keyboard */
    int kx = x + colw;
    gfx_text(s, font_ui_bold, kx, y, tr("Keyboard layout"), theme.text);
    if (!st->nkbds) st->nkbds = keymap_layouts(st->kbds, 16);
    for (int i = 0; i < st->nkbds; i++) {
        rect_t r = R(kx, y + 26 + i * 36, colw - 10, 32);
        if (ui_list_item(u, r, !strcmp(st->kbds[i].id, keymap_current()))) apply_keyboard(st->kbds[i].id);
        gfx_text(s, font_ui_md, r.x + 12, r.y + 7, st->kbds[i].name, theme.text);
    }
    int ky = y + 26 + st->nkbds * 36;
    note(s, kx, ky + 4, tr("Switch layouts with Win+Space or from the taskbar"), theme.text_faint);
    y = MAX(ly, ky + 30) + 16;

    /* time zone */
    gfx_text(s, font_ui_bold, x, y, tr("Time zone"), theme.text);
    char cur[96];
    snprintf(cur, sizeof(cur), "%s", tz_name());
    {
        bool dst;
        const char *abbr;
        int off = tz_offset(rtc_epoch(), &dst, &abbr);
        char b[64];
        snprintf(b, sizeof(b), "%s  UTC%+d:%02d%s", abbr, off / 3600, (off < 0 ? -off : off) / 60 % 60, dst ? "  (DST)" : "");
        gfx_text_right(s, font_ui, W - 40, y + 2, b, theme.text_dim);
    }
    y += 28;
    ui_textbox(u, R(x, y, colw - 10, 36), st->tz_filter, sizeof(st->tz_filter), tr("Search a city or country"));
    const struct tz_zone *zones;
    int nz = tz_zones(&zones);
    if (!st->tz_init) {
        st->tz_init = true;
        for (int i = 0; i < nz; i++)
            if (!strcmp(zones[i].name, tz_name())) st->tz_scroll = MAX(0, i - 2);
    }
    int rows = 6, shown = 0, first = st->tz_scroll;
    int matches = 0;
    for (int i = 0; i < nz; i++) {
        if (st->tz_filter[0] && !strstr_ci(zones[i].name, st->tz_filter) && !strstr_ci(zones[i].comment, st->tz_filter) &&
            !strstr_ci(zones[i].cc, st->tz_filter))
            continue;
        if (matches++ < first || shown >= rows) continue;
        rect_t r = R(x, y + 44 + shown * 34, colw - 10, 32);
        if (ui_list_item(u, r, !strcmp(zones[i].name, tz_name()))) apply_zone(zones[i].name);
        char label[96];
        snprintf(label, sizeof(label), "%s", zones[i].name);
        for (char *c = label; *c; c++) if (*c == '_') *c = ' ';
        gfx_text(s, font_ui, r.x + 12, r.y + 8, label, theme.text);
        if (zones[i].comment[0]) gfx_text_right(s, font_ui, r.x + r.w - 10, r.y + 8, zones[i].comment, theme.text_faint);
        shown++;
    }
    rect_t list = R(x, y + 44, colw - 10, rows * 34);
    if (ui_hover(u, list) && u->wheel) {
        st->tz_scroll = CLAMP(st->tz_scroll + (u->wheel > 0 ? 2 : -2), 0, MAX(0, matches - rows));
        u->wheel = 0;                                  /* the list took the wheel, the page must not scroll too */
    }
    if (st->tz_scroll > MAX(0, matches - rows)) st->tz_scroll = MAX(0, matches - rows);
    ui_scrollbar(u, R(x + colw - 8, y + 44, 8, rows * 34), &st->tz_scroll, matches, rows);

    /* clock */
    int cx = x + colw;
    struct datetime dt;
    rtc_now(&dt);
    char b[64], d[32];
    format_date(&dt, d, sizeof(d));
    snprintf(b, sizeof(b), "%02d:%02d:%02d", dt.hour, dt.minute, dt.second);
    gfx_text(s, font_huge, cx, y - 8, b, theme.text);
    gfx_text(s, font_ui_lg, cx, y + 56, d, theme.text_dim);
    int yy = y + 96;
    struct ntp_status ns;
    ntp_status(&ns);
    bool auto_time = ns.enabled;
    gfx_text(s, font_ui_md, cx, yy + 8, tr("Set the time automatically"), theme.text);
    if (ui_toggle(u, R(cx + colw - 80, yy, 60, 36), &auto_time)) {
        conf_set_int(&sysconf, "ntp", auto_time);
        sys_save_conf();
        if (auto_time) ntp_sync_now();
    }
    yy += 42;
    char sync[128];
    if (!ns.enabled) snprintf(sync, sizeof(sync), "%s", tr("Network time is off"));
    else if (ns.synced) snprintf(sync, sizeof(sync), "%s %s (%+ld s)", tr("Synchronised with"), ns.server, (long)ns.last_offset);
    else if (ns.error[0]) snprintf(sync, sizeof(sync), "%s", ns.error);
    else snprintf(sync, sizeof(sync), "%s %s ...", tr("Waiting to contact"), ns.server);
    gfx_text_ellipsis(s, font_ui, cx, yy, colw - 20, sync, ns.error[0] && !ns.synced ? theme.warning : theme.text_dim);
    yy += 30;
    if (ui_button(u, R(cx, yy, 140, 34), tr("Sync now"), BTN_NORMAL)) ntp_sync_now();
    yy += 50;
    bool local = rtc_is_local;
    gfx_text(s, font_ui_md, cx, yy + 8, tr("Hardware clock keeps local time"), theme.text);
    if (ui_toggle(u, R(cx + colw - 80, yy, 60, 36), &local)) {
        rtc_is_local = local;
        conf_set_int(&sysconf, "rtc_local", local);
        sys_save_conf();
        rtc_apply_zone();
    }
    note(s, cx, yy + 42, tr("Switch this on when Windows is installed next to ZenithOS."), theme.text_faint);
    (void)H;
    return y + 44 + rows * 34;
}

/* ---------------------------------------------------------------- users */

static void reset_user_form(struct settings *st)
{
    st->u_name[0] = st->u_full[0] = st->u_pass[0] = st->u_pass2[0] = st->u_old[0] = 0;
    st->u_admin = false;
    st->u_msg[0] = 0;
}

static int page_users(struct settings *st, ui_t *u, surface_t *s, int x, int y, int W)
{
    section(s, x, y, N_("Users"), N_("Accounts on this computer"));
    y += 66;
    int n = users_count();
    int lw = 330;
    if (st->user_sel >= n) st->user_sel = MAX(0, n - 1);
    for (int i = 0; i < n; i++) {
        const struct account *a = users_get(i);
        rect_t r = R(x, y + i * 54, lw, 50);
        if (ui_list_item(u, r, st->user_sel == i)) { st->user_sel = i; st->ustate = 0; st->u_msg[0] = 0; }
        gfx_circle(s, (float)r.x + 26, (float)r.y + 25, 18, color_hsv((float)((unsigned char)a->name[0] * 37 % 360), 0.55f, 0.85f));
        char ini[8] = "?";
        const char *p = a->fullname[0] ? a->fullname : a->name;
        uint32_t cp = utf8_next(&p);
        ini[utf8_encode(cp >= 'a' && cp <= 'z' ? cp - 32 : cp, ini)] = 0;
        gfx_text_center(s, font_ui_bold, R(r.x + 8, r.y + 7, 36, 36), ini, 0xFFFFFFFFu);
        gfx_text(s, font_ui_md, r.x + 52, r.y + 6, a->fullname, theme.text);
        char sub[80];
        snprintf(sub, sizeof(sub), "@%s  ·  %s%s", a->name, a->admin ? tr("Administrator") : tr("Standard user"),
                 session.active && !strcmp(a->name, session.user) ? tr("  ·  you") : "");
        gfx_text(s, font_ui, r.x + 52, r.y + 26, sub, theme.text_dim);
    }
    if (ui_button(u, R(x, y + n * 54 + 8, lw, 38), tr("Add a user"), BTN_PRIMARY)) {
        st->ustate = 1;
        reset_user_form(st);
    }

    int fx = x + lw + 30, fw = W - fx - 30;
    const struct account *sel = users_get(st->user_sel);
    if (st->ustate == 1) {
        gfx_text(s, font_ui_bold, fx, y, tr("New user"), theme.text);
        ui_textbox(u, R(fx, y + 28, fw, 38), st->u_full, sizeof(st->u_full), tr("Full name"));
        ui_textbox(u, R(fx, y + 74, fw, 38), st->u_name, sizeof(st->u_name), tr("User name (lower case letters, digits)"));
        ui_password(u, R(fx, y + 120, fw, 38), st->u_pass, sizeof(st->u_pass), tr("Password"));
        ui_password(u, R(fx, y + 166, fw, 38), st->u_pass2, sizeof(st->u_pass2), tr("Repeat the password"));
        gfx_text(s, font_ui_md, fx, y + 220, tr("Administrator"), theme.text);
        ui_toggle(u, R(fx + fw - 70, y + 212, 60, 36), &st->u_admin);
        if (ui_button(u, R(fx, y + 264, 160, 38), tr("Create"), BTN_PRIMARY)) {
            if (!users_valid_name(st->u_name)) {
                snprintf(st->u_msg, sizeof(st->u_msg), "%s", tr("The user name may only use a-z, 0-9, - and _"));
                st->u_msg_ok = false;
            } else if (!st->u_pass[0]) {
                snprintf(st->u_msg, sizeof(st->u_msg), "%s", tr("Choose a password"));
                st->u_msg_ok = false;
            } else if (strcmp(st->u_pass, st->u_pass2)) {
                snprintf(st->u_msg, sizeof(st->u_msg), "%s", tr("The passwords are not the same"));
                st->u_msg_ok = false;
            } else {
                int r = users_add(st->u_name, st->u_full, st->u_pass, st->u_admin);
                if (r == E_EXIST) snprintf(st->u_msg, sizeof(st->u_msg), "%s", tr("That user name is taken"));
                else if (r) snprintf(st->u_msg, sizeof(st->u_msg), "%s", tr("Could not save the account"));
                st->u_msg_ok = !r;
                if (!r) {
                    char home[96];
                    session_create_home(st->u_name, home, sizeof(home));
                    st->ustate = 0;
                    st->user_sel = users_count() - 1;
                    snprintf(st->u_msg, sizeof(st->u_msg), "%s", tr("User created"));
                    reset_user_form(st);
                    snprintf(st->u_msg, sizeof(st->u_msg), "%s", tr("User created"));
                    st->u_msg_ok = true;
                }
            }
        }
        if (ui_button(u, R(fx + 170, y + 264, 120, 38), tr("Cancel"), BTN_NORMAL)) st->ustate = 0;
    } else if (sel) {
        bool me = session.active && !strcmp(sel->name, session.user);
        gfx_text(s, font_ui_bold, fx, y, sel->fullname, theme.text);
        char l[96];
        snprintf(l, sizeof(l), "@%s", sel->name);
        gfx_text(s, font_ui, fx, y + 26, l, theme.text_dim);
        const char *homedir = session_home();
        char h[VFS_PATH_MAX];
        if (me) snprintf(h, sizeof(h), "%s", homedir);
        else snprintf(h, sizeof(h), "%s/home/%s", sys_persist_root() ? sys_persist_root() : "", sel->name);
        gfx_text_ellipsis(s, font_ui, fx, y + 50, fw, h, theme.text_faint);
        snprintf(l, sizeof(l), "%s", sel->hash[0] ? tr("Protected by a password") : tr("No password: anyone can sign in"));
        gfx_text(s, font_ui, fx, y + 74, l, sel->hash[0] ? theme.success : theme.warning);
        if (st->ustate == 2) {
            int yy = y + 110;
            gfx_text(s, font_ui_bold, fx, yy, tr("Change the password"), theme.text);
            if (me && sel->hash[0]) {
                ui_password(u, R(fx, yy + 28, fw, 38), st->u_old, sizeof(st->u_old), tr("Current password"));
                yy += 46;
            }
            ui_password(u, R(fx, yy + 28, fw, 38), st->u_pass, sizeof(st->u_pass), tr("New password (empty = none)"));
            ui_password(u, R(fx, yy + 74, fw, 38), st->u_pass2, sizeof(st->u_pass2), tr("Repeat the password"));
            if (ui_button(u, R(fx, yy + 124, 160, 38), tr("Save"), BTN_PRIMARY)) {
                if (strcmp(st->u_pass, st->u_pass2)) {
                    snprintf(st->u_msg, sizeof(st->u_msg), "%s", tr("The passwords are not the same"));
                    st->u_msg_ok = false;
                } else if (me && sel->hash[0] && !users_verify(sel->name, st->u_old)) {
                    snprintf(st->u_msg, sizeof(st->u_msg), "%s", tr("The current password is wrong"));
                    st->u_msg_ok = false;
                } else {
                    int r = users_set_password(sel->name, st->u_pass);
                    snprintf(st->u_msg, sizeof(st->u_msg), "%s", r ? tr("Could not save the account") : tr("Password changed"));
                    st->u_msg_ok = !r;
                    if (!r) { st->ustate = 0; reset_user_form(st); snprintf(st->u_msg, sizeof(st->u_msg), "%s", tr("Password changed")); st->u_msg_ok = true; }
                }
            }
            if (ui_button(u, R(fx + 170, yy + 124, 120, 38), tr("Cancel"), BTN_NORMAL)) { st->ustate = 0; reset_user_form(st); }
        } else {
            int yy = y + 110;
            if (ui_button(u, R(fx, yy, 220, 38), tr("Change the password"), BTN_NORMAL)) { st->ustate = 2; reset_user_form(st); }
            if (!me && session.admin && ui_button(u, R(fx, yy + 50, 220, 38), tr("Remove this account"), BTN_DANGER)) {
                users_remove(sel->name);
                snprintf(st->u_msg, sizeof(st->u_msg), "%s", tr("Account removed (its files stay in the home folder)"));
                st->u_msg_ok = true;
            }
        }
    }
    if (st->u_msg[0]) gfx_text_ellipsis(s, font_ui, fx, y + 330, fw, st->u_msg, st->u_msg_ok ? theme.success : theme.danger);
    return y + 340;
}

/* ---------------------------------------------------------------- network */

static int page_network(struct settings *st, ui_t *u, surface_t *s, int x, int y, int W)
{
    section(s, x, y, N_("Network"), NULL);
    y += 50;
    struct net_info ni;
    if (!net_get_info(&ni)) {
        gfx_text(s, font_ui_lg, x, y, tr("No supported network adapter found."), theme.text_dim);
        gfx_text(s, font_ui, x, y + 26, tr("ZenithOS drives Intel, Realtek, AMD, NE2000, Tulip, virtio, VMware and USB adapters"), theme.text_faint);
        gfx_text(s, font_ui, x, y + 46, tr("(type netcards in the Terminal). No Wi-Fi yet: plug in a phone and turn on USB tethering."), theme.text_faint);
        return y;
    }
    char ip[20], gw[20], dns[20], mask[20];
    ip_to_str(ni.ip, ip); ip_to_str(ni.gateway, gw); ip_to_str(ni.dns, dns); ip_to_str(ni.netmask, mask);
    const char *k[] = { N_("Adapter"), N_("MAC address"), N_("Link"), N_("IP address"), N_("Subnet mask"), N_("Gateway"), N_("DNS") };
    char mac[24];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
    const char *vals[] = { ni.driver, mac, ni.link ? tr("Connected") : tr("Disconnected"), ni.ip ? ip : tr("Not configured"), mask, gw, dns };
    for (int i = 0; i < 7; i++) {
        gfx_text(s, font_ui, x, y + i * 28, tr(k[i]), theme.text_dim);
        gfx_text(s, font_ui_md, x + 160, y + i * 28, vals[i], theme.text);
    }
    y += 7 * 28 + 4;
    char b[96];
    snprintf(b, sizeof(b), "%s: %s %lu, %s %lu", ni.ifname, tr("received"), ni.rx_packets, tr("sent"), ni.tx_packets);
    gfx_text(s, font_ui, x, y, b, theme.text_faint);
    if (ui_button(u, R(x, y + 28, 200, 38), tr("Renew address (DHCP)"), BTN_PRIMARY)) app_launch("netinfo");
    y += 86;
    int n = net_iface_count();
    if (n > 1) {
        gfx_text(s, font_ui_md, x, y, tr("All adapters"), theme.text);
        y += 30;
        for (int i = 0; i < n; i++) {
            struct net_info a;
            if (!net_iface_get(i, &a)) continue;
            char line[128], aip[20];
            ip_to_str(a.ip, aip);
            snprintf(line, sizeof(line), "%s  %s", a.ifname, a.driver);
            gfx_text(s, font_ui, x, y, line, a.link ? theme.text : theme.text_faint);
            snprintf(line, sizeof(line), "%s%s", !a.link ? tr("disconnected") : a.ip ? aip : tr("configuring..."),
                     a.is_default ? tr("  (default)") : "");
            gfx_text(s, font_ui, MIN(x + 440, W - 260), y, line, a.link ? theme.text_dim : theme.text_faint);
            y += 24;
        }
    }
    (void)st;
    return y;
}

/* ---------------------------------------------------------------- display */

static int page_display(struct settings *st, ui_t *u, surface_t *s, int x, int y, int W)
{
    (void)st; (void)u; (void)W;
    section(s, x, y, N_("Display"), NULL);
    y += 50;
    char b[128];
    snprintf(b, sizeof(b), "%d x %d  (%s)", wm_screen_w(), wm_screen_h(), boot_info->firmware_vendor[0] ? "UEFI GOP framebuffer" : "framebuffer");
    gfx_text(s, font_ui, x, y, tr("Current mode"), theme.text_dim);
    gfx_text(s, font_ui_md, x + 160, y, b, theme.text);
    y += 40;
    gfx_text(s, font_ui_bold, x, y, tr("Graphics adapters"), theme.text);
    y += 30;
    int found = 0;
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *p = pci_get(i);
        if (p->cls != 0x03) continue;
        found++;
        const char *vn = pci_vendor_name(p->vendor), *dn = pci_device_name(p->vendor, p->device);
        snprintf(b, sizeof(b), "%s %s", vn, dn ? dn : "");
        gfx_text(s, font_ui_md, x, y, b, theme.text);
        snprintf(b, sizeof(b), "PCI %04x:%04x at %02x:%02x.%x", p->vendor, p->device, p->bus, p->dev, p->fn);
        gfx_text(s, font_ui, x, y + 22, b, theme.text_faint);
        y += 52;
    }
    if (!found) gfx_text(s, font_ui, x, y, tr("No graphics adapter found on the PCI bus."), theme.text_dim);
    return y;
}

/* ---------------------------------------------------------------- sound */

static int page_sound(struct settings *st, ui_t *u, surface_t *s, int x, int y)
{
    (void)st;
    section(s, x, y, N_("Sound"), audio_available() ? hda_name() : N_("No sound card found (ZenithOS drives Intel HD Audio)"));
    y += 74;
    if (!audio_available()) return y;
    gfx_text(s, font_ui_md, x, y + 8, tr("Volume"), theme.text);
    float v = (float)audio_volume();
    if (ui_slider(u, R(x + 120, y + 6, 300, 24), &v, 0, 100)) audio_set_volume((int)(v + 0.5f));
    char b[16];
    snprintf(b, sizeof(b), "%d%%", audio_volume());
    gfx_text(s, font_ui_md, x + 440, y + 8, b, theme.text_dim);
    y += 48;
    bool m = audio_muted();
    gfx_text(s, font_ui_md, x, y + 8, tr("Mute"), theme.text);
    if (ui_toggle(u, R(x + 360, y, 60, 36), &m)) audio_set_muted(m);
    y += 60;
    gfx_text(s, font_ui_bold, x, y, tr("Try it"), theme.text);
    y += 28;
    static const struct { const char *n; int snd; } tests[] = {
        { N_("Startup"), SND_STARTUP }, { N_("Notification"), SND_NOTIFY }, { N_("Success"), SND_SUCCESS }, { N_("Error"), SND_ERROR },
    };
    for (int i = 0; i < 4; i++)
        if (ui_button(u, R(x + i * 128, y, 120, 36), tr(tests[i].n), BTN_NORMAL)) audio_sound(tests[i].snd);
    y += 56;
    if (ui_button(u, R(x, y, 200, 38), tr("Open Piano"), BTN_PRIMARY)) app_launch("piano");
    u->want_repaint = u->want_repaint || audio_level() > 0.01f;
    return y;
}

/* ---------------------------------------------------------------- apps */

static int page_apps(struct settings *st, ui_t *u, surface_t *s, int x, int y, int W, int H)
{
    section(s, x, y, N_("Apps"), N_("Everything in the start menu"));
    y += 66;
    if (ui_button(u, R(x, y, 200, 38), tr("Open the App Store"), BTN_PRIMARY)) app_launch("store");
    y += 56;
    int n = launchers_count();
    int rows = MAX(1, (H - y - 20) / 34);
    if (u->wheel) st->scroll = CLAMP(st->scroll + (u->wheel > 0 ? 2 : -2), 0, MAX(0, n - rows));
    for (int i = st->scroll; i < n && i < st->scroll + rows; i++) {
        const struct launcher *l = launcher_get(i);
        int yy = y + (i - st->scroll) * 34;
        icon_draw(s, l->icon, x, yy, 28);
        gfx_text(s, font_ui_md, x + 40, yy + 1, tr(l->name), theme.text);
        gfx_text_right(s, font_ui, W - 40, yy + 5, l->builtin ? tr("built in") : l->exec, theme.text_faint);
    }
    ui_scrollbar(u, R(W - 20, y, 8, rows * 34), &st->scroll, n, rows);
    return y;
}

/* ---------------------------------------------------------------- system */

static int page_system(struct settings *st, ui_t *u, surface_t *s, int x, int y, int W)
{
    section(s, x, y, N_("System"), NULL);
    y += 50;
    char b[160];
    logo_draw(s, x, y, 72, false);
    gfx_text(s, font_title, x + 90, y + 8, "ZenithOS " ZENITH_VERSION " \"" ZENITH_CODENAME "\"", theme.text);
    snprintf(b, sizeof(b), "%s  ·  %d %s  ·  %lu MiB", cpu_info.brand, ncpus, tr("cores"), (pmm_total_pages() * 4096) >> 20);
    gfx_text_ellipsis(s, font_ui, x + 90, y + 38, W - x - 120, b, theme.text_dim);
    y += 100;

    if (!st->host_init) {
        strlcpy(st->host, sys_hostname(), sizeof(st->host));
        st->host_init = true;
    }
    gfx_text(s, font_ui_bold, x, y, tr("Computer name"), theme.text);
    bool enter = ui_textbox(u, R(x, y + 28, 300, 38), st->host, sizeof(st->host), tr("host name"));
    if (ui_button(u, R(x + 310, y + 28, 110, 38), tr("Apply"), BTN_NORMAL) || enter) {
        int r = sys_set_hostname(st->host);
        snprintf(st->host_msg, sizeof(st->host_msg), "%s", r ? tr("Use letters, digits and - only") : tr("Saved. It is used for new network connections."));
        if (r) strlcpy(st->host, sys_hostname(), sizeof(st->host));
    }
    if (st->host_msg[0]) note(s, x, y + 74, st->host_msg, theme.text_dim);
    y += 110;

    snprintf(b, sizeof(b), "%s: %s", tr("Storage"), sys_persistent() ? tr("installed system, files are kept") : tr("live session, nothing is kept after a restart"));
    gfx_text(s, font_ui, x, y, b, sys_persistent() ? theme.success : theme.warning);
    y += 40;
    if (ui_button(u, R(x, y, 150, 40), tr("About"), BTN_NORMAL)) app_launch("about");
    if (ui_button(u, R(x + 160, y, 150, 40), tr("System Log"), BTN_NORMAL)) app_launch("logs");
    if (!sys_persistent() && ui_button(u, R(x + 320, y, 200, 40), tr("Install ZenithOS"), BTN_PRIMARY)) app_launch("installer");
    y += 56;
    if (ui_button(u, R(x, y, 150, 40), tr("Sign out"), BTN_NORMAL)) desktop_sign_out();
    if (ui_button(u, R(x + 160, y, 150, 40), tr("Restart"), BTN_NORMAL)) app_launch("@reboot");
    if (ui_button(u, R(x + 320, y, 150, 40), tr("Shut down"), BTN_DANGER)) app_launch("@poweroff");
    return y;
}

/* ---------------------------------------------------------------- frame */

static void settings_paint(app_t *a, surface_t *s)
{
    struct settings *st = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, theme.bg);
    int sbw = 220;
    gfx_fill(s, 0, 0, sbw, H, theme.panel);
    gfx_fill(s, sbw, 0, 1, H, ALPHA(0xFFFFFF, 12));
    static const struct { const char *n; int icon; } pages[P_COUNT] = {
        { N_("Personalization"), ICON_PAINT }, { N_("Language & region"), ICON_CLOCK }, { N_("Users"), ICON_USER },
        { N_("Network"), ICON_NETWORK }, { N_("Display"), ICON_MONITOR }, { N_("Sound"), ICON_SOUND },
        { N_("Apps"), ICON_STORE }, { N_("System"), ICON_INFO },
    };
    gfx_text(s, font_title, 20, 18, tr("Settings"), theme.text);
    for (int i = 0; i < P_COUNT; i++) {
        rect_t r = R(10, 62 + i * 42, sbw - 20, 38);
        if (ui_list_item(u, r, st->page == i)) { st->page = i; st->scroll = 0; st->pscroll = 0; }
        icon_draw(s, pages[i].icon, r.x + 10, r.y + 8, 22);
        gfx_text(s, font_ui_md, r.x + 44, r.y + 11, tr(pages[i].n), theme.text);
    }
    int x = sbw + 32, y0 = 24, y = y0 - st->pscroll, end = y;
    switch (st->page) {
    case P_PERSONAL: end = page_personal(st, u, s, x, y); break;
    case P_REGION: end = page_region(st, u, s, x, y, W, H); break;
    case P_USERS: end = page_users(st, u, s, x, y, W); break;
    case P_NETWORK: end = page_network(st, u, s, x, y, W); break;
    case P_DISPLAY: end = page_display(st, u, s, x, y, W); break;
    case P_SOUND: end = page_sound(st, u, s, x, y); break;
    case P_APPS: end = page_apps(st, u, s, x, y, W, H); st->pscroll = 0; break;
    case P_SYSTEM: end = page_system(st, u, s, x, y, W); break;
    }
    /* pages taller than the window scroll with the wheel */
    st->content_h = end - y + 60;
    int maxs = MAX(0, st->content_h - (H - y0));
    if (u->wheel && st->page != P_APPS) st->pscroll = CLAMP(st->pscroll + (u->wheel > 0 ? 40 : -40), 0, maxs);
    if (st->pscroll > maxs) st->pscroll = maxs;
}

static int settings_event(app_t *a, struct gui_event *ev)
{
    struct settings *st = a->data;
    if (ev->type == EV_TIMER) return st->page == P_REGION || st->page == P_NETWORK || st->page == P_SYSTEM;
    return 1;
}

int settings_main(void *arg)
{
    UNUSED(arg);
    struct settings *st = kzalloc(sizeof(*st));
    app_t a = { 0 };
    a.data = st;
    a.win = wm_create(tr("Settings"), 980, 680, WF_RESIZABLE);
    if (!a.win) return 1;
    wm_set_min_size(a.win, 760, 520);
    wm_set_icon(a.win, ICON_SETTINGS);
    wm_set_timer(a.win, 1000);
    a.on_paint = settings_paint;
    a.on_event = settings_event;
    int r = app_run(&a);
    for (int i = 0; i < 8; i++) surface_free(st->thumbs[i]);
    kfree(st);
    return r;
}
