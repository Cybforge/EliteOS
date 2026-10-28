/*
 * Store: find, install, update and remove software.
 *
 * Everything here comes from the package sources in /etc/zenith/sources.list
 * (see pkg/pkg.c): the list is read when the store opens, a search looks
 * through all of them, and before anything is downloaded the store says
 * whether it will run on this computer and what else it needs.
 */
#include <wm.h>
#include <vfs.h>
#include <mm.h>
#include <sched.h>
#include <net.h>
#include <sys.h>
#include <pkg.h>

#define MAX_RES 256

enum { W_NONE, W_UPDATE, W_INSTALL, W_REMOVE };

struct store {
    char query[64];
    char category[24];                  /* "" = everything, "@installed" = what is installed */
    char shown_query[64], shown_category[24];
    int shown_gen;
    struct pkg_brief *res;
    int nres;
    int scroll;
    char cats[16][24];
    int ccount[16];
    int ncats;
    int total;                          /* known packages */
    /* details */
    bool detail;
    struct pkg_brief cur;
    struct pkg full;
    struct pkg_verdict verdict;
    bool have_verdict;
    /* background work */
    struct pkg_job job;
    volatile int working;
    char work_name[48];
    volatile int generation;            /* bumped whenever the lists or what is installed changed */
    uint64_t done_at;                   /* when the last job finished */
    bool updated_once;
    int sources_n;
    struct pkg_source srcs[8];
};

static int worker(void *arg)
{
    struct store *st = arg;
    int kind = st->working;
    char name[48];
    strlcpy(name, st->work_name, sizeof(name));
    if (kind == W_UPDATE) pkg_update(&st->job);
    else if (kind == W_INSTALL) pkg_install(name, &st->job);
    else if (kind == W_REMOVE) pkg_remove(name, &st->job);
    launchers_reload();
    st->generation++;
    st->job.finished = true;
    return 0;
}

static void start_work(struct store *st, int kind, const char *name)
{
    if (st->working && !st->job.finished) return;
    memset(&st->job, 0, sizeof(st->job));
    strlcpy(st->work_name, name, sizeof(st->work_name));
    st->working = kind;
    thread_create("store", worker, st);
}

static void refresh(struct store *st)
{
    st->nres = pkg_search_brief(st->query, st->category[0] == '@' ? "" : st->category, !strcmp(st->category, "@installed"),
                                st->res, MAX_RES);
    st->ncats = pkg_categories(st->cats, st->ccount, 16);
    st->total = pkg_count();
    const struct pkg_source *s;
    int n = pkg_sources(&s);
    st->sources_n = MIN(n, 8);
    memcpy(st->srcs, s, sizeof(st->srcs[0]) * (size_t)st->sources_n);
    strlcpy(st->shown_query, st->query, sizeof(st->shown_query));
    strlcpy(st->shown_category, st->category, sizeof(st->shown_category));
    st->shown_gen = st->generation;
    st->scroll = 0;
    if (st->detail) {                                       /* an open page follows what was installed or removed */
        struct pkg_brief *one = kzalloc(4 * sizeof(*one));
        if (one) {
            int n = pkg_search_brief(st->cur.name, "", false, one, 4);
            for (int i = 0; i < n; i++)
                if (!strcmp(one[i].name, st->cur.name)) { st->cur = one[i]; break; }
            n = pkg_search_brief(st->cur.name, "", true, one, 4);
            bool inst = false;
            for (int i = 0; i < n; i++) if (!strcmp(one[i].name, st->cur.name)) inst = true;
            st->cur.installed = inst;
            pkg_verdict_for(st->cur.name, &st->verdict);
            kfree(one);
        }
    }
}

static int cat_icon(const char *cat, const char *type)
{
    if (!strcmp(type, "driver")) return ICON_MONITOR;
    if (!strcasecmp(cat, "Games")) return ICON_GAME;
    if (!strcasecmp(cat, "Internet") || !strcasecmp(cat, "Network")) return ICON_BROWSER;
    if (!strcasecmp(cat, "Graphics")) return ICON_PAINT;
    if (!strcasecmp(cat, "Multimedia") || !strcasecmp(cat, "Audio")) return ICON_PIANO;
    if (!strcasecmp(cat, "Development")) return ICON_EDITOR;
    if (!strcasecmp(cat, "System") || !strcasecmp(cat, "Tools") || !strcasecmp(cat, "Utilities")) return ICON_SETTINGS;
    return ICON_APP;
}

static void open_detail(struct store *st, const struct pkg_brief *b)
{
    st->detail = true;
    st->cur = *b;
    st->have_verdict = false;
    pkg_full(b->name, &st->full);
    pkg_verdict_for(b->name, &st->verdict);
    st->have_verdict = true;
}

static void size_str(uint64_t b, char *out, size_t n)
{
    if (b >= (1ull << 30)) snprintf(out, n, "%lu.%lu GiB", b >> 30, (b >> 27) % 10 * 10 / 8);
    else if (b >= (1ull << 20)) snprintf(out, n, "%lu.%lu MiB", b >> 20, (b >> 17) % 10 * 10 / 8);
    else if (b >= 1024) snprintf(out, n, "%lu KiB", b >> 10);
    else snprintf(out, n, "%lu B", b);
}

static void open_package(const char *name)
{
    char prefix[80];
    snprintf(prefix, sizeof(prefix), "/opt/%s/", name);
    for (int i = 0; i < launchers_count(); i++) {
        const struct launcher *l = launcher_get(i);
        if (!strncmp(l->exec, prefix, strlen(prefix))) { launcher_run(l); return; }
    }
    wm_notify("Store", "This package has no program to open", ICON_STORE);
}

static color_t verdict_color(int code)
{
    switch (code) {
    case PV_OK: return theme.success;
    case PV_NEEDS_DEPS: return theme.accent;
    default: return theme.danger;
    }
}

/* ------------------------------------------------------------------------ */

static void paint_job(struct store *st, ui_t *u, surface_t *s, rect_t r)
{
    struct pkg_job *j = &st->job;
    gfx_round_rect(s, r.x, r.y, r.w, r.h, 12, theme.panel);
    const char *what = st->working == W_UPDATE ? tr("Refreshing the package lists")
                       : st->working == W_INSTALL ? tr("Installing") : tr("Removing");
    char t[120];
    if (st->working == W_UPDATE) snprintf(t, sizeof(t), "%s", what);
    else snprintf(t, sizeof(t), "%s %s", what, st->work_name);
    gfx_text(s, font_ui_bold, r.x + 16, r.y + 12, t, theme.text);
    gfx_round_rect(s, r.x + 16, r.y + 38, r.w - 32, 8, 4, theme.surface2);
    int pct = j->finished ? 100 : CLAMP(j->percent, 0, 100);
    gfx_round_rect(s, r.x + 16, r.y + 38, MAX(8, (r.w - 32) * pct / 100), 8, 4, j->finished && j->result ? theme.danger : theme.accent);
    gfx_text_ellipsis(s, font_ui, r.x + 16, r.y + 54, r.w - 32, j->stage, theme.text_dim);
    int ly = r.y + 78;
    int first = MAX(0, j->nlog - 6);
    for (int i = first; i < j->nlog; i++, ly += 18)
        gfx_text_ellipsis(s, font_ui, r.x + 16, ly, r.w - 32, j->log[i], theme.text_faint);
    if (j->finished) {
        if (j->result) {
            gfx_text_wrap(s, font_ui_md, R(r.x + 16, ly + 6, r.w - 32, 60), j->error, theme.danger, 2);
        } else {
            gfx_text(s, font_ui_md, r.x + 16, ly + 6, tr("Done"), theme.success);
            if (!st->done_at) st->done_at = uptime_ms();
            if (uptime_ms() - st->done_at > 2500) { st->working = W_NONE; st->done_at = 0; }      /* success needs no click */
            else u->want_repaint = true;
        }
        if (ui_button(u, R(r.x + r.w - 116, r.y + r.h - 46, 100, 34), tr("OK"), BTN_PRIMARY)) { st->working = W_NONE; st->done_at = 0; }
    } else {
        u->want_repaint = true;
        if (ui_button(u, R(r.x + r.w - 116, r.y + r.h - 46, 100, 34), tr("Cancel"), BTN_NORMAL)) j->cancel = true;
    }
}

static void paint_detail(struct store *st, ui_t *u, surface_t *s, int x, int y, int w, int h)
{
    const struct pkg_brief *b = &st->cur;
    if (ui_button(u, R(x, y, 90, 32), tr("Back"), BTN_NORMAL)) { st->detail = false; return; }
    y += 46;
    int ic = st->full.icon[0] ? icon_by_name(st->full.icon) : 0;
    icon_draw(s, ic ? ic : cat_icon(b->category, b->type), x, y, 64);
    gfx_text(s, font_bold_lg, x + 84, y + 2, b->title, theme.text);
    char line[160], sz[24];
    size_str(st->verdict.download ? st->verdict.download : b->size, sz, sizeof(sz));
    snprintf(line, sizeof(line), "%s  ·  %s  ·  %s  ·  %s", b->version, b->category, b->source, sz);
    gfx_text(s, font_ui, x + 84, y + 38, line, theme.text_dim);

    /* the action */
    rect_t br = R(x + w - 170, y + 8, 160, 40);
    bool busy = st->working && !st->job.finished;
    if (!busy && st->working == W_NONE) {
        if (b->installed && !b->update) {
            if (ui_button(u, R(x + w - 170 - 130, y + 8, 120, 40), tr("Remove"), BTN_NORMAL)) start_work(st, W_REMOVE, b->name);
            if (ui_button(u, br, tr("Open"), BTN_PRIMARY)) open_package(b->name);
        } else if (b->installed) {
            if (ui_button(u, R(x + w - 170 - 130, y + 8, 120, 40), tr("Remove"), BTN_NORMAL)) start_work(st, W_REMOVE, b->name);
            if (ui_button(u, br, tr("Update"), BTN_PRIMARY)) start_work(st, W_INSTALL, b->name);
        } else if (st->have_verdict && (st->verdict.code == PV_OK || st->verdict.code == PV_NEEDS_DEPS)) {
            if (ui_button(u, br, tr("Install"), BTN_PRIMARY)) start_work(st, W_INSTALL, b->name);
        } else {
            ui_button(u, br, tr("Install"), BTN_SUBTLE);
        }
    }
    y += 82;

    if (st->have_verdict && !b->installed) {
        rect_t vr = R(x, y, w, 50);
        gfx_round_rect(s, vr.x, vr.y, vr.w, vr.h, 10, color_lerp(verdict_color(st->verdict.code), theme.bg, 215));
        gfx_circle(s, (float)vr.x + 24, (float)vr.y + 25, 7, verdict_color(st->verdict.code));
        gfx_text_wrap(s, font_ui_md, R(vr.x + 44, vr.y + 8, vr.w - 60, 36), st->verdict.text, theme.text, 2);
        y += 62;
    }
    gfx_text_wrap(s, font_ui_md, R(x, y, w, 80), b->desc, theme.text, 4);
    y += 70;
    if (st->full.homepage[0]) {
        gfx_text(s, font_ui, x, y, st->full.homepage, theme.accent);
        y += 26;
    }
    if (st->full.ndeps) {
        gfx_text(s, font_ui_bold, x, y, tr("Needs"), theme.text);
        char deps[300] = "";
        for (int i = 0; i < st->full.ndeps; i++) {
            if (i) strlcat(deps, ", ", sizeof(deps));
            strlcat(deps, st->full.depends[i], sizeof(deps));
        }
        gfx_text_wrap(s, font_ui, R(x + 70, y + 1, w - 70, 40), deps, theme.text_dim, 2);
        y += 44;
    }
    if (!st->full.verified && st->have_verdict) {
        gfx_text(s, font_ui, x, y, tr("This source is not signed by a trusted key."), theme.warning);
        y += 26;
    }
    if (st->working) {
        int jh = MIN(h - (y - 0) - 8, 230);
        paint_job(st, u, s, R(x, MAX(y, h - 240), w, MAX(jh, 200)));
    }
}

static void paint_list(struct store *st, ui_t *u, surface_t *s, int x, int y, int w, int h)
{
    int rowh = 84;
    int viewh = h - y;
    if (!st->nres) {
        const char *msg = st->total ? tr("Nothing matches. Try another search.")
                          : st->working == W_UPDATE ? tr("Reading the package lists ...")
                          : tr("No package lists yet. Check the network and press Refresh.");
        gfx_text(s, font_ui_lg, x, y + 30, msg, theme.text_dim);
        const struct pkg_source *src;
        int n = pkg_sources(&src);
        int yy = y + 70;
        for (int i = 0; i < n; i++, yy += 24) {
            char t[200];
            snprintf(t, sizeof(t), "%s  %s%s%s", src[i].name, src[i].url, src[i].error[0] ? "  -  " : "", src[i].error);
            gfx_text_ellipsis(s, font_ui, x, yy, w, t, src[i].error[0] ? theme.warning : theme.text_faint);
        }
        return;
    }
    int content = st->nres * rowh;
    if (ui_hover(u, R(x, y, w, viewh)) && u->wheel) {
        st->scroll = CLAMP(st->scroll - u->wheel * 50, 0, MAX(0, content - viewh));
        u->wheel = 0;
    }
    st->scroll = CLAMP(st->scroll, 0, MAX(0, content - viewh));
    surface_set_clip(s, R(x, y, w, viewh));
    for (int i = 0; i < st->nres; i++) {
        int ry = y + i * rowh - st->scroll;
        if (ry + rowh < y || ry > y + viewh) continue;
        const struct pkg_brief *b = &st->res[i];
        rect_t card = R(x, ry, w - 14, rowh - 8);
        rect_t click = R(card.x, card.y, card.w - 150, card.h);
        if (ui_list_item(u, click, false)) { open_detail(st, b); break; }
        gfx_round_rect(s, card.x, card.y, card.w, card.h, 12, ui_hover(u, card) ? theme.surface : theme.panel);
        int ic = cat_icon(b->category, b->type);
        icon_draw(s, ic, card.x + 14, card.y + 14, 48);
        char t[120];
        snprintf(t, sizeof(t), "%s", b->title);
        int tx = gfx_text(s, font_ui_bold, card.x + 78, card.y + 12, t, theme.text);          /* returns where the text ended */
        gfx_text(s, font_ui, tx + 10, card.y + 14, b->version, theme.text_faint);
        gfx_text_ellipsis(s, font_ui, card.x + 78, card.y + 36, card.w - 78 - 160, b->desc, theme.text_dim);
        char meta[100];
        snprintf(meta, sizeof(meta), "%s%s%s", b->category, b->type[0] && strcmp(b->type, "app") ? "  ·  " : "",
                 strcmp(b->type, "app") ? b->type : "");
        gfx_text(s, font_ui, card.x + 78, card.y + 56, meta, theme.text_faint);
        /* the state on the right */
        rect_t br = R(card.x + card.w - 130, card.y + (card.h - 34) / 2, 116, 34);
        bool busy = st->working && !st->job.finished;
        const char *label = b->update ? tr("Update") : b->installed ? tr("Installed") : tr("Get");
        if (b->installed && !b->update) ui_button(u, br, label, BTN_SUBTLE);
        else if (ui_button(u, br, label, BTN_PRIMARY) && !busy) { open_detail(st, b); break; }
    }
    surface_reset_clip(s);
    ui_scrollbar(u, R(x + w - 8, y, 8, viewh), &st->scroll, content, viewh);
}

static void store_paint(app_t *a, surface_t *s)
{
    struct store *st = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, theme.bg);

    /* first start: read the lists in the background */
    if (!st->updated_once) {
        st->updated_once = true;
        start_work(st, W_UPDATE, "");
    }
    if (st->working && st->job.finished && st->working == W_UPDATE) st->working = W_NONE;
    if (strcmp(st->query, st->shown_query) || strcmp(st->category, st->shown_category) || st->shown_gen != st->generation || !st->res)
        refresh(st);

    /* header */
    gfx_vgradient(s, 0, 0, W, 64, color_lerp(theme.accent, theme.bg, 60), theme.bg);
    icon_draw(s, ICON_STORE, 20, 12, 40);
    gfx_text(s, font_title, 72, 16, tr("Store"), theme.text);
    ui_textbox(u, R(210, 14, W - 210 - 70, 36), st->query, sizeof(st->query), tr("Search apps, games and drivers"));
    if (ui_icon_button(u, R(W - 52, 14, 36, 36), ICON_RESTART, tr("Refresh the lists")) && !st->working) {
        start_work(st, W_UPDATE, "");
    }

    /* sidebar */
    int sw = 200;
    gfx_fill(s, 0, 64, sw, H - 64, ALPHA(0xFFFFFF, 6));
    int y = 76;
    struct { const char *label; const char *key; } fixed[] = { { N_("Discover"), "" }, { N_("Installed"), "@installed" } };
    for (int i = 0; i < 2; i++) {
        rect_t r = R(10, y, sw - 20, 34);
        if (ui_list_item(u, r, !strcmp(st->category, fixed[i].key) && !st->detail)) { strlcpy(st->category, fixed[i].key, sizeof(st->category)); st->detail = false; }
        gfx_text(s, font_ui_md, r.x + 14, r.y + 8, tr(fixed[i].label), theme.text);
        y += 38;
    }
    y += 8;
    gfx_text(s, font_ui, 24, y, tr("Categories"), theme.text_faint);
    y += 22;
    for (int i = 0; i < st->ncats; i++) {
        rect_t r = R(10, y, sw - 20, 32);
        if (ui_list_item(u, r, !strcasecmp(st->category, st->cats[i]) && !st->detail)) { strlcpy(st->category, st->cats[i], sizeof(st->category)); st->detail = false; }
        gfx_text(s, font_ui_md, r.x + 14, r.y + 7, st->cats[i], theme.text);
        char c[12];
        snprintf(c, sizeof(c), "%d", st->ccount[i]);
        gfx_text_right(s, font_ui, r.x + r.w - 12, r.y + 8, c, theme.text_faint);
        y += 34;
    }
    /* sources at the bottom */
    int sy = H - 28 - st->sources_n * 20;
    gfx_text(s, font_ui, 24, sy - 22, tr("Sources"), theme.text_faint);
    for (int i = 0; i < st->sources_n; i++) {
        const struct pkg_source *src = &st->srcs[i];
        gfx_circle(s, 30, (float)sy + i * 20 + 8, 4, src->error[0] ? theme.warning : src->count ? theme.success : theme.text_faint);
        char t[60];
        snprintf(t, sizeof(t), "%s  (%d)", src->name, src->count);
        gfx_text_ellipsis(s, font_ui, 42, sy + i * 20, sw - 54, t, theme.text_dim);
    }

    int cx = sw + 24, cw = W - cx - 24;
    if (st->detail) {
        paint_detail(st, u, s, cx, 84, cw, H - 20);
    } else {
        char title[80];
        if (st->query[0]) snprintf(title, sizeof(title), "%s \"%s\"", tr("Results for"), st->query);
        else if (!st->category[0]) snprintf(title, sizeof(title), "%s", tr("Discover"));
        else if (!strcmp(st->category, "@installed")) snprintf(title, sizeof(title), "%s", tr("Installed"));
        else snprintf(title, sizeof(title), "%s", st->category);
        gfx_text(s, font_bold_lg, cx, 76, title, theme.text);
        char cnt[48];
        snprintf(cnt, sizeof(cnt), "%d", st->nres);
        gfx_text_right(s, font_ui, W - 24, 84, cnt, theme.text_faint);
        if (st->working) {
            paint_job(st, u, s, R(cx, 120, cw, 210));
            paint_list(st, u, s, cx, 344, cw, H - 12);
        } else {
            paint_list(st, u, s, cx, 120, cw, H - 12);
        }
    }
    if (st->working && !st->job.finished) u->want_repaint = true;
}

static void store_close(app_t *a)
{
    struct store *st = a->data;
    if (st->working && !st->job.finished) { st->job.cancel = true; return; }
    a->quit = true;
}

int store_main(void *arg)
{
    UNUSED(arg);
    struct store *st = kzalloc(sizeof(*st));
    st->res = kzalloc(MAX_RES * sizeof(*st->res));
    app_t a = { 0 };
    a.data = st;
    a.win = wm_create(tr("Store"), 1040, 680, WF_CENTER | WF_RESIZABLE);
    if (!a.win) { kfree(st->res); kfree(st); return 1; }
    wm_set_icon(a.win, ICON_STORE);
    wm_set_min_size(a.win, 760, 480);
    a.on_paint = store_paint;
    a.on_close = store_close;
    int r = app_run(&a);
    while (st->working && !st->job.finished) sched_sleep(50);
    kfree(st->res);
    kfree(st);
    return r;
}
