/* Images viewer, System Log, Settings, Network. */
#include <wm.h>
#include <sys.h>
#include <mm.h>
#include <sched.h>
#include <vfs.h>
#include <cpu.h>
#include <net.h>
#include <audio.h>
#include <image.h>
#include "../wm/wm_internal.h"

/* ========================================================================
 * Images
 * ====================================================================== */

struct viewer {
    char dir[VFS_PATH_MAX];
    char files[64][64];
    int nfiles;
    int cur;
    surface_t *img;
    float zoom;         /* 0 = fit */
    int panx, pany;
    int dx, dy;
    bool drag;
};

static bool is_image(const char *n) { return image_is_supported(n); }

static void viewer_scan(struct viewer *v)
{
    struct vfs_dirent de[64];
    int n = vfs_list(v->dir, de, 64);
    v->nfiles = 0;
    for (int i = 0; i < n; i++)
        if (de[i].type == VN_FILE && is_image(de[i].name)) strlcpy(v->files[v->nfiles++], de[i].name, 64);
}

static void viewer_load(struct viewer *v)
{
    surface_free(v->img);
    v->img = NULL;
    v->zoom = 0;
    v->panx = v->pany = 0;
    if (v->cur < 0 || v->cur >= v->nfiles) return;
    char p[VFS_PATH_MAX + 64];
    snprintf(p, sizeof(p), "%s/%s", v->dir, v->files[v->cur]);
    v->img = image_load(p);
}

static void viewer_paint(app_t *a, surface_t *s)
{
    struct viewer *v = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, HEX(0x0D0F1A));
    rect_t area = R(0, 0, W, H - 56);
    if (v->img) {
        float fit = MIN((float)area.w / (float)v->img->w, (float)area.h / (float)v->img->h);
        if (fit > 1) fit = 1;
        float z = v->zoom > 0 ? v->zoom : fit;
        int dw = (int)((float)v->img->w * z), dh = (int)((float)v->img->h * z);
        rect_t d = R(area.x + (area.w - dw) / 2 + v->panx, area.y + (area.h - dh) / 2 + v->pany, dw, dh);
        surface_t sub = *s;
        sub.clip = rect_intersect(s->clip, area);
        gfx_shadow(&sub, d.x, d.y + 4, d.w, d.h, 2, 16, 140);
        gfx_blit_scaled(&sub, d, v->img, R(0, 0, v->img->w, v->img->h));
    } else {
        icon_draw(s, ICON_IMAGE, W / 2 - 40, area.h / 2 - 70, 80);
        gfx_text_center(s, font_ui_lg, R(0, area.h / 2 + 20, W, 24),
                        v->nfiles ? "Could not open this picture" : "No pictures yet - press Print Screen to take one!",
                        theme.text_dim);
    }
    /* bottom bar */
    gfx_fill(s, 0, H - 56, W, 56, theme.panel);
    if (ui_button(u, R(12, H - 46, 40, 36), "‹", BTN_SUBTLE) && v->nfiles) { v->cur = (v->cur + v->nfiles - 1) % v->nfiles; viewer_load(v); }
    if (ui_button(u, R(58, H - 46, 40, 36), "›", BTN_SUBTLE) && v->nfiles) { v->cur = (v->cur + 1) % v->nfiles; viewer_load(v); }
    char info[160];
    if (v->img)
        snprintf(info, sizeof(info), "%s   ·   %d x %d   ·   %d of %d", v->files[v->cur], v->img->w, v->img->h, v->cur + 1, v->nfiles);
    else
        snprintf(info, sizeof(info), "%s", v->dir);
    surface_t sub = *s;
    sub.clip = rect_intersect(s->clip, R(110, H - 56, W - 360, 56));
    gfx_text_ellipsis(&sub, font_ui, 112, H - 36, W - 380, info, theme.text_dim);
    if (ui_button(u, R(W - 236, H - 46, 70, 36), "Fit", v->zoom == 0 ? BTN_PRIMARY : BTN_SUBTLE)) { v->zoom = 0; v->panx = v->pany = 0; }
    if (ui_button(u, R(W - 160, H - 46, 70, 36), "100%", v->zoom == 1 ? BTN_PRIMARY : BTN_SUBTLE)) v->zoom = 1;
    if (ui_button(u, R(W - 84, H - 46, 72, 36), "Edit", BTN_SUBTLE) && v->img) {
        char *p = kmalloc(VFS_PATH_MAX + 64);
        snprintf(p, VFS_PATH_MAX + 64, "%s/%s", v->dir, v->files[v->cur]);
        extern int paint_main(void *);
        thread_create_ex("Paint", paint_main, p, 0, -1, NULL, 128 * 1024);
    }
}

static int viewer_event(app_t *a, struct gui_event *ev)
{
    struct viewer *v = a->data;
    switch (ev->type) {
    case EV_KEY_DOWN:
        if (ev->key == KEY_RIGHT && v->nfiles) { v->cur = (v->cur + 1) % v->nfiles; viewer_load(v); }
        if (ev->key == KEY_LEFT && v->nfiles) { v->cur = (v->cur + v->nfiles - 1) % v->nfiles; viewer_load(v); }
        break;
    case EV_MOUSE_WHEEL:
        if (v->img) {
            float fit = MIN((float)a->win->cw / (float)v->img->w, (float)(a->win->ch - 56) / (float)v->img->h);
            float z = v->zoom > 0 ? v->zoom : MIN(fit, 1.0f);
            z *= ev->wheel > 0 ? 1.25f : 0.8f;
            v->zoom = CLAMP(z, 0.05f, 16.0f);
        }
        break;
    case EV_MOUSE_DOWN: v->drag = ev->y < a->win->ch - 56; v->dx = ev->x; v->dy = ev->y; break;
    case EV_MOUSE_UP: v->drag = false; break;
    case EV_MOUSE_MOVE:
        if (v->drag) { v->panx += ev->x - v->dx; v->pany += ev->y - v->dy; v->dx = ev->x; v->dy = ev->y; }
        break;
    case EV_TIMER: {
        int old = v->nfiles;
        viewer_scan(v);
        if (v->nfiles != old && !v->img && v->nfiles) { v->cur = v->nfiles - 1; viewer_load(v); }
        return v->nfiles != old;
    }
    }
    return 1;
}

int imageview_main(void *arg)
{
    struct viewer *v = kzalloc(sizeof(*v));
    session_path("Pictures", v->dir, sizeof(v->dir));
    const char *want = NULL;
    if (arg) {
        vfs_dirname(arg, v->dir, sizeof(v->dir));
        want = vfs_basename(arg);
    }
    viewer_scan(v);
    v->cur = v->nfiles ? v->nfiles - 1 : -1;
    if (want)
        for (int i = 0; i < v->nfiles; i++) if (!strcmp(v->files[i], want)) v->cur = i;
    viewer_load(v);
    kfree(arg);
    app_t a = { 0 };
    a.data = v;
    a.win = wm_create("Images", 820, 580, WF_RESIZABLE);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_IMAGE);
    wm_set_timer(a.win, 2000);
    a.on_paint = viewer_paint;
    a.on_event = viewer_event;
    int r = app_run(&a);
    surface_free(v->img);
    kfree(v);
    return r;
}

/* ========================================================================
 * System Log
 * ====================================================================== */

const char *memchr_(const char *s, char c, int n);

struct logview {
    const char *lines[2048];
    int lens[2048];
    char *buf;
    size_t len;
    int scroll;         /* lines from the bottom */
    char filter[48];
    bool follow;
};

static void logview_paint(app_t *a, surface_t *s)
{
    struct logview *l = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, HEX(0x0F1120));
    gfx_fill(s, 0, 0, W, 50, theme.panel);
    rect_t fr = R(12, 8, 300, 34);
    ui_textbox(u, fr, l->filter, sizeof(l->filter), "Filter...");
    ui_checkbox(u, R(330, 12, 150, 26), "Follow", &l->follow);
    char info[48];
    snprintf(info, sizeof(info), "%lu bytes", l->len);
    gfx_text_right(s, font_ui, W - 14, 18, info, theme.text_faint);

    /* collect matching lines (pointers into buf) */
    const char **lines = l->lines;
    int *lens = l->lens;
    int n = 0;
    const char *p = l->buf;
    while (p && *p && n < 2048) {
        const char *e = strchr(p, '\n');
        int len = e ? (int)(e - p) : (int)strlen(p);
        bool ok = !l->filter[0];
        if (!ok) {
            char tmp[256];
            int c = MIN(len, 255);
            memcpy(tmp, p, (size_t)c);
            tmp[c] = 0;
            ok = strstr_ci(tmp, l->filter) != NULL;
        }
        if (ok) { lines[n] = p; lens[n] = len; n++; }
        if (!e) break;
        p = e + 1;
    }
    int lh = font_height(font_mono) + 2;
    int view = (H - 60) / lh;
    if (u->wheel) { l->scroll = CLAMP(l->scroll + u->wheel * 3, 0, MAX(0, n - view)); l->follow = l->scroll == 0; }
    if (l->follow) l->scroll = 0;
    int first = MAX(0, n - view - l->scroll);
    for (int i = 0; i < view && first + i < n; i++) {
        const char *ln = lines[first + i];
        int y = 56 + i * lh;
        /* timestamp dim, rest bright */
        int ts = (ln[0] == '[') ? (int)(strchr(ln, ']') ? strchr(ln, ']') - ln + 1 : 0) : 0;
        if (ts > lens[first + i]) ts = 0;
        int x = gfx_text_n(s, font_mono, 12, y, ln, ts, theme.text_faint);
        const char *rest = ln + ts;
        int rl = lens[first + i] - ts;
        /* subsystem prefix in accent */
        const char *colon = memchr_(rest, ':', rl);
        if (colon && colon - rest < 14) {
            x = gfx_text_n(s, font_mono, x, y, rest, (int)(colon - rest) + 1, theme.accent2);
            rl -= (int)(colon - rest) + 1;
            rest = colon + 1;
        }
        gfx_text_n(s, font_mono, x, y, rest, rl, theme.text);
    }
}

const char *memchr_(const char *s, char c, int n)
{
    for (int i = 0; i < n; i++) if (s[i] == c) return s + i;
    return NULL;
}

static int logview_event(app_t *a, struct gui_event *ev)
{
    struct logview *l = a->data;
    if (ev->type == EV_TIMER) {
        size_t old = l->len;
        l->len = klog_read(l->buf, 65536);
        return l->len != old;
    }
    return 1;
}

int logview_main(void *arg)
{
    UNUSED(arg);
    struct logview *l = kzalloc(sizeof(*l));
    l->buf = kmalloc(65536);
    l->len = klog_read(l->buf, 65536);
    l->follow = true;
    app_t a = { 0 };
    a.data = l;
    a.win = wm_create("System Log", 860, 520, WF_RESIZABLE);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_LOG);
    wm_set_timer(a.win, 1000);
    a.on_paint = logview_paint;
    a.on_event = logview_event;
    int r = app_run(&a);
    kfree(l->buf);
    kfree(l);
    return r;
}

/* ========================================================================
 * Network
 * ====================================================================== */

struct netapp {
    volatile bool closed;
    char host[64];
    char log[16][96];
    int nlog;
    volatile bool busy;
    struct app *app;
};

static void netlog(struct netapp *n, const char *fmt, ...)
{
    char b[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n->nlog == 16) { memmove(n->log[0], n->log[1], sizeof(n->log[0]) * 15); n->nlog--; }
    strlcpy(n->log[n->nlog++], b, sizeof(n->log[0]));
    if (n->closed) return;
    struct gui_event ev = { 0 };
    ev.type = EV_TIMER;
    wm_post_event(n->app->win, &ev);
}

static int net_worker(void *arg)
{
    struct netapp *n = arg;
    char ip[20];
    if (!n->host[0]) {
        netlog(n, "Requesting an address via DHCP...");
        if (net_dhcp(4000)) {
            struct net_info ni;
            net_get_info(&ni);
            ip_to_str(ni.ip, ip);
            netlog(n, "Got address %s", ip);
        } else netlog(n, "No DHCP answer.");
    } else {
        uint32_t a;
        netlog(n, "Resolving %s...", n->host);
        if (!net_resolve(n->host, &a, 3000)) { netlog(n, "Could not resolve %s", n->host); n->busy = false; return 0; }
        ip_to_str(a, ip);
        for (int i = 0; i < 4; i++) {
            int rtt = net_ping(a, (uint16_t)i, 2000);
            if (rtt >= 0) netlog(n, "Reply from %s: seq=%d time=%d ms", ip, i, rtt);
            else netlog(n, "Request to %s timed out", ip);
            sched_sleep(400);
        }
    }
    n->busy = false;
    return 0;
}

static void net_paint(app_t *a, surface_t *s)
{
    struct netapp *n = a->data;
    ui_t *u = &a->ui;
    int W = s->w;
    gfx_clear(s, theme.bg);
    struct net_info ni;
    bool have = net_get_info(&ni);
    icon_draw(s, ICON_NETWORK, 24, 20, 40);
    gfx_text(s, font_title, 80, 22, have ? ni.driver : "No network adapter", theme.text);
    if (have) {
        char ip[20], b[128];
        ip_to_str(ni.ip, ip);
        snprintf(b, sizeof(b), "%s  ·  %s  ·  rx %lu / tx %lu packets", ni.link ? "Link up" : "Link down",
                 ni.ip ? ip : "no address", ni.rx_packets, ni.tx_packets);
        gfx_text(s, font_ui, 80, 48, b, theme.text_dim);
    }
    int y = 90;
    if (ui_button(u, R(24, y, 150, 38), "DHCP", BTN_PRIMARY) && !n->busy && have) {
        n->busy = true;
        n->host[0] = 0;
        thread_create("netapp", net_worker, n);
    }
    static char host[64];
    static bool host_init;
    if (!host_init && have && ni.gateway) {            /* start with the router, which is always worth a ping */
        ip_to_str(ni.gateway, host);
        host_init = true;
    }
    ui_textbox(u, R(190, y, W - 330, 38), host, sizeof(host), tr("host to ping"));
    if (ui_button(u, R(W - 124, y, 100, 38), "Ping", BTN_NORMAL) && !n->busy && have) {
        n->busy = true;
        strlcpy(n->host, host, sizeof(n->host));
        thread_create("netapp", net_worker, n);
    }
    y += 56;
    gfx_round_rect(s, 24, y, W - 48, s->h - y - 20, 10, HEX(0x0F1120));
    for (int i = 0; i < n->nlog; i++) gfx_text(s, font_mono, 36, y + 12 + i * 18, n->log[i], theme.text);
    if (n->busy) gfx_text_right(s, font_ui, W - 36, y + 12, "working...", theme.accent2);
}

static int net_event(app_t *a, struct gui_event *ev)
{
    UNUSED(a); UNUSED(ev);
    return 1;
}

static void net_close(app_t *a)
{
    struct netapp *n = a->data;
    n->closed = true;
    while (n->busy) sched_sleep(50);
    a->quit = true;
}

int netinfo_main(void *arg)
{
    UNUSED(arg);
    struct netapp *n = kzalloc(sizeof(*n));
    app_t a = { 0 };
    n->app = &a;
    a.data = n;
    a.win = wm_create("Network", 640, 460, 0);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_NETWORK);
    wm_set_timer(a.win, 1000);
    a.on_paint = net_paint;
    a.on_event = net_event;
    a.on_close = net_close;
    int r = app_run(&a);
    kfree(n);
    return r;
}
