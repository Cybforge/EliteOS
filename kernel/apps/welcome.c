/* Welcome tour and About box. */
#include <wm.h>
#include <cpu.h>
#include <mm.h>
#include <dev.h>
#include <sched.h>

extern struct bootinfo *boot_info;

static void hero(surface_t *s, int w, int h, const char *title, const char *sub)
{
    gfx_vgradient(s, 0, 0, w, h, color_lerp(theme.accent, HEX(0x101328), 60), HEX(0x151827));
    /* soft circles for depth */
    gfx_circle(s, (float)w - 90, 30, 120, ALPHA(0xFFFFFF, 10));
    gfx_circle(s, (float)w - 30, (float)h, 90, ALPHA(0xFFFFFF, 8));
    logo_draw(s, 36, (h - 84) / 2, 84, false);
    gfx_text(s, font_light, 144, h / 2 - 34, title, 0xFFFFFFFFu);
    gfx_text(s, font_ui_lg, 146, h / 2 + 10, sub, ALPHA(0xFFFFFF, 200));
}

/* ------------------------------------------------------------------------ */

static void welcome_paint(app_t *a, surface_t *s)
{
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    gfx_clear(s, theme.bg);
    char sub[96];
    snprintf(sub, sizeof(sub), "Version %s \"%s\"  ·  running on %d CPU core%s", ZENITH_VERSION, ZENITH_CODENAME,
             ncpus, ncpus == 1 ? "" : "s");
    hero(s, W, 150, "Welcome to ZenithOS", sub);

    static const struct { int icon; const char *t, *d; } cards[] = {
        { ICON_MONITOR, "True multi-core", "Preemptive scheduler on every core" },
        { ICON_CUBE, "Compositor", "Blur, shadows, fades and animations" },
        { ICON_SETTINGS, "Own bootloader", "UEFI loader and kernel from scratch" },
        { ICON_FRACTAL, "Parallel apps", "3D and fractals on all cores" },
        { ICON_TERMINAL, "Ring-3 programs", "Isolated processes with syscalls" },
        { ICON_NETWORK, "TCP/IP stack", "DHCP, DNS, ping and HTTP" },
        { ICON_FILES, "Full desktop", "Files, editor, paint and more" },
        { ICON_TETRIS, "Games", "Blocks, Snake and Game of Life" },
    };
    int cols = 4, cw = (W - 48 - 3 * 14) / cols, ch = 96;
    for (int i = 0; i < 8; i++) {
        int x = 24 + (i % cols) * (cw + 14), y = 172 + (i / cols) * (ch + 14);
        gfx_round_rect(s, x, y, cw, ch, 12, theme.panel);
        gfx_round_outline(s, x, y, cw, ch, 12, ALPHA(0xFFFFFF, 14));
        icon_draw(s, cards[i].icon, x + 14, y + 14, 36);
        gfx_text(s, font_ui_bold, x + 60, y + 16, cards[i].t, theme.text);
        gfx_text_wrap(s, font_ui, R(x + 14, y + 58, cw - 24, 40), cards[i].d, theme.text_dim, 0);
    }

    int by = H - 58;
    gfx_fill(s, 0, by - 14, W, 1, ALPHA(0xFFFFFF, 12));
    gfx_text(s, font_ui, 24, by + 3, "Windows key: start menu", theme.text_faint);
    gfx_text(s, font_ui, 24, by + 21, "Ctrl+Alt+T: terminal", theme.text_faint);
    if (ui_button(u, R(W - 150, by, 126, 38), "Get started", BTN_PRIMARY)) a->quit = true;
    if (ui_button(u, R(W - 290, by, 126, 38), "Zenith 3D", BTN_NORMAL)) app_launch("demo3d");
    if (ui_button(u, R(W - 430, by, 126, 38), "Terminal", BTN_NORMAL)) app_launch("terminal");
}

int welcome_main(void *arg)
{
    UNUSED(arg);
    app_t a = { 0 };
    a.win = wm_create("Welcome", 900, 480, WF_CENTER);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_HOME);
    a.on_paint = welcome_paint;
    return app_run(&a);
}

/* ------------------------------------------------------------------------ */

static void row(surface_t *s, int x, int y, const char *k, const char *v)
{
    gfx_text(s, font_ui, x, y, k, theme.text_dim);
    gfx_text(s, font_ui_md, x + 150, y, v, theme.text);
}

static void about_paint(app_t *a, surface_t *s)
{
    int W = s->w;
    gfx_clear(s, theme.bg);
    hero(s, W, 140, ZENITH_NAME, "A 64-bit operating system built from scratch");
    char buf[128];
    int x = 36, y = 164;
    snprintf(buf, sizeof(buf), "%s \"%s\"", ZENITH_VERSION, ZENITH_CODENAME);
    row(s, x, y, "Version", buf); y += 26;
    row(s, x, y, "Kernel build", __DATE__ " " __TIME__); y += 26;
    row(s, x, y, "Processor", cpu_info.brand); y += 26;
    snprintf(buf, sizeof(buf), "%d online  ·  %lu MHz", ncpus, cpu_info.tsc_hz / 1000000);
    row(s, x, y, "Cores", buf); y += 26;
    snprintf(buf, sizeof(buf), "%lu MiB total  ·  %lu MiB free", (pmm_total_pages() * 4096) >> 20,
             (pmm_free_pages() * 4096) >> 20);
    row(s, x, y, "Memory", buf); y += 26;
    snprintf(buf, sizeof(buf), "%d x %d, 32-bit, %u fps", wm_screen_w(), wm_screen_h(), wm_fps());
    row(s, x, y, "Display", buf); y += 26;
    row(s, x, y, "Firmware", boot_info->firmware_vendor); y += 26;
    uint64_t up = uptime_ms() / 1000;
    snprintf(buf, sizeof(buf), "%lu h %lu min %lu s", up / 3600, (up / 60) % 60, up % 60);
    row(s, x, y, "Uptime", buf); y += 26;
    snprintf(buf, sizeof(buf), "%d threads  ·  %lu context switches", sched_thread_count(), sched_context_switches());
    row(s, x, y, "Scheduler", buf); y += 34;
    gfx_text_wrap(s, font_ui, R(x, y, W - 2 * x, 60),
                  "Everything above the firmware is part of ZenithOS: the UEFI bootloader, the kernel, "
                  "memory management, SMP scheduler, drivers, the compositor and all applications.",
                  theme.text_faint, 2);
}

static int about_event(app_t *a, struct gui_event *ev)
{
    UNUSED(a);
    return ev->type == EV_TIMER;
}

int about_main(void *arg)
{
    UNUSED(arg);
    app_t a = { 0 };
    a.win = wm_create("About ZenithOS", 560, 500, WF_CENTER | WF_DIALOG);
    if (!a.win) return 1;
    wm_set_icon(a.win, ICON_INFO);
    wm_set_timer(a.win, 1000);
    a.on_paint = about_paint;
    a.on_event = about_event;
    return app_run(&a);
}
