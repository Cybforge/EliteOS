/* System Monitor: per-core CPU graphs, memory, threads and processes. */
#include <wm.h>
#include <cpu.h>
#include <mm.h>
#include <sched.h>
#include <dev.h>
#include <vfs.h>

#define HISTN 90

struct sysmon {
    int tab;
    uint8_t core_hist[MAX_CPUS][HISTN];
    uint8_t total_hist[HISTN];
    uint8_t mem_hist[HISTN];
    struct thread_snapshot snap[160];
    int nsnap;
    int sel_tid;
    int scroll;
    uint64_t last_switches, switch_rate;
    uint64_t last_t;
    int sort;           /* 0 cpu, 1 name, 2 tid */
};

static void push(uint8_t *h, int v)
{
    memmove(h, h + 1, HISTN - 1);
    h[HISTN - 1] = (uint8_t)CLAMP(v, 0, 100);
}

static void sample(struct sysmon *m)
{
    int total = 0;
    for (int i = 0; i < ncpus; i++) {
        push(m->core_hist[i], (int)cpus[i].load);
        total += (int)cpus[i].load;
    }
    push(m->total_hist, total / MAX(ncpus, 1));
    uint64_t t = pmm_total_pages(), f = pmm_free_pages();
    push(m->mem_hist, (int)((t - f) * 100 / MAX(t, 1)));
    m->nsnap = sched_snapshot(m->snap, 160);
    uint64_t now = uptime_ms(), sw = sched_context_switches();
    if (m->last_t) m->switch_rate = (sw - m->last_switches) * 1000 / MAX(now - m->last_t, 1ul);
    m->last_switches = sw;
    m->last_t = now;
}

static void graph(surface_t *s, rect_t r, const uint8_t *h, color_t c, bool grid)
{
    gfx_round_rect(s, r.x, r.y, r.w, r.h, 8, HEX(0x111422));
    if (grid) {
        for (int i = 1; i < 4; i++) gfx_fill(s, r.x + 1, r.y + r.h * i / 4, r.w - 2, 1, ALPHA(0xFFFFFF, 12));
        for (int i = 1; i < 6; i++) gfx_fill(s, r.x + r.w * i / 6, r.y + 1, 1, r.h - 2, ALPHA(0xFFFFFF, 8));
    }
    surface_t sub = *s;
    sub.clip = rect_intersect(s->clip, rect_inset(r, 1));
    float step = (float)(r.w - 2) / (float)(HISTN - 1);
    int base = r.y + r.h - 2;
    float span = (float)(r.h - 6);
    /* filled area with a vertical fade */
    for (int x = 0; x < r.w - 2; x++) {
        float fi = (float)x / step;
        int i0 = (int)fi;
        int i1 = MIN(i0 + 1, HISTN - 1);
        float t = fi - (float)i0;
        float v = (float)h[i0] * (1 - t) + (float)h[i1] * t;
        int top = base - (int)(v * span / 100.0f);
        for (int y = top; y <= base; y++) {
            int a = 90 - (y - top) * 70 / MAX(1, base - top + 1);
            gfx_pixel(&sub, r.x + 1 + x, y, ALPHA(c, MAX(a, 16)));
        }
    }
    for (int i = 1; i < HISTN; i++) {
        float x0 = (float)r.x + 1 + step * (float)(i - 1), x1 = (float)r.x + 1 + step * (float)i;
        float y0 = (float)base - (float)h[i - 1] * span / 100.0f, y1 = (float)base - (float)h[i] * span / 100.0f;
        gfx_line_w(&sub, x0, y0, x1, y1, 1.6f, c);
    }
    gfx_round_outline(s, r.x, r.y, r.w, r.h, 8, ALPHA(0xFFFFFF, 18));
}

static void card(surface_t *s, rect_t r, const char *title, const char *value, const char *sub, color_t accent)
{
    gfx_round_rect(s, r.x, r.y, r.w, r.h, 12, theme.panel);
    gfx_round_outline(s, r.x, r.y, r.w, r.h, 12, ALPHA(0xFFFFFF, 14));
    gfx_round_rect(s, r.x + 14, r.y + 16, 4, r.h - 32, 2, accent);
    gfx_text(s, font_ui, r.x + 28, r.y + 12, title, theme.text_dim);
    gfx_text(s, font_bold_lg, r.x + 28, r.y + 30, value, theme.text);
    gfx_text(s, font_ui, r.x + 28, r.y + 56, sub, theme.text_faint);
}

static void perf_tab(struct sysmon *m, surface_t *s, rect_t area)
{
    char v[48], sub[64];
    int cw = (area.w - 3 * 12) / 4;
    int tot = m->total_hist[HISTN - 1];
    snprintf(v, sizeof(v), "%d%%", tot);
    snprintf(sub, sizeof(sub), "%d cores  ·  %lu MHz", ncpus, cpu_info.tsc_hz / 1000000);
    card(s, R(area.x, area.y, cw, 80), "Processor", v, sub, theme.accent);
    uint64_t t = pmm_total_pages() * PAGE_SIZE, f = pmm_free_pages() * PAGE_SIZE;
    snprintf(v, sizeof(v), "%lu MiB", (t - f) >> 20);
    snprintf(sub, sizeof(sub), "of %lu MiB", t >> 20);
    card(s, R(area.x + (cw + 12), area.y, cw, 80), "Memory", v, sub, theme.success);
    snprintf(v, sizeof(v), "%d", sched_thread_count());
    snprintf(sub, sizeof(sub), "%lu switches/s", m->switch_rate);
    card(s, R(area.x + 2 * (cw + 12), area.y, cw, 80), "Threads", v, sub, theme.warning);
    uint64_t up = uptime_ms() / 1000;
    snprintf(v, sizeof(v), "%lu:%02lu:%02lu", up / 3600, (up / 60) % 60, up % 60);
    snprintf(sub, sizeof(sub), "compositor %u fps", wm_fps());
    card(s, R(area.x + 3 * (cw + 12), area.y, cw, 80), "Uptime", v, sub, theme.danger);

    int y = area.y + 96;
    gfx_text(s, font_ui_bold, area.x, y, "CPU usage per core", theme.text);
    gfx_text_right(s, font_ui, area.x + area.w, y + 2, "last 45 seconds", theme.text_faint);
    y += 26;
    int cols = ncpus > 4 ? 4 : ncpus > 1 ? 2 : 1;
    int rows = (ncpus + cols - 1) / cols;
    int mem_h = 110;
    int gh = MAX(60, (area.y + area.h - y - mem_h - 40 - (rows - 1) * 10) / rows);
    int gw = (area.w - (cols - 1) * 12) / cols;
    for (int i = 0; i < ncpus; i++) {
        rect_t r = R(area.x + (i % cols) * (gw + 12), y + (i / cols) * (gh + 10), gw, gh);
        color_t c = color_hsv(255.0f - (float)i * 28.0f, 0.60f, 1.0f);
        graph(s, r, m->core_hist[i], c, true);
        char l[32];
        snprintf(l, sizeof(l), "CPU %d", i);
        gfx_text(s, font_ui_md, r.x + 10, r.y + 8, l, theme.text);
        snprintf(l, sizeof(l), "%u%%", m->core_hist[i][HISTN - 1]);
        gfx_text_right(s, font_ui_md, r.x + r.w - 10, r.y + 8, l, c);
    }
    y += rows * (gh + 10) + 6;
    gfx_text(s, font_ui_bold, area.x, y, "Memory", theme.text);
    y += 24;
    graph(s, R(area.x, y, area.w, area.y + area.h - y), m->mem_hist, theme.success, true);
}

static int cmp_snap(struct sysmon *m, const struct thread_snapshot *a, const struct thread_snapshot *b)
{
    if (m->sort == 1) return strcasecmp(a->name, b->name);
    if (m->sort == 2) return a->tid - b->tid;
    if (a->cpu_pct != b->cpu_pct) return (int)b->cpu_pct - (int)a->cpu_pct;
    return a->tid - b->tid;
}

static void proc_tab(struct sysmon *m, ui_t *u, surface_t *s, rect_t area)
{
    /* sort (insertion sort, n is small) */
    for (int i = 1; i < m->nsnap; i++) {
        struct thread_snapshot t = m->snap[i];
        int j = i - 1;
        while (j >= 0 && cmp_snap(m, &m->snap[j], &t) > 0) { m->snap[j + 1] = m->snap[j]; j--; }
        m->snap[j + 1] = t;
    }
    static const char *cols[] = { "Name", "TID", "PID", "CPU", "State", "Core", "Memory" };
    int cx[] = { 14, 250, 310, 370, 440, 520, 580 };
    rect_t hdr = R(area.x, area.y, area.w, 30);
    gfx_round_rect(s, hdr.x, hdr.y, hdr.w, hdr.h, 8, theme.panel);
    for (int i = 0; i < 7; i++) {
        bool sortable = i == 0 || i == 1 || i == 3;
        int si = i == 0 ? 1 : i == 1 ? 2 : 0;
        rect_t hr = R(area.x + cx[i] - 6, area.y, 70, 30);
        if (sortable && ui_list_item(u, hr, false)) { m->sort = si; u->want_repaint = true; }
        gfx_text(s, font_ui_md, area.x + cx[i], area.y + 7, cols[i], m->sort == si && sortable ? theme.accent2 : theme.text_dim);
    }
    int rh = 28, y0 = area.y + 36;
    int view = (area.h - 36 - 50) / rh;
    if (u->wheel && ui_hover(u, area)) m->scroll = CLAMP(m->scroll - u->wheel * 3, 0, MAX(0, m->nsnap - view));
    surface_t sub = *s;
    sub.clip = rect_intersect(s->clip, R(area.x, y0, area.w, view * rh));
    ui_t *uu = u;
    surface_t *saved = uu->s;
    uu->s = &sub;
    for (int i = m->scroll; i < m->nsnap && i < m->scroll + view; i++) {
        struct thread_snapshot *t = &m->snap[i];
        rect_t r = R(area.x, y0 + (i - m->scroll) * rh, area.w - 12, rh - 2);
        if (ui_list_item(uu, r, t->tid == m->sel_tid)) m->sel_tid = t->tid;
        char b[32];
        int ty = r.y + 6;
        icon_draw(&sub, t->user ? ICON_APP : ICON_SETTINGS, r.x + 8, r.y + 5, 16);
        gfx_text_ellipsis(&sub, font_ui, r.x + cx[0] + 18, ty, 200, t->name, t->user ? theme.accent2 : theme.text);
        snprintf(b, sizeof(b), "%d", t->tid);
        gfx_text(&sub, font_ui, r.x + cx[1], ty, b, theme.text_dim);
        snprintf(b, sizeof(b), "%s", t->pid ? "" : "-");
        if (t->pid) snprintf(b, sizeof(b), "%d", t->pid);
        gfx_text(&sub, font_ui, r.x + cx[2], ty, b, theme.text_dim);
        snprintf(b, sizeof(b), "%u.%u%%", t->cpu_pct / 10, t->cpu_pct % 10);
        gfx_text(&sub, font_ui_md, r.x + cx[3], ty, b, t->cpu_pct > 300 ? theme.warning : theme.text);
        const char *st = t->state == T_RUNNING ? "Running" : t->state == T_READY ? "Ready" : t->state == T_BLOCKED ? "Waiting" : "Exited";
        gfx_text(&sub, font_ui, r.x + cx[4], ty, st, t->state == T_RUNNING ? theme.success : theme.text_dim);
        snprintf(b, sizeof(b), "%d", t->cpu);
        gfx_text(&sub, font_ui, r.x + cx[5], ty, t->cpu >= 0 ? b : "-", theme.text_dim);
        snprintf(b, sizeof(b), "%lu KiB", t->mem >> 10);
        gfx_text(&sub, font_ui, r.x + cx[6], ty, b, theme.text_dim);
    }
    uu->s = saved;
    ui_scrollbar(u, R(area.x + area.w - 10, y0, 10, view * rh), &m->scroll, m->nsnap, view);
    /* footer */
    int fy = area.y + area.h - 40;
    char info[96];
    int users = 0;
    for (int i = 0; i < m->nsnap; i++) users += m->snap[i].user;
    snprintf(info, sizeof(info), "%d threads  ·  %d user processes", m->nsnap, users);
    gfx_text(s, font_ui, area.x, fy + 10, info, theme.text_faint);
    struct thread_snapshot *sel = NULL;
    for (int i = 0; i < m->nsnap; i++) if (m->snap[i].tid == m->sel_tid) sel = &m->snap[i];
    if (sel && sel->user) {
        if (ui_button(u, R(area.x + area.w - 130, fy, 130, 36), "End process", BTN_DANGER)) {
            proc_kill(sel->pid);
            m->sel_tid = 0;
        }
    } else {
        gfx_text_right(s, font_ui, area.x + area.w, fy + 10, "Select a user process to end it", theme.text_faint);
    }
}

static void sm_paint(app_t *a, surface_t *s)
{
    struct sysmon *m = a->data;
    ui_t *u = &a->ui;
    gfx_clear(s, theme.bg);
    gfx_fill(s, 0, 0, s->w, 52, theme.panel);
    gfx_fill(s, 0, 52, s->w, 1, ALPHA(0xFFFFFF, 14));
    if (ui_tab(u, R(16, 10, 130, 32), "Performance", m->tab == 0)) m->tab = 0;
    if (ui_tab(u, R(152, 10, 130, 32), "Processes", m->tab == 1)) m->tab = 1;
    rect_t area = R(20, 68, s->w - 40, s->h - 84);
    if (m->tab == 0) perf_tab(m, s, area);
    else proc_tab(m, u, s, area);
}

static int sm_event(app_t *a, struct gui_event *ev)
{
    struct sysmon *m = a->data;
    if (ev->type == EV_TIMER) { sample(m); return 1; }
    if (ev->type == EV_KEY_DOWN && ev->key == KEY_TAB) { m->tab ^= 1; return 1; }
    return 1;
}

int sysmon_main(void *arg)
{
    UNUSED(arg);
    struct sysmon *m = kzalloc(sizeof(*m));
    app_t a = { 0 };
    a.data = m;
    a.win = wm_create("System Monitor", 860, 620, WF_RESIZABLE);
    if (!a.win) { kfree(m); return 1; }
    wm_set_icon(a.win, ICON_MONITOR);
    wm_set_min_size(a.win, 640, 460);
    sample(m);
    wm_set_timer(a.win, 500);
    a.on_paint = sm_paint;
    a.on_event = sm_event;
    int r = app_run(&a);
    kfree(m);
    return r;
}
