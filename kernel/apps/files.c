/* Files: icon/list views, sidebar places, history, context menu, rename. */
#include <wm.h>
#include <sys.h>
#include <vfs.h>
#include <mm.h>
#include <dev.h>
#include <tty.h>
#include <sched.h>

#define MAXE 256

struct files {
    char path[VFS_PATH_MAX];
    char back[16][VFS_PATH_MAX];
    int nback;
    char fwd[16][VFS_PATH_MAX];
    int nfwd;
    struct vfs_dirent ent[MAXE];
    int n;
    int sel;
    int scroll;
    bool list_view;
    /* context menu */
    bool menu;
    int mx, my;
    int menu_hover;
    int menu_target;        /* -1 = background */
    /* inline prompt: 1 rename, 2 new folder, 3 new file */
    int prompt;
    char input[64];
    char status[96];
    uint64_t status_t;
};

static void reload(struct files *f)
{
    f->n = vfs_list(f->path, f->ent, MAXE);
    if (f->n < 0) f->n = 0;
    if (f->sel >= f->n) f->sel = f->n - 1;
}

static void go(struct files *f, const char *p, bool record)
{
    struct vfs_stat st;
    if (vfs_stat(p, &st) || st.type != VN_DIR) return;
    if (record && strcmp(p, f->path)) {
        if (f->nback == 16) { memmove(f->back[0], f->back[1], sizeof(f->back[0]) * 15); f->nback--; }
        strlcpy(f->back[f->nback++], f->path, VFS_PATH_MAX);
        f->nfwd = 0;
    }
    strlcpy(f->path, p, sizeof(f->path));
    f->sel = -1;
    f->scroll = 0;
    reload(f);
}

static void child(struct files *f, int i, char *out)
{
    snprintf(out, VFS_PATH_MAX, "%s/%s", strcmp(f->path, "/") ? f->path : "", f->ent[i].name);
}

static void open_entry(struct files *f, int i)
{
    char p[VFS_PATH_MAX];
    child(f, i, p);
    if (f->ent[i].type == VN_DIR) go(f, p, true);
    else open_path(p);
}

static void set_status(struct files *f, const char *s)
{
    strlcpy(f->status, s, sizeof(f->status));
    f->status_t = uptime_ms();
}

static void human(uint64_t b, char *buf, size_t n)
{
    if (b < 1024) snprintf(buf, n, "%lu B", b);
    else if (b < 1024 * 1024) snprintf(buf, n, "%lu KB", (b + 512) / 1024);
    else snprintf(buf, n, "%lu.%lu MB", b >> 20, ((b >> 10) % 1024) * 10 / 1024);
}

static const char *menu_items_file[] = { "Open", "Rename", "Duplicate", "Delete", "Copy path" };
static const char *menu_items_bg[] = { "New folder", "New text file", "Open terminal here", "Refresh" };

static int menu_count(struct files *f) { return f->menu_target >= 0 ? 5 : 4; }
static const char *menu_item(struct files *f, int i) { return f->menu_target >= 0 ? menu_items_file[i] : menu_items_bg[i]; }

static void menu_action(struct files *f, int i)
{
    char p[VFS_PATH_MAX];
    if (f->menu_target >= 0) {
        child(f, f->menu_target, p);
        switch (i) {
        case 0: open_entry(f, f->menu_target); break;
        case 1:
            f->prompt = 1;
            strlcpy(f->input, f->ent[f->menu_target].name, sizeof(f->input));
            f->sel = f->menu_target;
            break;
        case 2: {
            char d[VFS_PATH_MAX + 8];
            snprintf(d, sizeof(d), "%s copy", p);
            vfs_copy(p, d);
            reload(f);
            set_status(f, "Duplicated");
            break;
        }
        case 3:
            vfs_unlink(p, true);
            reload(f);
            set_status(f, "Deleted");
            break;
        case 4:
            clipboard_set(p, strlen(p));
            set_status(f, "Path copied to the clipboard");
            break;
        }
    } else {
        switch (i) {
        case 0: f->prompt = 2; strcpy(f->input, "New folder"); break;
        case 1: f->prompt = 3; strcpy(f->input, "notes.txt"); break;
        case 2: {
            extern int terminal_main(void *);
            char cmd[VFS_PATH_MAX + 8];
            snprintf(cmd, sizeof(cmd), "cd %s", f->path);
            thread_create_ex("Terminal", terminal_main, strdup(cmd), 0, -1, NULL, 128 * 1024);
            break;
        }
        case 3: reload(f); break;
        }
    }
}


static void commit_prompt(struct files *f)
{
    if (!f->input[0]) { f->prompt = 0; return; }
    char p[VFS_PATH_MAX];
    snprintf(p, sizeof(p), "%s/%s", strcmp(f->path, "/") ? f->path : "", f->input);
    int r = 0;
    if (f->prompt == 1 && f->sel >= 0) {
        char old[VFS_PATH_MAX];
        child(f, f->sel, old);
        r = vfs_rename(old, p);
    } else if (f->prompt == 2) {
        r = vfs_mkdir(p);
    } else if (f->prompt == 3) {
        vnode_t *v = vfs_open(p, true);
        if (v) vfs_close(v); else r = E_NOENT;
    }
    set_status(f, r ? "That did not work (name taken?)" : "Done");
    f->prompt = 0;
    reload(f);
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->ent[i].name, f->input)) f->sel = i;
}

/* the sidebar: the signed-in user's folders, then fixed system places */
struct place { const char *name; char path[VFS_PATH_MAX]; int icon; };

static int build_places(struct place *pl)
{
    int n = 0;
    static const struct { const char *name; const char *rel; int icon; } user_places[] = {
        { N_("Home"), "", ICON_HOME }, { N_("Desktop"), "Desktop", ICON_FOLDER },
        { N_("Documents"), "Documents", ICON_FOLDER }, { N_("Pictures"), "Pictures", ICON_FOLDER },
        { N_("Downloads"), "Downloads", ICON_FOLDER },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(user_places); i++) {
        pl[n].name = user_places[i].name;
        if (user_places[i].rel[0]) session_path(user_places[i].rel, pl[n].path, sizeof(pl[n].path));
        else strlcpy(pl[n].path, session_home(), sizeof(pl[n].path));
        pl[n].icon = user_places[i].icon;
        n++;
    }
    static const struct { const char *name; const char *path; int icon; } system_places[] = {
        { N_("Programs"), "/bin", ICON_FOLDER }, { N_("Devices"), "/dev", ICON_FOLDER }, { N_("Computer"), "/", ICON_FOLDER },
    };
    for (unsigned i = 0; i < ARRAY_SIZE(system_places); i++, n++) {
        pl[n].name = system_places[i].name;
        strlcpy(pl[n].path, system_places[i].path, sizeof(pl[n].path));
        pl[n].icon = system_places[i].icon;
    }
    return n;
}

static rect_t item_rect(struct files *f, rect_t area, int i, int *per_row)
{
    if (f->list_view) {
        *per_row = 1;
        return R(area.x, area.y + i * 34 - f->scroll, area.w - 12, 32);
    }
    int cw = 104, ch = 104;
    int pr = MAX(1, (area.w - 12) / cw);
    *per_row = pr;
    return R(area.x + (i % pr) * cw, area.y + (i / pr) * ch - f->scroll, cw - 6, ch - 6);
}

static void files_paint(app_t *a, surface_t *s)
{
    struct files *f = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, theme.bg);

    /* sidebar */
    int sbw = 180;
    gfx_fill(s, 0, 0, sbw, H, theme.panel);
    gfx_fill(s, sbw, 0, 1, H, ALPHA(0xFFFFFF, 12));
    gfx_text(s, font_ui_md, 18, 16, tr("Places"), theme.text_faint);
    struct place places[8];
    int nplaces = build_places(places);
    for (int i = 0; i < nplaces; i++) {
        rect_t r = R(8, 40 + i * 36, sbw - 16, 32);
        bool cur = !strcmp(f->path, places[i].path);
        if (ui_list_item(u, r, cur)) go(f, places[i].path, true);
        icon_draw(s, places[i].icon, r.x + 10, r.y + 6, 20);
        gfx_text(s, font_ui, r.x + 40, r.y + 8, tr(places[i].name), theme.text);
    }
    /* mounted disks */
    int py = 40 + nplaces * 36 + 10;
    struct fs_mount *here = NULL;
    for (struct fs_mount *m = vfs_mounts(); m; m = m->next) {
        size_t pl = strlen(m->path);
        if (!strncmp(f->path, m->path, pl) && (f->path[pl] == 0 || f->path[pl] == '/')) here = m;
    }
    if (vfs_mounts() && py + 60 < H - 70) {
        gfx_text(s, font_ui_md, 18, py, "Drives", theme.text_faint);
        py += 24;
        for (struct fs_mount *m = vfs_mounts(); m && py + 32 < H - 70; m = m->next, py += 36) {
            rect_t r = R(8, py, sbw - 16, 32);
            if (ui_list_item(u, r, !strcmp(f->path, m->path))) go(f, m->path, true);
            icon_draw(s, ICON_DISK, r.x + 10, r.y + 6, 20);
            char name[40];
            snprintf(name, sizeof(name), "%s", m->label[0] ? m->label : m->path + 1);
            gfx_text_ellipsis(s, font_ui, r.x + 40, r.y + 8, r.w - 46, name, theme.text);
        }
    }
    char line[64], used[32];
    if (here) {
        uint64_t total, free;
        here->ops->statfs(here, &total, &free);
        char fr[32];
        human(free, fr, sizeof(fr));
        human(total, used, sizeof(used));
        gfx_text_ellipsis(s, font_ui, 18, H - 72, sbw - 30, here->label[0] ? here->label : here->dev, theme.text_faint);
        snprintf(line, sizeof(line), "%s free of %s", fr, used);
        gfx_text_ellipsis(s, font_ui, 18, H - 52, sbw - 30, line, theme.text_dim);
        int bw = sbw - 36;
        int fill = total ? (int)((total - free) * (uint64_t)bw / total) : 0;
        gfx_round_rect(s, 18, H - 28, bw, 6, 3, theme.surface2);
        gfx_round_rect(s, 18, H - 28, MAX(fill, 6), 6, 3, theme.accent);
    } else {
        human(vfs_total_bytes(), used, sizeof(used));
        gfx_text(s, font_ui, 18, H - 52, "RAM disk", theme.text_faint);
        snprintf(line, sizeof(line), "%s used", used);
        gfx_text(s, font_ui, 18, H - 32, line, theme.text_dim);
    }

    /* toolbar */
    int tx = sbw + 12;
    if (ui_button(u, R(tx, 10, 34, 32), "<", BTN_FLAT) && f->nback) {
        strlcpy(f->fwd[f->nfwd++ % 16], f->path, VFS_PATH_MAX);
        char p[VFS_PATH_MAX];
        strlcpy(p, f->back[--f->nback], sizeof(p));
        go(f, p, false);
    }
    if (ui_button(u, R(tx + 38, 10, 34, 32), ">", BTN_FLAT) && f->nfwd) {
        char p[VFS_PATH_MAX];
        strlcpy(p, f->fwd[--f->nfwd], sizeof(p));
        go(f, p, true);
    }
    if (ui_button(u, R(tx + 76, 10, 34, 32), "^", BTN_FLAT)) {
        char p[VFS_PATH_MAX];
        vfs_dirname(f->path, p, sizeof(p));
        go(f, p, true);
    }
    /* breadcrumb */
    rect_t bc = R(tx + 118, 10, W - tx - 118 - 200, 32);
    gfx_round_rect(s, bc.x, bc.y, bc.w, bc.h, 8, theme.surface);
    {
        int x = bc.x + 10;
        char acc[VFS_PATH_MAX] = "";
        const char *p = f->path;
        surface_t sub = *s;
        sub.clip = rect_intersect(s->clip, bc);
        if (!strcmp(p, "/")) gfx_text(&sub, font_ui_md, x, bc.y + 8, "Computer", theme.text);
        while (*p) {
            while (*p == '/') p++;
            if (!*p) break;
            const char *e = strchr(p, '/');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            char seg[64];
            memcpy(seg, p, MIN(n, (size_t)63));
            seg[MIN(n, (size_t)63)] = 0;
            strlcat(acc, "/", sizeof(acc));
            strlcat(acc, seg, sizeof(acc));
            int w = font_text_width(font_ui_md, seg) + 12;
            rect_t sr = R(x - 6, bc.y + 3, w, 26);
            if (ui_hover(u, sr)) gfx_round_rect(&sub, sr.x, sr.y, sr.w, sr.h, 6, ALPHA(0xFFFFFF, 18));
            if (u->mreleased && ui_hover(u, sr)) go(f, acc, true);
            x = gfx_text(&sub, font_ui_md, x, bc.y + 8, seg, e ? theme.text_dim : theme.text);
            if (e) x = gfx_text(&sub, font_ui, x + 4, bc.y + 8, "›", theme.text_faint) + 8;
            p += n;
        }
    }
    if (ui_button(u, R(W - 196, 10, 88, 32), "New folder", BTN_SUBTLE)) {
        f->prompt = 2;
        strcpy(f->input, "New folder");
    }
    if (ui_button(u, R(W - 100, 10, 84, 32), f->list_view ? "Icons" : "List", BTN_SUBTLE)) {
        f->list_view = !f->list_view;
        f->scroll = 0;
    }
    gfx_fill(s, sbw + 1, 52, W - sbw, 1, ALPHA(0xFFFFFF, 12));

    /* items */
    int bottom = f->prompt ? 60 : 30;
    rect_t area = R(sbw + 16, 64, W - sbw - 24, H - 64 - bottom);
    if (f->list_view) {
        gfx_text(s, font_ui_md, area.x + 44, area.y, "Name", theme.text_faint);
        gfx_text(s, font_ui_md, area.x + area.w - 250, area.y, "Size", theme.text_faint);
        gfx_text(s, font_ui_md, area.x + area.w - 160, area.y, "Modified", theme.text_faint);
        area.y += 24;
        area.h -= 24;
    }
    surface_t sub = *s;
    sub.clip = rect_intersect(s->clip, area);
    surface_t *saved = u->s;
    u->s = &sub;
    int per_row = 1;
    int content_h = 0;
    bool hit_any = false;
    for (int i = 0; i < f->n; i++) {
        rect_t r = item_rect(f, area, i, &per_row);
        content_h = MAX(content_h, r.y + r.h + f->scroll - area.y);
        if (r.y + r.h < area.y || r.y > area.y + area.h) continue;
        if (ui_list_item(u, r, i == f->sel)) f->sel = i;
        if (ui_hover(u, r)) {
            hit_any = true;
            if (u->dclicked) { open_entry(f, i); u->dclicked = false; u->want_repaint = true; break; }
            if (u->rclicked) {
                f->menu = true; f->menu_target = i; f->sel = i;
                f->mx = u->mx; f->my = u->my; f->menu_hover = -1;
            }
        }
        struct vfs_dirent *e = &f->ent[i];
        int icon = e->type == VN_DIR ? ICON_FOLDER : file_icon_for(e->name);
        if (f->list_view) {
            icon_draw(&sub, icon, r.x + 10, r.y + 5, 22);
            gfx_text_ellipsis(&sub, font_ui, r.x + 44, r.y + 8, r.w - 320, e->name, theme.text);
            char sz[24], tm[32];
            if (e->type == VN_DIR) strcpy(sz, "—");
            else human(e->size, sz, sizeof(sz));
            struct datetime dt;
            epoch_to_datetime(e->mtime, &dt);
            snprintf(tm, sizeof(tm), "%02d.%02d.%04d %02d:%02d", dt.day, dt.month, dt.year, dt.hour, dt.minute);
            gfx_text(&sub, font_ui, r.x + r.w - 238, r.y + 8, sz, theme.text_dim);
            gfx_text(&sub, font_ui, r.x + r.w - 148, r.y + 8, tm, theme.text_dim);
        } else {
            icon_draw(&sub, icon, r.x + (r.w - 52) / 2, r.y + 8, 52);
            int tw = font_text_width(font_ui, e->name);
            gfx_text_ellipsis(&sub, font_ui, r.x + MAX(4, (r.w - tw) / 2), r.y + 70, r.w - 8, e->name, theme.text);
        }
    }
    u->s = saved;
    if (!f->n) gfx_text_center(s, font_ui_lg, area, "This folder is empty", theme.text_faint);
    int maxs = MAX(0, content_h - area.h);
    if (u->wheel && ui_hover(u, area)) f->scroll = CLAMP(f->scroll - u->wheel * 40, 0, maxs);
    if (maxs) {
        int sc = f->scroll;
        ui_scrollbar(u, R(area.x + area.w - 8, area.y, 8, area.h), &sc, content_h, area.h);
        f->scroll = sc;
    }
    if (!hit_any && ui_hover(u, area)) {
        if (u->mpressed && !f->menu) f->sel = -1;
        if (u->rclicked) { f->menu = true; f->menu_target = -1; f->mx = u->mx; f->my = u->my; f->menu_hover = -1; }
    }

    /* status bar or prompt */
    if (f->prompt) {
        int py = H - 52;
        gfx_fill(s, sbw + 1, py - 8, W - sbw, 60, theme.panel);
        const char *lbl = f->prompt == 1 ? "Rename to" : f->prompt == 2 ? "Folder name" : "File name";
        gfx_text(s, font_ui_md, sbw + 16, py + 10, lbl, theme.text_dim);
        rect_t ir = R(sbw + 110, py, W - sbw - 330, 36);
        u->focus = ui_id(ir);
        bool enter = ui_textbox(u, ir, f->input, sizeof(f->input), NULL);
        if (ui_button(u, R(W - 210, py, 90, 36), "Cancel", BTN_NORMAL)) f->prompt = 0;
        if (ui_button(u, R(W - 110, py, 94, 36), "OK", BTN_PRIMARY) || enter) commit_prompt(f);
    } else {
        char st[128];
        if (f->status[0] && uptime_ms() - f->status_t < 3000) strlcpy(st, f->status, sizeof(st));
        else if (f->sel >= 0 && f->sel < f->n) {
            char sz[24];
            human(f->ent[f->sel].size, sz, sizeof(sz));
            snprintf(st, sizeof(st), "\"%s\" selected  ·  %s", f->ent[f->sel].name,
                     f->ent[f->sel].type == VN_DIR ? "folder" : sz);
        } else snprintf(st, sizeof(st), "%d item%s", f->n, f->n == 1 ? "" : "s");
        gfx_text(s, font_ui, sbw + 16, H - 24, st, theme.text_faint);
    }

    /* context menu */
    if (f->menu) {
        int n = menu_count(f);
        rect_t m = R(f->mx, f->my, 190, n * 32 + 8);
        if (m.x + m.w > W) m.x = W - m.w - 4;
        if (m.y + m.h > H) m.y = H - m.h - 4;
        gfx_shadow(s, m.x, m.y + 3, m.w, m.h, 8, 12, 110);
        gfx_round_rect(s, m.x, m.y, m.w, m.h, 8, HEX(0x262B45));
        gfx_round_outline(s, m.x, m.y, m.w, m.h, 8, ALPHA(0xFFFFFF, 30));
        for (int i = 0; i < n; i++) {
            rect_t r = R(m.x + 4, m.y + 4 + i * 32, m.w - 8, 30);
            bool hov = ui_hover(u, r);
            if (hov) gfx_round_rect(s, r.x, r.y, r.w, r.h, 6, ALPHA(theme.accent, 110));
            const char *it = menu_item(f, i);
            gfx_text(s, font_ui, r.x + 12, r.y + 7, it, !strcmp(it, "Delete") && !hov ? theme.danger : theme.text);
            if (hov && u->mreleased) { f->menu = false; menu_action(f, i); u->want_repaint = true; }
        }
        if (u->mpressed && !ui_hover(u, m)) f->menu = false;
    }
}

static int files_event(app_t *a, struct gui_event *ev)
{
    struct files *f = a->data;
    if (ev->type == EV_TIMER) { reload(f); return 1; }
    if (ev->type != EV_KEY_DOWN || f->prompt) {
        if (f->prompt && ev->type == EV_KEY_DOWN && ev->key == KEY_ESC) f->prompt = 0;
        return 1;
    }
    if (f->menu && ev->key == KEY_ESC) { f->menu = false; return 1; }
    int per_row = 1;
    item_rect(f, R(196, 64, a->win->cw - 204, 100), 0, &per_row);
    switch (ev->key) {
    case KEY_RIGHT: f->sel = MIN(f->n - 1, f->sel + 1); break;
    case KEY_LEFT: f->sel = MAX(0, f->sel - 1); break;
    case KEY_DOWN: f->sel = MIN(f->n - 1, f->sel + per_row); break;
    case KEY_UP: f->sel = MAX(0, f->sel - per_row); break;
    case KEY_ENTER: if (f->sel >= 0) open_entry(f, f->sel); break;
    case KEY_BACKSPACE: {
        char p[VFS_PATH_MAX];
        vfs_dirname(f->path, p, sizeof(p));
        go(f, p, true);
        break;
    }
    case KEY_DELETE:
        if (f->sel >= 0) { f->menu_target = f->sel; menu_action(f, 3); }
        break;
    case KEY_F2:
        if (f->sel >= 0) { f->menu_target = f->sel; menu_action(f, 1); }
        break;
    case KEY_F5: reload(f); break;
    }
    return 1;
}

int files_main(void *arg)
{
    struct files *f = kzalloc(sizeof(*f));
    if (!f) return 1;
    strlcpy(f->path, session_home(), sizeof(f->path));
    if (arg) {
        struct vfs_stat st;
        if (!vfs_stat(arg, &st) && st.type == VN_DIR) strlcpy(f->path, arg, sizeof(f->path));
        kfree(arg);
    }
    f->sel = -1;
    reload(f);
    app_t a = { 0 };
    a.data = f;
    a.win = wm_create("Files", 900, 580, WF_RESIZABLE);
    if (!a.win) { kfree(f); return 1; }
    wm_set_icon(a.win, ICON_FILES);
    wm_set_min_size(a.win, 560, 360);
    wm_set_timer(a.win, 2000);
    a.on_paint = files_paint;
    a.on_event = files_event;
    int r = app_run(&a);
    kfree(f);
    return r;
}
