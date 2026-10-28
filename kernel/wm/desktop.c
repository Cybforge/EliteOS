/*
 * Desktop shell: wallpaper, desktop icons, taskbar with system tray,
 * start menu with search, notifications, Alt+Tab switcher, context menu.
 * All functions run on the compositor thread with wm_mtx held.
 */
#include "wm_internal.h"
#include <audio.h>
#include <dev.h>
#include <mm.h>
#include <cpu.h>
#include <sched.h>
#include <net.h>
#include <sys.h>

#define TB_H 52

static int SW, SH;
static surface_t *wall, *wall_blur;
static int gen_style = -1;              /* the wallpaper style that wall holds */
int wallpaper_style;
const char *wallpaper_names[] = { N_("Aurora"), N_("Nebula"), N_("Sunset"), N_("Ocean"), N_("Graphite") };
int wallpaper_count = 5;

/* ------------------------------------------------------------------------
 * wallpaper generation (integer math, it runs once per change)
 * ---------------------------------------------------------------------- */

static uint8_t lut_up[1024], lut_dn[1024];

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void add_px(uint32_t *p, int r, int g, int b)
{
    uint32_t c = *p;
    *p = RGB(clamp255((int)C_R(c) + r), clamp255((int)C_G(c) + g), clamp255((int)C_B(c) + b));
}

static void base_gradient(surface_t *s, uint32_t top, uint32_t mid, uint32_t bot)
{
    for (int y = 0; y < s->h; y++) {
        int t = y * 512 / s->h;
        color_t c = t < 256 ? color_lerp(HEX(top), HEX(mid), t) : color_lerp(HEX(mid), HEX(bot), t - 256);
        memset32(s->px + (size_t)y * s->stride, c, (size_t)s->w);
    }
}

static void glow(surface_t *s, int cx, int cy, int radius, int r, int g, int b)
{
    int x0 = MAX(0, cx - radius), x1 = MIN(s->w, cx + radius);
    int y0 = MAX(0, cy - radius), y1 = MIN(s->h, cy + radius);
    int64_t r2 = (int64_t)radius * radius;
    for (int y = y0; y < y1; y++) {
        uint32_t *line = s->px + (size_t)y * s->stride;
        int64_t dy = y - cy;
        for (int x = x0; x < x1; x++) {
            int64_t dx = x - cx;
            int64_t d2 = dx * dx + dy * dy;
            if (d2 >= r2) continue;
            int t = (int)(256 - d2 * 256 / r2);    /* 0..256 */
            t = t * t / 256;
            t = t * t / 256;                       /* soft falloff */
            add_px(&line[x], r * t / 256, g * t / 256, b * t / 256);
        }
    }
}

static void stars(surface_t *s, int count, int max_y)
{
    for (int i = 0; i < count; i++) {
        int x = (int)(krand() % (uint32_t)s->w), y = (int)(krand() % (uint32_t)max_y);
        int b = 60 + (int)(krand() % 170);
        int big = krand() % 23 == 0;
        uint32_t *p = &s->px[(size_t)y * s->stride + x];
        add_px(p, b, b, b);
        if (big) {
            int h = b / 3;
            if (x > 0) add_px(p - 1, h, h, h);
            if (x < s->w - 1) add_px(p + 1, h, h, h);
            if (y > 0) add_px(p - s->stride, h, h, h);
            if (y < s->h - 1) add_px(p + s->stride, h, h, h);
        }
    }
}

struct ribbon {
    float y, amp, freq, phase, amp2, freq2, phase2;
    int th;
    int r, g, b;
};

static void aurora(surface_t *s, const struct ribbon *rb, int n)
{
    int W_ = s->w, H_ = s->h;
    int *cy = kmalloc((size_t)W_ * sizeof(int));
    int *cm = kmalloc((size_t)W_ * sizeof(int));
    for (int i = 0; i < n; i++) {
        const struct ribbon *r = &rb[i];
        for (int x = 0; x < W_; x++) {
            float fx = (float)x / (float)W_;
            cy[x] = (int)(r->y * (float)H_ + r->amp * (float)H_ * (float)k_sin(fx * r->freq + r->phase) +
                          r->amp2 * (float)H_ * (float)k_sin(fx * r->freq2 + r->phase2));
            /* curtain rays + fade towards the edges */
            float ray = 0.70f + 0.30f * (float)k_sin((double)x * 0.045 + r->phase * 3) *
                                (float)k_sin((double)x * 0.013 + r->phase);
            float edge = (float)k_sin((double)fx * K_PI);
            cm[x] = (int)(ray * (0.35f + 0.65f * edge) * 256.0f);
        }
        int th = r->th;
        for (int x = 0; x < W_; x++) {
            int c = cy[x];
            int top = MAX(0, c - th * 4), bot = MIN(H_, c + th);
            for (int y = top; y < bot; y++) {
                int d = y - c;
                int g;
                if (d < 0) g = lut_up[MIN(1023, (-d) * 256 / (th * 4) * 4)];
                else g = lut_dn[MIN(1023, d * 1024 / th)];
                g = g * cm[x] / 256;
                add_px(&s->px[(size_t)y * s->stride + x], r->r * g / 255, r->g * g / 255, r->b * g / 255);
            }
        }
    }
    kfree(cy);
    kfree(cm);
}

static void mountains(surface_t *s, float base, float amp, float f1, float f2, float ph, uint32_t top, uint32_t bot)
{
    for (int x = 0; x < s->w; x++) {
        float fx = (float)x / (float)s->w;
        float h = base + amp * ((float)k_sin(fx * f1 + ph) * 0.6f + (float)k_sin(fx * f2 + ph * 2.3f) * 0.3f +
                                (float)k_sin(fx * f2 * 3.1f + ph) * 0.1f);
        int y0 = (int)((1.0f - h) * (float)s->h);
        for (int y = MAX(0, y0); y < s->h; y++) {
            int t = (y - y0) * 256 / MAX(1, s->h - y0);
            s->px[(size_t)y * s->stride + x] = color_lerp(HEX(top), HEX(bot), t);
        }
    }
}

static void dither(surface_t *s)
{
    uint32_t seed = 12345;
    for (int y = 0; y < s->h; y++) {
        uint32_t *line = s->px + (size_t)y * s->stride;
        for (int x = 0; x < s->w; x++) {
            seed = seed * 1103515245u + 12345u;
            int n = (int)((seed >> 16) % 3) - 1;
            if (n) add_px(&line[x], n, n, n);
        }
    }
}

static void gen_wallpaper(surface_t *s, int style)
{
    ksrand(0xC0FFEE + (uint64_t)style);
    int W_ = s->w, H_ = s->h;
    switch (style) {
    default:
    case 0: {   /* Aurora */
        base_gradient(s, 0x050816, 0x0B1030, 0x1D0F3F);
        stars(s, W_ * H_ / 2600, H_ * 3 / 4);
        glow(s, W_ / 2, H_ + H_ / 6, W_ * 2 / 3, 70, 20, 110);
        struct ribbon rb[] = {
            { 0.52f, 0.06f, 5.0f, 0.4f, 0.03f, 11.0f, 1.2f, H_ / 16, 30, 230, 170 },
            { 0.44f, 0.08f, 3.2f, 2.1f, 0.02f, 9.0f, 0.3f, H_ / 22, 60, 170, 255 },
            { 0.60f, 0.05f, 4.1f, 4.0f, 0.03f, 13.0f, 2.2f, H_ / 26, 150, 90, 255 },
        };
        aurora(s, rb, 3);
        mountains(s, 0.13f, 0.05f, 7.0f, 17.0f, 0.7f, 0x0B0D22, 0x05060F);
        break;
    }
    case 1:     /* Nebula */
        base_gradient(s, 0x07060F, 0x0D0A1E, 0x100820);
        stars(s, W_ * H_ / 1800, H_);
        glow(s, W_ * 3 / 10, H_ * 4 / 10, W_ / 2, 150, 30, 120);
        glow(s, W_ * 7 / 10, H_ * 6 / 10, W_ / 2, 30, 70, 170);
        glow(s, W_ * 55 / 100, H_ * 3 / 10, W_ / 3, 90, 40, 170);
        glow(s, W_ * 2 / 10, H_ * 8 / 10, W_ / 3, 20, 120, 130);
        glow(s, W_ * 6 / 10, H_ * 45 / 100, W_ / 8, 120, 90, 160);
        break;
    case 2:     /* Sunset */
        base_gradient(s, 0x1B0B3A, 0x8E2D6B, 0xFF9A5A);
        glow(s, W_ / 2, H_ * 72 / 100, W_ / 5, 120, 90, 40);
        mountains(s, 0.42f, 0.07f, 5.0f, 13.0f, 0.3f, 0x5E2A6E, 0x3A1A55);
        mountains(s, 0.33f, 0.06f, 6.5f, 15.0f, 2.1f, 0x3A1850, 0x28103E);
        mountains(s, 0.22f, 0.05f, 8.0f, 19.0f, 4.2f, 0x1E0B30, 0x130720);
        break;
    case 3: {   /* Ocean */
        base_gradient(s, 0x04142E, 0x0A3A6B, 0x0E6FA0);
        glow(s, W_ * 3 / 4, H_ / 5, W_ / 3, 60, 110, 140);
        for (int layer = 0; layer < 5; layer++) {
            float base = 0.10f + 0.09f * (float)layer;
            uint32_t c1 = layer % 2 ? 0x0B4F85 : 0x0A3D6E;
            mountains(s, base, 0.025f, 9.0f + (float)layer, 21.0f, (float)layer * 1.7f,
                      (c1 & 0xFEFEFE) + 0x020406 * (uint32_t)(4 - layer), 0x031027);
        }
        break;
    }
    case 4:     /* Graphite */
        base_gradient(s, 0x14161E, 0x191C27, 0x0F1118);
        glow(s, W_ * 8 / 10, H_ * 2 / 10, W_ / 2, (int)C_R(theme.accent) / 3, (int)C_G(theme.accent) / 3,
             (int)C_B(theme.accent) / 3);
        glow(s, W_ / 10, H_, W_ / 2, 20, 30, 50);
        break;
    }
    dither(s);
}

void wallpaper_thumb(surface_t *s, int style)
{
    gen_wallpaper(s, style);
}

static void make_blur(void)
{
    gfx_blit(wall_blur, 0, 0, wall, 0, 0, SW, SH);
    gfx_box_blur(wall_blur, R(0, 0, SW, SH), 16, 3);
    for (int i = 0; i < SW * SH; i++) wall_blur->px[i] = blend(wall_blur->px[i], HEX(0x0C0F1E), 120);
}

void wm_set_wallpaper(int style)
{
    wallpaper_style = CLAMP(style, 0, wallpaper_count - 1);
    surface_t *n = surface_new(SW, SH);
    if (!n) return;
    gen_wallpaper(n, wallpaper_style);
    mutex_lock(&wm_mtx);
    surface_free(wall);
    wall = n;
    make_blur();
    wm_dirty(R(0, 0, SW, SH));
    mutex_unlock(&wm_mtx);
    wm_kick();
}

/* ------------------------------------------------------------------------
 * helpers
 * ---------------------------------------------------------------------- */

/* blurred-wallpaper panel with rounded corners and a tint on top */
static void acrylic(surface_t *s, rect_t r, int radius, color_t tint)
{
    rect_t v = rect_intersect(r, s->clip);
    if (rect_empty(v)) return;
    if (!ui_transparency) {
        gfx_round_rect(s, r.x, r.y, r.w, r.h, radius, HEX(0x1A1E31));
        return;
    }
    for (int y = v.y; y < v.y + v.h; y++) {
        uint32_t *d = s->px + (size_t)y * s->stride;
        uint32_t *b = wall_blur->px + (size_t)y * wall_blur->stride;
        int row = y - r.y;
        float fy = -1;
        if (radius && row < radius) fy = (float)(radius - row) - 0.5f;
        else if (radius && row >= r.h - radius) fy = (float)(row - (r.h - radius)) + 0.5f;
        for (int x = v.x; x < v.x + v.w; x++) {
            int cov = 255;
            if (fy >= 0) {
                int col = x - r.x;
                float fx = -1;
                if (col < radius) fx = (float)(radius - col) - 0.5f;
                else if (col >= r.w - radius) fx = (float)(col - (r.w - radius)) + 0.5f;
                if (fx >= 0) {
                    float dd = (float)k_sqrt((double)(fx * fx + fy * fy));
                    float c = (float)radius - dd + 0.5f;
                    cov = c <= 0 ? 0 : c >= 1 ? 255 : (int)(c * 255.0f);
                }
            }
            if (!cov) continue;
            uint32_t p = blend(b[x], tint, C_A(tint));
            d[x] = cov == 255 ? p : blend(d[x], p, (uint32_t)cov);
        }
    }
}


/* ------------------------------------------------------------------------
 * settings of the signed-in user: <home>/.config/zenith/settings.conf
 * (system-wide ones such as language, keyboard and time zone are in
 * /etc/zenith/system.conf and belong to sys/)
 * ---------------------------------------------------------------------- */

struct saved_settings { int wallpaper, accent, transparency, animations, welcome, volume, muted; };

static struct saved_settings saved;
static bool settings_known;
static bool welcome_seen;

/* the Welcome tour opens on a user's first sign-in only (when settings can be kept) */
bool settings_welcome_seen(void)
{
    bool seen = welcome_seen;
    welcome_seen = true;
    return seen;
}

static void settings_current(struct saved_settings *c)
{
    c->wallpaper = wallpaper_style;
    c->accent = accent_index;
    c->transparency = ui_transparency;
    c->animations = ui_animations;
    c->welcome = welcome_seen;
    c->volume = audio_volume();
    c->muted = audio_muted();
}

void settings_load(void)
{
    if (!session.active) return;
    struct conf *c = &session.prefs;
    wallpaper_style = (int)CLAMP(conf_int(c, "wallpaper", 0), 0, wallpaper_count - 1);
    theme_set_accent((int)conf_int(c, "accent", accent_index));
    ui_transparency = conf_int(c, "transparency", 1) != 0;
    ui_animations = conf_int(c, "animations", 1) != 0;
    welcome_seen = conf_int(c, "welcome_seen", 0) != 0;
    if (conf_get(c, "volume", NULL)) audio_set_volume((int)conf_int(c, "volume", audio_volume()));
    audio_set_muted(conf_int(c, "muted", 0) != 0);
    settings_current(&saved);
    settings_known = true;
    klog("desktop: settings of %s loaded", session.user);
}

void settings_autosave(void)
{
    if (!session.active) return;
    struct saved_settings c;
    settings_current(&c);
    if (!settings_known || !memcmp(&c, &saved, sizeof(c))) return;
    saved = c;
    struct conf *p = &session.prefs;
    conf_set_int(p, "wallpaper", c.wallpaper);
    conf_set_int(p, "accent", c.accent);
    conf_set_int(p, "transparency", c.transparency);
    conf_set_int(p, "animations", c.animations);
    conf_set_int(p, "welcome_seen", c.welcome);
    conf_set_int(p, "volume", c.volume);
    conf_set_int(p, "muted", c.muted);
    session_save_prefs();
}

/* ------------------------------------------------------------------------
 * desktop icons
 * ---------------------------------------------------------------------- */

struct dicon {
    char label[48];
    int icon;
    char exec[160];             /* "@app" or a program path, from a .zapp file on the desktop */
    char path[128];             /* a file or folder to open */
    rect_t r;
};

static struct dicon dicons[32];
static int ndicons;
static int dicon_sel = -1;

static void add_dicon(const char *label, int icon, const char *exec, const char *path)
{
    if (ndicons >= (int)ARRAY_SIZE(dicons)) return;
    struct dicon *d = &dicons[ndicons];
    strlcpy(d->label, label, sizeof(d->label));
    d->icon = icon;
    strlcpy(d->exec, exec ? exec : "", sizeof(d->exec));
    strlcpy(d->path, path ? path : "", sizeof(d->path));
    int per_col = MAX(1, (SH - TB_H - 16) / 100);
    int col = ndicons / per_col, row = ndicons % per_col;
    d->r = R(12 + col * 100, 14 + row * 100, 92, 92);
    ndicons++;
}

int file_icon_for(const char *name);

/* the desktop shows what is in ~/Desktop (shortcuts are .zapp files) and the mounted disks */
static void load_desktop_icons(void)
{
    ndicons = 0;
    char dir[VFS_PATH_MAX];
    if (session_path("Desktop", dir, sizeof(dir))) {
        struct vfs_dirent de;
        for (int i = 0; ndicons < (int)ARRAY_SIZE(dicons) - 8 && vfs_readdir(dir, i, &de) == 0; i++) {
            if (de.name[0] == '.') continue;
            char p[VFS_PATH_MAX];
            snprintf(p, sizeof(p), "%s/%s", dir, de.name);
            size_t l = strlen(de.name);
            if (l > 5 && !strcmp(de.name + l - 5, ".zapp")) {
                struct conf c;
                conf_init(&c);
                if (conf_load(&c, p)) {
                    char label[48];
                    strlcpy(label, conf_get(&c, "name", de.name), sizeof(label));
                    int ic = icon_by_name(conf_get(&c, "icon", "app"));
                    add_dicon(tr(label), ic ? ic : ICON_APP, conf_get(&c, "exec", ""), NULL);
                }
                conf_free(&c);
            } else {
                add_dicon(de.name, de.type == VN_DIR ? ICON_FOLDER : file_icon_for(de.name), NULL, p);
            }
        }
    }
    for (struct fs_mount *m = vfs_mounts(); m; m = m->next) {
        char label[48];
        snprintf(label, sizeof(label), "%s", m->label[0] ? m->label : tr("Disk"));
        add_dicon(label, ICON_DISK, NULL, m->path);
    }
}

static void draw_dicons(surface_t *s, rect_t clip)
{
    for (int i = 0; i < ndicons; i++) {
        struct dicon *d = &dicons[i];
        if (!rect_overlaps(d->r, clip)) continue;
        if (i == dicon_sel) gfx_round_rect(s, d->r.x, d->r.y, d->r.w, d->r.h, 10, ALPHA(0xFFFFFF, 40));
        icon_draw(s, d->icon, d->r.x + (d->r.w - 48) / 2, d->r.y + 8, 48);
        int tw = font_text_width(font_ui, d->label);
        int tx = d->r.x + (d->r.w - MIN(tw, d->r.w - 4)) / 2;
        surface_t sub = *s;
        sub.clip = rect_intersect(clip, R(d->r.x + 2, d->r.y, d->r.w - 4, d->r.h));
        gfx_text_ellipsis(&sub, font_ui, tx + 1, d->r.y + 63, d->r.w - 4, d->label, ALPHA(0, 180));
        gfx_text_ellipsis(&sub, font_ui, tx, d->r.y + 62, d->r.w - 4, d->label, HEX(0xF4F5FF));
    }
}

void open_path(const char *path);

static void dicon_open(int i)
{
    struct dicon *d = &dicons[i];
    if (d->exec[0]) {
        struct launcher l;
        memset(&l, 0, sizeof(l));
        strlcpy(l.name, d->label, sizeof(l.name));
        strlcpy(l.exec, d->exec, sizeof(l.exec));
        l.icon = d->icon;
        launcher_run(&l);
    } else if (d->path[0]) {
        open_path(d->path);
    }
}

/* ------------------------------------------------------------------------
 * taskbar
 * ---------------------------------------------------------------------- */

struct tb_item {
    const struct launcher *app;
    window_t *win;
    int icon;
    rect_t r;
};

static struct tb_item tb[24];
static int ntb;
static int tb_hover = -1;
static rect_t tb_start_r;
static bool tb_start_hover;
static uint64_t tb_hover_since;
static bool tooltip_shown;
static int cpu_hist[48];
static uint64_t last_cpu_sample;
static char clock_str[16], date_str[24];
static int last_sec = -1;
static rect_t tray_clock_r, tray_cpu_r, tray_kbd_r, tray_net_r, tray_show_r, tray_snd_r;
static uint64_t osd_until;            /* volume on-screen display */

static rect_t osd_rect(void) { return R(SW - 300, SH - TB_H - 76, 280, 60); }

static void osd_show(void)
{
    osd_until = uptime_ms() + 1500;
    wm_dirty(osd_rect());
    wm_dirty(tray_snd_r);
}

static void draw_osd(surface_t *s, rect_t clip)
{
    if (!osd_until) return;
    rect_t r = osd_rect();
    if (!rect_overlaps(r, clip)) return;
    gfx_shadow(s, r.x, r.y + 4, r.w, r.h, 14, 18, 110);
    gfx_round_rect(s, r.x, r.y, r.w, r.h, 14, ALPHA(0x1B1F33, 240));
    gfx_round_outline(s, r.x, r.y, r.w, r.h, 14, ALPHA(0xFFFFFF, 20));
    icon_draw(s, ICON_SOUND, r.x + 14, r.y + 18, 24);
    int bx = r.x + 52, bw = r.w - 110;
    gfx_round_rect(s, bx, r.y + 27, bw, 6, 3, ALPHA(0xFFFFFF, 30));
    int v = audio_muted() ? 0 : audio_volume();
    gfx_round_rect(s, bx, r.y + 27, MAX(6, bw * v / 100), 6, 3, theme.accent);
    char t[16];
    if (audio_muted()) strcpy(t, "Muted");
    else snprintf(t, sizeof(t), "%d%%", v);
    gfx_text(s, font_ui_md, bx + bw + 12, r.y + 21, t, theme.text);
}


static rect_t tb_rect(void) { return R(0, SH - TB_H, SW, TB_H); }

static const struct launcher *app_for_window(window_t *w)
{
    for (int i = 0; i < launchers_count(); i++) {
        const struct launcher *l = launcher_get(i);
        if (l->builtin && l->icon == w->icon && w->icon) return l;
    }
    return NULL;
}

/* "files,terminal,..." from the user's settings, else the system default */
static int pinned_list(const char *key, const char *fallback_key, char out[][32], int max)
{
    const char *list = conf_get(&session.prefs, key, conf_get(&sysconf, fallback_key, ""));
    int n = 0;
    for (const char *p = list; *p && n < max;) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l && l < 32) { memcpy(out[n], p, l); out[n][l] = 0; n++; }
        p += l + (e ? 1 : 0);
    }
    return n;
}

void desktop_windows_changed(void)
{
    window_t *wins[32];
    int nw = wm_window_list(wins, 32);
    ntb = 0;
    bool used[32] = { 0 };
    char pins[12][32];
    int npins = pinned_list("taskbar_pinned", "taskbar_pinned", pins, 12);
    for (int p = 0; p < npins; p++) {
        const struct launcher *a = launcher_find(pins[p]);
        if (!a) continue;
        struct tb_item *it = &tb[ntb++];
        it->app = a;
        it->icon = a->icon;
        it->win = NULL;
        for (int i = nw - 1; i >= 0; i--) {
            if (!used[i] && app_for_window(wins[i]) == a) { it->win = wins[i]; used[i] = true; break; }
        }
    }
    for (int i = 0; i < nw && ntb < (int)ARRAY_SIZE(tb); i++) {
        if (used[i]) continue;
        struct tb_item *it = &tb[ntb++];
        it->app = app_for_window(wins[i]);
        it->icon = wins[i]->icon ? wins[i]->icon : ICON_APP;
        it->win = wins[i];
    }
    int item_w = 48, gap = 4;
    int total = 48 + gap + ntb * (item_w + gap);
    int x = (SW - total) / 2;
    tb_start_r = R(x, SH - TB_H + 4, 48, TB_H - 8);
    x += 48 + gap;
    for (int i = 0; i < ntb; i++) {
        tb[i].r = R(x, SH - TB_H + 4, item_w, TB_H - 8);
        x += item_w + gap;
    }
    wm_dirty(tb_rect());
}

static int count_windows_of(const struct launcher *a)
{
    int n = 0;
    for (window_t *w = wm_list; w; w = w->next)
        if (!w->close_requested && app_for_window(w) == a) n++;
    return n;
}

static void draw_taskbar(surface_t *s, rect_t clip)
{
    rect_t tr = tb_rect();
    if (!rect_overlaps(tr, clip)) return;
    acrylic(s, tr, 0, ALPHA(0x0E1122, 150));
    gfx_fill(s, 0, tr.y, SW, 1, ALPHA(0xFFFFFF, 28));

    /* start button */
    if (tb_start_hover) gfx_round_rect(s, tb_start_r.x, tb_start_r.y, tb_start_r.w, tb_start_r.h, 8, ALPHA(0xFFFFFF, 26));
    logo_draw(s, tb_start_r.x + (tb_start_r.w - 26) / 2, tb_start_r.y + (tb_start_r.h - 26) / 2, 26, false);

    for (int i = 0; i < ntb; i++) {
        struct tb_item *it = &tb[i];
        bool focused = it->win && it->win == wm_focus_win;
        if (i == tb_hover || focused)
            gfx_round_rect(s, it->r.x, it->r.y, it->r.w, it->r.h, 8, ALPHA(0xFFFFFF, focused ? 30 : 22));
        icon_draw(s, it->icon, it->r.x + (it->r.w - 28) / 2, it->r.y + (it->r.h - 28) / 2 - 2, 28);
        if (it->win) {
            int n = it->app ? count_windows_of(it->app) : 1;
            int bw = focused ? 16 : 6;
            color_t c = focused ? theme.accent : ALPHA(0xFFFFFF, 150);
            gfx_round_rect(s, it->r.x + (it->r.w - bw) / 2, it->r.y + it->r.h - 4, bw, 3, 1, c);
            if (n > 1 && !focused)
                gfx_round_rect(s, it->r.x + (it->r.w - bw) / 2 + 9, it->r.y + it->r.h - 4, 3, 3, 1, c);
        }
    }

    /* tray: show-desktop strip, clock, keyboard layout, network, cpu graph */
    int x = SW - 8;
    tray_show_r = R(SW - 6, tr.y, 6, TB_H);
    gfx_fill(s, SW - 6, tr.y + 12, 1, TB_H - 24, ALPHA(0xFFFFFF, 40));
    x -= 6;
    int cw = MAX(font_text_width(font_ui, date_str), font_text_width(font_ui_md, clock_str)) + 20;
    tray_clock_r = R(x - cw, tr.y + 4, cw, TB_H - 8);
    x -= cw;
    gfx_text_center(s, font_ui_md, R(tray_clock_r.x, tr.y + 6, cw, 20), clock_str, theme.text);
    gfx_text_center(s, font_ui, R(tray_clock_r.x, tr.y + 25, cw, 20), date_str, theme.text_dim);

    tray_kbd_r = R(x - 36, tr.y + 10, 32, TB_H - 20);
    x -= 36;
    gfx_text_center(s, font_ui_md, tray_kbd_r, keymap_short(), theme.text);

    if (audio_available()) {
        tray_snd_r = R(x - 32, tr.y + 10, 28, TB_H - 20);
        x -= 32;
        icon_draw(s, ICON_SOUND, tray_snd_r.x + 4, tray_snd_r.y + 6, 20);
        if (audio_muted())
            gfx_line_w(s, (float)tray_snd_r.x + 5, (float)tray_snd_r.y + 26, (float)tray_snd_r.x + 23,
                       (float)tray_snd_r.y + 8, 2.2f, theme.danger);
    } else {
        tray_snd_r = R(0, 0, 0, 0);
    }

    tray_net_r = R(x - 32, tr.y + 10, 28, TB_H - 20);
    x -= 32;
    {
        /* the real state of the network: no adapter, cable out, waiting for an address, online */
        int st = 0;                                    /* 0 none, 1 no link, 2 no address yet, 3 online */
        for (int i = 0; i < net_iface_count(); i++) {
            struct net_info ni;
            if (!net_iface_get(i, &ni)) continue;
            int v = !ni.link ? 1 : !ni.ip ? 2 : 3;
            if (v > st) st = v;
        }
        icon_draw(s, ICON_ETHERNET, tray_net_r.x + 4, tray_net_r.y + 6, 20);
        if (st == 0) {
            gfx_line_w(s, (float)tray_net_r.x + 5, (float)tray_net_r.y + 26, (float)tray_net_r.x + 23, (float)tray_net_r.y + 8, 2.2f, theme.danger);
        } else {
            color_t dot = st == 3 ? theme.success : st == 2 ? theme.warning : theme.danger;
            gfx_circle(s, (float)tray_net_r.x + 22, (float)tray_net_r.y + 24, 4.2f, HEX(0x101326));
            gfx_circle(s, (float)tray_net_r.x + 22, (float)tray_net_r.y + 24, 3.2f, dot);
        }
    }

    tray_cpu_r = R(x - 84, tr.y + 10, 76, TB_H - 20);
    rect_t g = tray_cpu_r;
    gfx_round_rect(s, g.x, g.y, g.w, g.h, 6, ALPHA(0xFFFFFF, 14));
    int n = (int)ARRAY_SIZE(cpu_hist);
    float step = (float)(g.w - 8) / (float)(n - 1);
    for (int i = 1; i < n; i++) {
        float x0 = (float)g.x + 4 + step * (float)(i - 1), x1 = (float)g.x + 4 + step * (float)i;
        float y0 = (float)(g.y + g.h - 4) - (float)cpu_hist[i - 1] * (float)(g.h - 8) / 100.0f;
        float y1 = (float)(g.y + g.h - 4) - (float)cpu_hist[i] * (float)(g.h - 8) / 100.0f;
        gfx_line(s, x0, y0, x1, y1, theme.accent2);
    }
    char cpu_txt[16];
    snprintf(cpu_txt, sizeof(cpu_txt), "%d%%", cpu_hist[n - 1]);
    gfx_text(s, font_ui, g.x + 6, g.y + 3, cpu_txt, ALPHA(0xFFFFFF, 190));
}

/* ------------------------------------------------------------------------
 * start menu
 * ---------------------------------------------------------------------- */

#define SM_W 640
#define SM_H 580

static bool sm_open;
static float sm_t;             /* 0 closed .. 1 open */
static uint64_t sm_anim_t0;
static char sm_search[48];
static int sm_hover = -1;
static int sm_sel;
static bool sm_power_open;
static int sm_power_hover = -1;
static bool sm_power_btn_hover;
static int sm_results[32];
static int sm_nresults;
static int sm_tiles[256];
static int sm_ntiles;
static bool sm_all;                   /* showing every app instead of the pinned ones */
static int sm_scroll;                 /* first visible tile row */
#define SM_ROWS 3

static rect_t sm_rect(void)
{
    int dy = (int)((1.0f - sm_t) * 24.0f);
    return R((SW - SM_W) / 2, SH - TB_H - SM_H - 12 + dy, SM_W, SM_H);
}

static rect_t sm_full_rect(void)
{
    return R((SW - SM_W) / 2 - 30, SH - TB_H - SM_H - 40, SM_W + 60, SM_H + 70);
}

static int sm_score(const struct launcher *a)
{
    if (!sm_search[0]) return 1;
    const char *name = tr(a->name);
    if (!strncasecmp(name, sm_search, strlen(sm_search))) return 4;
    if (!strncasecmp(a->id, sm_search, strlen(sm_search))) return 3;
    if (strstr_ci(name, sm_search)) return 2;
    if (strstr_ci(tr(a->desc), sm_search) || strstr_ci(a->category, sm_search)) return 1;
    return 0;
}

static void sm_filter(void)
{
    sm_nresults = 0;
    for (int score = 4; score >= 1; score--)
        for (int i = 0; i < launchers_count() && sm_nresults < 32; i++)
            if (sm_score(launcher_get(i)) == score) sm_results[sm_nresults++] = i;
    if (sm_sel >= sm_nresults) sm_sel = MAX(0, sm_nresults - 1);
}

/* the tiles of the start menu: the pinned apps, or every app */
static void sm_build_tiles(void)
{
    sm_ntiles = 0;
    if (sm_all) {
        for (int i = 0; i < launchers_count() && sm_ntiles < (int)ARRAY_SIZE(sm_tiles); i++) sm_tiles[sm_ntiles++] = i;
        return;
    }
    char pins[32][32];
    int n = pinned_list("startmenu_pinned", "startmenu_pinned", pins, 32);
    for (int i = 0; i < n; i++)
        for (int k = 0; k < launchers_count(); k++)
            if (!strcmp(launcher_get(k)->id, pins[i])) { sm_tiles[sm_ntiles++] = k; break; }
}

static void sm_set(bool open)
{
    if (open == sm_open) return;
    sm_open = open;
    sm_anim_t0 = uptime_ms();
    if (open) {
        sm_search[0] = 0;
        sm_sel = 0;
        sm_power_open = false;
        sm_all = false;
        sm_scroll = 0;
        sm_build_tiles();
        sm_filter();
    }
    wm_dirty(sm_full_rect());
    wm_dirty(tb_start_r);
}

void desktop_toggle_start(void) { sm_set(!sm_open); }

static rect_t sm_tile_rect(rect_t m, int i)
{
    int cols = 6, tw = 96, th = 90;
    int gx = m.x + (SM_W - cols * tw) / 2;
    return R(gx + (i % cols) * tw, m.y + 118 + (i / cols - sm_scroll) * th, tw, th);
}

static bool sm_tile_visible(int i)
{
    int row = i / 6 - sm_scroll;
    return row >= 0 && row < SM_ROWS;
}

static rect_t sm_result_rect(rect_t m, int i)
{
    return R(m.x + 24, m.y + 110 + i * 46, SM_W - 48, 42);
}

static rect_t sm_power_rect(rect_t m) { return R(m.x + SM_W - 64, m.y + SM_H - 56, 44, 44); }
static rect_t sm_power_menu_rect(rect_t m) { return R(m.x + SM_W - 200, m.y + SM_H - 56 - 180, 180, 176); }

static rect_t sm_allapps_rect(rect_t m) { return R(m.x + SM_W - 160, m.y + 80, 128, 24); }

static void draw_start_menu(surface_t *s, rect_t clip)
{
    if (sm_t <= 0) return;
    rect_t m = sm_rect();
    if (!rect_overlaps(sm_full_rect(), clip)) return;
    int op = (int)(sm_t * 255.0f);
    gfx_shadow(s, m.x, m.y + 8, m.w, m.h, 14, 30, 120 * op / 255);
    acrylic(s, m, 14, ALPHA(0x121528, 170));
    gfx_round_outline(s, m.x, m.y, m.w, m.h, 14, ALPHA(0xFFFFFF, 30));

    /* search */
    rect_t sb = R(m.x + 24, m.y + 22, SM_W - 48, 42);
    gfx_round_rect(s, sb.x, sb.y, sb.w, sb.h, 21, ALPHA(0xFFFFFF, 20));
    gfx_round_outline(s, sb.x, sb.y, sb.w, sb.h, 21, sm_search[0] ? theme.accent : ALPHA(0xFFFFFF, 30));
    icon_draw(s, ICON_SEARCH, sb.x + 14, sb.y + 12, 18);
    int ty = sb.y + (sb.h - font_height(font_ui_lg)) / 2;
    if (sm_search[0]) {
        int ex = gfx_text(s, font_ui_lg, sb.x + 44, ty, sm_search, theme.text);
        if ((uptime_ms() / 500) % 2 == 0) gfx_fill(s, ex + 1, ty + 1, 2, font_height(font_ui_lg) - 2, theme.accent);
    } else {
        gfx_text(s, font_ui_lg, sb.x + 44, ty, tr("Search apps, settings and files"), theme.text_faint);
    }

    if (!sm_search[0]) {
        gfx_text(s, font_ui_bold, m.x + 32, m.y + 86, sm_all ? tr("All apps") : tr("Pinned"), theme.text);
        gfx_text_right(s, font_ui, m.x + SM_W - 32, m.y + 88, sm_all ? tr("Pinned") : tr("All apps"), theme.text_dim);
        for (int i = 0; i < sm_ntiles; i++) {
            if (!sm_tile_visible(i)) continue;
            const struct launcher *l = launcher_get(sm_tiles[i]);
            rect_t t = sm_tile_rect(m, i);
            if (i == sm_hover) gfx_round_rect(s, t.x + 4, t.y, t.w - 8, t.h - 6, 10, ALPHA(0xFFFFFF, 22));
            icon_draw(s, l->icon, t.x + (t.w - 40) / 2, t.y + 10, 40);
            surface_t sub = *s;
            sub.clip = rect_intersect(s->clip, R(t.x + 4, t.y, t.w - 8, t.h));
            const char *nm = tr(l->name);
            int tw = font_text_width(font_ui, nm);
            gfx_text_ellipsis(&sub, font_ui, t.x + MAX(6, (t.w - tw) / 2), t.y + 58, t.w - 12, nm, theme.text);
        }
        int rows = (sm_ntiles + 5) / 6;
        if (rows > SM_ROWS) {
            /* scroll indicator */
            int th = 3 * 90 - 10, bh = MAX(24, th * SM_ROWS / rows);
            int by = m.y + 118 + (th - bh) * sm_scroll / MAX(1, rows - SM_ROWS);
            gfx_round_rect(s, m.x + SM_W - 14, by, 4, bh, 2, ALPHA(0xFFFFFF, 70));
        }
        /* system card */
        rect_t card = R(m.x + 24, m.y + SM_H - 150, SM_W - 48, 70);
        gfx_round_rect(s, card.x, card.y, card.w, card.h, 12, ALPHA(0xFFFFFF, 14));
        logo_draw(s, card.x + 14, card.y + 13, 44, false);
        char l1[96], l2[128];
        snprintf(l1, sizeof(l1), "%s %s \"%s\"", ZENITH_NAME, ZENITH_VERSION, ZENITH_CODENAME);
        uint64_t up = uptime_ms() / 1000;
        snprintf(l2, sizeof(l2), "%d %s  ·  %lu MiB RAM  ·  %s %lu:%02lu:%02lu", ncpus, tr("CPU cores"),
                 (pmm_total_pages() * 4096) >> 20, tr("up"), up / 3600, (up / 60) % 60, up % 60);
        gfx_text(s, font_ui_bold, card.x + 72, card.y + 14, l1, theme.text);
        gfx_text(s, font_ui, card.x + 72, card.y + 38, l2, theme.text_dim);
    } else {
        gfx_text(s, font_ui_bold, m.x + 32, m.y + 80, sm_nresults ? tr("Best matches") : tr("No results"), theme.text);
        for (int i = 0; i < sm_nresults && i < 9; i++) {
            const struct launcher *a = launcher_get(sm_results[i]);
            rect_t r = sm_result_rect(m, i);
            if (i == sm_sel) gfx_round_rect(s, r.x, r.y, r.w, r.h, 8, ALPHA(theme.accent, 90));
            else if (i == sm_hover) gfx_round_rect(s, r.x, r.y, r.w, r.h, 8, ALPHA(0xFFFFFF, 20));
            icon_draw(s, a->icon, r.x + 10, r.y + 7, 28);
            gfx_text(s, font_ui_md, r.x + 50, r.y + 5, tr(a->name), theme.text);
            gfx_text(s, font_ui, r.x + 50, r.y + 22, tr(a->desc), theme.text_dim);
        }
    }

    /* bottom bar: who is signed in, and power */
    gfx_fill(s, m.x + 1, m.y + SM_H - 68, SM_W - 2, 1, ALPHA(0xFFFFFF, 20));
    gfx_circle(s, (float)(m.x + 46), (float)(m.y + SM_H - 34), 17, theme.accent);
    {
        char ini[8] = "?";
        const char *p = session.fullname[0] ? session.fullname : session.user;
        uint32_t cp = utf8_next(&p);
        if (cp) ini[utf8_encode(cp >= 'a' && cp <= 'z' ? cp - 32 : cp, ini)] = 0;
        gfx_text_center(s, font_ui_bold, R(m.x + 29, m.y + SM_H - 51, 34, 34), ini, 0xFFFFFFFFu);
    }
    gfx_text(s, font_ui_md, m.x + 72, m.y + SM_H - 43, session.fullname[0] ? session.fullname : session.user, theme.text);
    rect_t pb = sm_power_rect(m);
    if (sm_power_btn_hover || sm_power_open) gfx_round_rect(s, pb.x, pb.y, pb.w, pb.h, 8, ALPHA(0xFFFFFF, 24));
    icon_draw(s, ICON_POWER, pb.x + 11, pb.y + 11, 22);

    if (sm_power_open) {
        rect_t pm = sm_power_menu_rect(m);
        gfx_shadow(s, pm.x, pm.y + 4, pm.w, pm.h, 10, 16, 110);
        gfx_round_rect(s, pm.x, pm.y, pm.w, pm.h, 10, HEX(0x232842));
        gfx_round_outline(s, pm.x, pm.y, pm.w, pm.h, 10, ALPHA(0xFFFFFF, 30));
        const char *items[] = { N_("Lock"), N_("Sign out"), N_("Restart"), N_("Shut down") };
        int icons[] = { ICON_LOCK, ICON_LOGOUT, ICON_RESTART, ICON_POWER };
        for (int i = 0; i < 4; i++) {
            rect_t it = R(pm.x + 6, pm.y + 6 + i * 42, pm.w - 12, 40);
            if (i == sm_power_hover) gfx_round_rect(s, it.x, it.y, it.w, it.h, 8, ALPHA(0xFFFFFF, 24));
            icon_draw(s, icons[i], it.x + 10, it.y + 10, 20);
            gfx_text(s, font_ui_md, it.x + 42, it.y + 11, tr(items[i]), theme.text);
        }
    }
}

static void power_action(int which);

static bool sm_mouse(int x, int y, int pressed, int released)
{
    rect_t m = sm_rect();
    int old_hover = sm_hover, old_ph = sm_power_hover;
    bool old_pbh = sm_power_btn_hover;
    sm_hover = -1;
    sm_power_hover = -1;
    sm_power_btn_hover = rect_has(sm_power_rect(m), x, y);
    if (sm_power_open) {
        rect_t pm = sm_power_menu_rect(m);
        for (int i = 0; i < 4; i++)
            if (rect_has(R(pm.x + 6, pm.y + 6 + i * 42, pm.w - 12, 40), x, y)) sm_power_hover = i;
        if ((released & BTN_LEFT) && sm_power_hover >= 0) {
            power_action(sm_power_hover);
            return true;
        }
        if ((pressed & BTN_LEFT) && !rect_has(pm, x, y) && !sm_power_btn_hover) {
            sm_power_open = false;
            wm_dirty(m);
        }
    }
    if (!sm_search[0]) {
        for (int i = 0; i < sm_ntiles; i++)
            if (sm_tile_visible(i) && rect_has(sm_tile_rect(m, i), x, y)) sm_hover = i;
        if ((released & BTN_LEFT) && rect_has(sm_allapps_rect(m), x, y)) {
            sm_all = !sm_all;
            sm_scroll = 0;
            sm_build_tiles();
            wm_dirty(sm_full_rect());
            return true;
        }
    } else {
        for (int i = 0; i < sm_nresults && i < 9; i++)
            if (rect_has(sm_result_rect(m, i), x, y)) sm_hover = i;
    }
    if (sm_hover != old_hover || sm_power_hover != old_ph || old_pbh != sm_power_btn_hover) wm_dirty(m);
    if (released & BTN_LEFT) {
        if (sm_power_btn_hover) {
            sm_power_open = !sm_power_open;
            wm_dirty(sm_full_rect());
        } else if (sm_hover >= 0) {
            int idx = sm_search[0] ? sm_results[sm_hover] : sm_tiles[sm_hover];
            sm_set(false);
            launcher_run(launcher_get(idx));
        }
    }
    return true;
}

/* ------------------------------------------------------------------------
 * context menu, notifications, alt-tab, tooltip
 * ---------------------------------------------------------------------- */

static bool ctx_open;
static rect_t ctx_r;
static int ctx_hover = -1;
static const char *ctx_items[] = { N_("Open Terminal"), N_("Next wallpaper"), N_("Refresh desktop"), N_("Settings"), N_("About ZenithOS") };
#define CTX_N 5

static void draw_ctx(surface_t *s, rect_t clip)
{
    if (!ctx_open || !rect_overlaps(R(ctx_r.x - 20, ctx_r.y - 20, ctx_r.w + 40, ctx_r.h + 44), clip)) return;
    gfx_shadow(s, ctx_r.x, ctx_r.y + 4, ctx_r.w, ctx_r.h, 10, 16, 120);
    gfx_round_rect(s, ctx_r.x, ctx_r.y, ctx_r.w, ctx_r.h, 10, HEX(0x20243B));
    gfx_round_outline(s, ctx_r.x, ctx_r.y, ctx_r.w, ctx_r.h, 10, ALPHA(0xFFFFFF, 30));
    for (int i = 0; i < CTX_N; i++) {
        rect_t it = R(ctx_r.x + 5, ctx_r.y + 5 + i * 34, ctx_r.w - 10, 32);
        if (i == ctx_hover) gfx_round_rect(s, it.x, it.y, it.w, it.h, 7, ALPHA(theme.accent, 110));
        gfx_text(s, font_ui, it.x + 12, it.y + 8, tr(ctx_items[i]), theme.text);
    }
}

static void ctx_action(int i)
{
    switch (i) {
    case 0: app_launch("terminal"); break;
    case 1: {
        int next = (wallpaper_style + 1) % wallpaper_count;
        wallpaper_style = next;
        app_launch("@wallpaper");
        break;
    }
    case 2: load_desktop_icons(); wm_dirty(R(0, 0, 320, SH)); break;
    case 3: app_launch("settings"); break;
    case 4: app_launch("about"); break;
    }
}

struct toast {
    char title[64];
    char body[128];
    int icon;
    uint64_t t0;
};
static struct toast toasts[4];
static int ntoasts;
#define TOAST_W 340
#define TOAST_H 74
#define TOAST_LIFE 4500

static rect_t toast_rect(int i, uint64_t now)
{
    struct toast *t = &toasts[i];
    uint64_t age = now - t->t0;
    int slide = 0;
    if (age < 220) slide = (int)((220 - age) * TOAST_W / 220);
    else if (age > TOAST_LIFE - 250) slide = (int)(MIN(250ull, age - (TOAST_LIFE - 250)) * TOAST_W / 250);
    return R(SW - TOAST_W - 16 + slide, 16 + i * (TOAST_H + 10), TOAST_W, TOAST_H);
}

void desktop_notify(const char *title, const char *body, int icon)
{
    audio_sound(SND_NOTIFY);
    if (ntoasts == 4) {
        memmove(&toasts[0], &toasts[1], sizeof(toasts[0]) * 3);
        ntoasts--;
    }
    struct toast *t = &toasts[ntoasts++];
    strlcpy(t->title, title, sizeof(t->title));
    strlcpy(t->body, body ? body : "", sizeof(t->body));
    t->icon = icon;
    t->t0 = uptime_ms();
    wm_dirty(R(SW - TOAST_W - 40, 0, TOAST_W + 40, 4 * (TOAST_H + 10) + 40));
}

static void draw_toasts(surface_t *s, rect_t clip)
{
    uint64_t now = uptime_ms();
    for (int i = 0; i < ntoasts; i++) {
        rect_t r = toast_rect(i, now);
        if (!rect_overlaps(R(r.x - 20, r.y - 20, r.w + 40, r.h + 44), clip)) continue;
        gfx_shadow(s, r.x, r.y + 4, r.w, r.h, 12, 18, 110);
        acrylic(s, r, 12, ALPHA(0x161A2E, 185));
        gfx_round_outline(s, r.x, r.y, r.w, r.h, 12, ALPHA(0xFFFFFF, 30));
        icon_draw(s, toasts[i].icon ? toasts[i].icon : ICON_INFO, r.x + 14, r.y + 17, 40);
        surface_t sub = *s;
        sub.clip = rect_intersect(s->clip, R(r.x, r.y, r.w - 12, r.h));
        gfx_text_ellipsis(&sub, font_ui_bold, r.x + 66, r.y + 15, r.w - 80, toasts[i].title, theme.text);
        gfx_text_ellipsis(&sub, font_ui, r.x + 66, r.y + 39, r.w - 80, toasts[i].body, theme.text_dim);
    }
}

static bool at_open;
static int at_sel;
static window_t *at_list[16];
static int at_n;

static rect_t at_rect(void)
{
    int w = MAX(1, at_n) * 132 + 24;
    return R((SW - w) / 2, (SH - 170) / 2, w, 170);
}

void desktop_alt_tab(bool start, bool reverse)
{
    if (start) {
        at_n = 0;
        for (window_t *w = wm_list; w; w = w->next)
            if (!w->close_requested && !(w->flags & WF_NO_TASKBAR) && at_n < 16) at_list[at_n++] = w;
        /* most recent (top) first */
        for (int i = 0; i < at_n / 2; i++) {
            window_t *t = at_list[i];
            at_list[i] = at_list[at_n - 1 - i];
            at_list[at_n - 1 - i] = t;
        }
        if (!at_n) return;
        at_open = true;
        at_sel = at_n > 1 ? 1 : 0;
    } else if (at_open) {
        at_sel = (at_sel + (reverse ? at_n - 1 : 1)) % at_n;
    }
    wm_dirty(R(at_rect().x - 30, at_rect().y - 30, at_rect().w + 60, at_rect().h + 70));
}

void desktop_alt_release(void)
{
    if (!at_open) return;
    at_open = false;
    wm_dirty(R(at_rect().x - 30, at_rect().y - 30, at_rect().w + 60, at_rect().h + 70));
    if (at_sel < at_n) {
        window_t *w = at_list[at_sel];
        if (w->state == WS_MINIMIZED) wm_restore(w);
        wm_raise(w);
        wm_focus_locked(w);
    }
}

static void draw_alt_tab(surface_t *s, rect_t clip)
{
    if (!at_open) return;
    rect_t r = at_rect();
    if (!rect_overlaps(R(r.x - 30, r.y - 30, r.w + 60, r.h + 70), clip)) return;
    gfx_shadow(s, r.x, r.y + 6, r.w, r.h, 16, 28, 140);
    acrylic(s, r, 16, ALPHA(0x121528, 190));
    gfx_round_outline(s, r.x, r.y, r.w, r.h, 16, ALPHA(0xFFFFFF, 34));
    for (int i = 0; i < at_n; i++) {
        rect_t c = R(r.x + 12 + i * 132, r.y + 12, 124, r.h - 24);
        if (i == at_sel) {
            gfx_round_rect(s, c.x, c.y, c.w, c.h, 12, ALPHA(theme.accent, 70));
            gfx_round_outline(s, c.x, c.y, c.w, c.h, 12, theme.accent);
        }
        icon_draw(s, at_list[i]->icon ? at_list[i]->icon : ICON_APP, c.x + (c.w - 56) / 2, c.y + 22, 56);
        surface_t sub = *s;
        sub.clip = rect_intersect(s->clip, R(c.x + 6, c.y, c.w - 12, c.h));
        int tw = font_text_width(font_ui, at_list[i]->title);
        gfx_text_ellipsis(&sub, font_ui, c.x + MAX(8, (c.w - tw) / 2), c.y + 100, c.w - 16, at_list[i]->title,
                          theme.text);
    }
}

static rect_t tooltip_rect(const char *text, rect_t anchor)
{
    int w = font_text_width(font_ui, text) + 20;
    return R(anchor.x + (anchor.w - w) / 2, anchor.y - 40, w, 28);
}

static const char *tb_label(int i)
{
    if (i < 0 || i >= ntb) return NULL;
    if (tb[i].win) return tb[i].win->title;
    return tb[i].app ? tr(tb[i].app->name) : NULL;
}

static void draw_tooltip(surface_t *s, rect_t clip)
{
    if (!tooltip_shown || tb_hover < 0) return;
    const char *t = tb_label(tb_hover);
    if (!t) return;
    rect_t r = tooltip_rect(t, tb[tb_hover].r);
    if (!rect_overlaps(R(r.x - 10, r.y - 10, r.w + 20, r.h + 20), clip)) return;
    gfx_shadow(s, r.x, r.y + 2, r.w, r.h, 6, 10, 90);
    gfx_round_rect(s, r.x, r.y, r.w, r.h, 6, HEX(0x262B45));
    gfx_round_outline(s, r.x, r.y, r.w, r.h, 6, ALPHA(0xFFFFFF, 30));
    gfx_text_center(s, font_ui, r, t, theme.text);
}

static void dirty_tooltip(void)
{
    const char *t = tb_label(tb_hover);
    if (t) {
        rect_t r = tooltip_rect(t, tb[tb_hover].r);
        wm_dirty(R(r.x - 12, r.y - 12, r.w + 24, r.h + 24));
    }
}

/* ------------------------------------------------------------------------
 * hooks
 * ---------------------------------------------------------------------- */

void desktop_draw_background(surface_t *s, rect_t clip)
{
    for (int y = clip.y; y < clip.y + clip.h; y++)
        memcpy(s->px + (size_t)y * s->stride + clip.x, wall->px + (size_t)y * wall->stride + clip.x,
               (size_t)clip.w * 4);
    draw_dicons(s, clip);
}

void desktop_draw_overlay(surface_t *s, rect_t clip)
{
    if (login_active()) {
        login_draw(s, clip, wall_blur);
        return;
    }
    draw_taskbar(s, clip);
    draw_start_menu(s, clip);
    draw_ctx(s, clip);
    draw_alt_tab(s, clip);
    draw_toasts(s, clip);
    draw_osd(s, clip);
    draw_tooltip(s, clip);
}

int desktop_taskbar_h(void) { return TB_H; }

static void power_action(int which)
{
    sm_set(false);
    switch (which) {
    case 0: login_start(true); break;                            /* lock */
    case 1: desktop_sign_out(); break;
    case 2: app_launch("@reboot"); break;
    default: app_launch("@poweroff"); break;
    }
}

bool desktop_mouse_overlay(int x, int y, int buttons, int pressed, int released, int wheel)
{
    UNUSED(buttons);
    if (login_active()) return login_mouse(x, y, pressed);
    if (wheel && rect_has(tray_snd_r, x, y)) {
        audio_set_muted(false);
        audio_set_volume(audio_volume() + wheel * 5);
        audio_sound(SND_POP);
        osd_show();
        return true;
    }
    /* notifications: click to dismiss */
    uint64_t now = uptime_ms();
    for (int i = 0; i < ntoasts; i++) {
        if (rect_has(toast_rect(i, now), x, y)) {
            if (pressed & BTN_LEFT) toasts[i].t0 = now - TOAST_LIFE + 250;
            return true;
        }
    }
    if (ctx_open) {
        int old = ctx_hover;
        ctx_hover = -1;
        for (int i = 0; i < CTX_N; i++)
            if (rect_has(R(ctx_r.x + 5, ctx_r.y + 5 + i * 34, ctx_r.w - 10, 32), x, y)) ctx_hover = i;
        if (old != ctx_hover) wm_dirty(ctx_r);
        if ((released & BTN_LEFT) && ctx_hover >= 0) {
            int a = ctx_hover;
            ctx_open = false;
            wm_dirty(R(ctx_r.x - 20, ctx_r.y - 20, ctx_r.w + 40, ctx_r.h + 44));
            ctx_action(a);
            return true;
        }
        if (rect_has(ctx_r, x, y)) return true;
        if (pressed) {
            ctx_open = false;
            wm_dirty(R(ctx_r.x - 20, ctx_r.y - 20, ctx_r.w + 40, ctx_r.h + 44));
            return true;
        }
    }
    if (sm_open) {
        rect_t m = sm_rect();
        if (wheel && rect_has(m, x, y) && !sm_search[0]) {
            int rows = (sm_ntiles + 5) / 6;
            sm_scroll = CLAMP(sm_scroll + (wheel > 0 ? 1 : -1), 0, MAX(0, rows - SM_ROWS));
            wm_dirty(sm_full_rect());
            return true;
        }
        if (rect_has(m, x, y) || (sm_power_open && rect_has(sm_power_menu_rect(m), x, y)))
            return sm_mouse(x, y, pressed, released);
        if (pressed && !rect_has(tb_start_r, x, y)) {
            sm_set(false);
            return y >= SH - TB_H;
        }
    }

    rect_t tr = tb_rect();
    if (!rect_has(tr, x, y)) {
        if (tb_hover >= 0 || tb_start_hover) {
            dirty_tooltip();
            tb_hover = -1;
            tb_start_hover = false;
            tooltip_shown = false;
            wm_dirty(tr);
        }
        return false;
    }
    int old = tb_hover;
    bool old_sh = tb_start_hover;
    tb_hover = -1;
    for (int i = 0; i < ntb; i++)
        if (rect_has(tb[i].r, x, y)) tb_hover = i;
    tb_start_hover = rect_has(tb_start_r, x, y);
    if (old != tb_hover || old_sh != tb_start_hover) {
        if (tooltip_shown) {
            int cur = tb_hover;
            tb_hover = old;
            dirty_tooltip();
            tb_hover = cur;
        }
        tooltip_shown = false;
        tb_hover_since = uptime_ms();
        wm_dirty(tr);
    }
    if (pressed & BTN_LEFT) {
        if (tb_start_hover) {
            desktop_toggle_start();
        } else if (tb_hover >= 0) {
            struct tb_item *it = &tb[tb_hover];
            if (it->win) {
                if (it->win == wm_focus_win && it->win->state != WS_MINIMIZED) wm_minimize(it->win);
                else {
                    if (it->win->state == WS_MINIMIZED) wm_restore(it->win);
                    wm_raise(it->win);
                    wm_focus_locked(it->win);
                }
            } else if (it->app) {
                launcher_run(it->app);
            }
            if (tooltip_shown) { dirty_tooltip(); tooltip_shown = false; }
        } else if (rect_has(tray_show_r, x, y)) {
            for (window_t *w = wm_list; w; w = w->next)
                if (!(w->flags & WF_NO_TASKBAR)) wm_minimize(w);
        } else if (rect_has(tray_clock_r, x, y)) {
            app_launch("clock");
        } else if (rect_has(tray_cpu_r, x, y)) {
            app_launch("sysmon");
        } else if (rect_has(tray_kbd_r, x, y)) {
            keymap_next();
            conf_set(&sysconf, "keyboard", keymap_current());
            sys_save_conf();
            desktop_notify(tr("Keyboard layout"), keymap_long(), ICON_SETTINGS);
            wm_dirty(tr);
        } else if (rect_has(tray_net_r, x, y)) {
            app_launch("netinfo");
        } else if (rect_has(tray_snd_r, x, y)) {
            audio_set_muted(!audio_muted());
            osd_show();
        }
    }
    if ((pressed & BTN_MIDDLE) && tb_hover >= 0 && tb[tb_hover].app) launcher_run(tb[tb_hover].app);
    return true;
}

void desktop_mouse_background(int x, int y, int buttons, int pressed, int released, int clicks)
{
    UNUSED(buttons);
    UNUSED(released);
    if (pressed & BTN_LEFT) {
        int old = dicon_sel;
        dicon_sel = -1;
        for (int i = 0; i < ndicons; i++)
            if (rect_has(dicons[i].r, x, y)) dicon_sel = i;
        if (old >= 0) wm_dirty(dicons[old].r);
        if (dicon_sel >= 0) {
            wm_dirty(dicons[dicon_sel].r);
            if (clicks >= 2) dicon_open(dicon_sel);
        }
    }
    if (pressed & BTN_RIGHT) {
        ctx_open = true;
        ctx_hover = -1;
        ctx_r = R(x, y, 210, CTX_N * 34 + 10);
        if (ctx_r.x + ctx_r.w > SW) ctx_r.x = SW - ctx_r.w - 4;
        if (ctx_r.y + ctx_r.h > SH - TB_H) ctx_r.y = SH - TB_H - ctx_r.h - 4;
        wm_dirty(R(ctx_r.x - 20, ctx_r.y - 20, ctx_r.w + 40, ctx_r.h + 44));
    }
}

bool desktop_key(int key, uint32_t ch, int mods, bool pressed)
{
    if (login_active()) return login_key(key, ch, mods, pressed);
    if (ctx_open && pressed && key == KEY_ESC) {
        ctx_open = false;
        wm_dirty(R(ctx_r.x - 20, ctx_r.y - 20, ctx_r.w + 40, ctx_r.h + 44));
        return true;
    }
    if (!sm_open) return false;
    if (!pressed) return true;
    rect_t m = sm_rect();
    if (key == KEY_ESC) { sm_set(false); return true; }
    if (key == KEY_ENTER || key == KEY_KPENTER) {
        if (sm_search[0] && sm_nresults) {
            int idx = sm_results[sm_sel];
            sm_set(false);
            launcher_run(launcher_get(idx));
        }
        return true;
    }
    if (key == KEY_DOWN) { if (sm_sel < sm_nresults - 1) sm_sel++; wm_dirty(m); return true; }
    if (key == KEY_UP) { if (sm_sel > 0) sm_sel--; wm_dirty(m); return true; }
    if (key == KEY_BACKSPACE) {
        size_t l = strlen(sm_search);
        if (l) sm_search[l - utf8_prev_len(sm_search, sm_search + l)] = 0;
        sm_filter();
        wm_dirty(m);
        return true;
    }
    if (ch >= 32 && ch != 127) {
        char enc[4];
        int n = utf8_encode(ch, enc);
        size_t l = strlen(sm_search);
        if (l + (size_t)n < sizeof(sm_search) - 1) {
            memcpy(sm_search + l, enc, (size_t)n);
            sm_search[l + (size_t)n] = 0;
        }
        sm_sel = 0;
        sm_filter();
        wm_dirty(m);
    }
    return true;
}

static void update_clock(void)
{
    struct datetime dt;
    rtc_now(&dt);
    if (dt.second == last_sec) return;
    last_sec = dt.second;
    settings_autosave();
    char c[16], d[24];
    snprintf(c, sizeof(c), "%02d:%02d", dt.hour, dt.minute);
    format_date(&dt, d, sizeof(d));
    if (strcmp(c, clock_str) || strcmp(d, date_str)) {
        strcpy(clock_str, c);
        strcpy(date_str, d);
        wm_dirty(tray_clock_r.w ? R(tray_clock_r.x - 20, tray_clock_r.y, tray_clock_r.w + 20, tray_clock_r.h) : tb_rect());
    }
}

bool desktop_tick(uint64_t now)
{
    bool login_anim = login_tick();
    static int seen_mounts = -1;
    if (seen_mounts != vfs_mount_generation()) {
        seen_mounts = vfs_mount_generation();
        load_desktop_icons();
        wm_dirty(R(0, 0, SW, SH));
    }
    bool anim = login_anim;
    update_clock();
    if (osd_until && now > osd_until) {
        osd_until = 0;
        wm_dirty(osd_rect());
    }

    if (now - last_cpu_sample >= 500) {
        last_cpu_sample = now;
        int total = 0;
        for (int i = 0; i < ncpus; i++) total += (int)cpus[i].load;
        memmove(cpu_hist, cpu_hist + 1, sizeof(cpu_hist) - sizeof(int));
        cpu_hist[ARRAY_SIZE(cpu_hist) - 1] = total / MAX(1, ncpus);
        wm_dirty(tray_cpu_r);
        if (sm_open) wm_dirty(sm_rect());   /* uptime in the start menu */
    }

    /* start menu slide/fade */
    float target = sm_open ? 1.0f : 0.0f;
    if (sm_t != target) {
        float t = (float)(now - sm_anim_t0) / 160.0f;
        if (t > 1) t = 1;
        float e = 1.0f - (1.0f - t) * (1.0f - t);
        sm_t = sm_open ? e : 1.0f - e;
        if (!ui_animations) sm_t = target;
        wm_dirty(sm_full_rect());
        anim = true;
    }
    if (sm_open && sm_search[0]) {
        static bool blink;
        bool b = (now / 500) % 2 == 0;
        if (b != blink) { blink = b; wm_dirty(R(sm_rect().x, sm_rect().y + 20, SM_W, 48)); }
    }

    /* notifications */
    for (int i = 0; i < ntoasts; i++) {
        uint64_t age = now - toasts[i].t0;
        if (age < 260 || age > TOAST_LIFE - 260) {
            wm_dirty(R(SW - TOAST_W - 40, 0, TOAST_W + 40, 4 * (TOAST_H + 10) + 40));
            anim = true;
        }
        if (age >= TOAST_LIFE) {
            memmove(&toasts[i], &toasts[i + 1], sizeof(toasts[0]) * (size_t)(ntoasts - i - 1));
            ntoasts--;
            i--;
            wm_dirty(R(SW - TOAST_W - 40, 0, TOAST_W + 40, 4 * (TOAST_H + 10) + 40));
        }
    }

    /* taskbar tooltip after hovering for a moment */
    if (tb_hover >= 0 && !tooltip_shown && now - tb_hover_since > 600) {
        tooltip_shown = true;
        dirty_tooltip();
    }
    return anim || ntoasts > 0;
}

void desktop_init(int w, int h)
{
    SW = w;
    SH = h;
    wall = surface_new(SW, SH);
    wall_blur = surface_new(SW, SH);
    if (!wall || !wall_blur) panic("desktop: out of memory");
    for (int i = 0; i < 1024; i++) {
        float t = (float)i / 1023.0f;
        float up = (1.0f - t);
        up = up * up * up;
        float dn = (1.0f - t);
        dn = dn * dn * dn * dn;
        lut_up[i] = (uint8_t)(up * 200.0f);
        lut_dn[i] = (uint8_t)(dn * 230.0f);
    }
    uint64_t t0 = uptime_ms();
    gen_wallpaper(wall, wallpaper_style);
    gen_style = wallpaper_style;
    make_blur();
    klog("desktop: wallpaper '%s' generated in %lu ms", wallpaper_names[wallpaper_style], uptime_ms() - t0);
    launchers_reload();
    load_desktop_icons();
    update_clock();
    desktop_windows_changed();
}

static int session_boot(void *arg)
{
    UNUSED(arg);
    if (wallpaper_style != gen_style) {
        gen_style = wallpaper_style;
        wm_set_wallpaper(wallpaper_style);
    }
    sched_sleep(300);
    audio_sound(SND_STARTUP);
    if (!settings_welcome_seen()) app_launch("welcome");
    return 0;
}

/* a user signed in (login screen, or automatically): their settings, their desktop */
void desktop_session_started(void)
{
    static bool first = true;
    settings_load();
    launchers_reload();
    load_desktop_icons();
    desktop_windows_changed();
    wm_dirty(R(0, 0, SW, SH));
    if (first) {
        first = false;
        thread_create("session", session_boot, NULL);
    }
}

void desktop_sign_out(void)
{
    for (window_t *w = wm_list; w; w = w->next) wm_request_close(w);
    ntoasts = 0;
    session_logout();
    login_start(false);
}
