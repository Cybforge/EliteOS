/*
 * Application registry and the generic app event loop.
 */
#include <wm.h>
#include <sched.h>
#include <dev.h>
#include <vfs.h>
#include <mm.h>
#include <image.h>
#include <audio.h>
#include <sys.h>

int welcome_main(void *arg);
int about_main(void *arg);
int terminal_main(void *arg);
int files_main(void *arg);
int editor_main(void *arg);
int sysmon_main(void *arg);
int demo3d_main(void *arg);
int mandel_main(void *arg);
int tetris_main(void *arg);
int snake_main(void *arg);
int paint_main(void *arg);
int calc_main(void *arg);
int clock_main(void *arg);
int imageview_main(void *arg);
int logview_main(void *arg);
int settings_main(void *arg);
int netinfo_main(void *arg);
int browser_main(void *arg);
int installer_main(void *arg);
int piano_main(void *arg);
int chess_main(void *arg);
int mines_main(void *arg);
int g2048_main(void *arg);
int user_app_main(void *arg);

const struct app_info app_table[] = {
    { "files",    "Files",          "Browse and manage your files",      ICON_FILES,    files_main,     true },
    { "terminal", "Terminal",       "Command line shell",                ICON_TERMINAL, terminal_main,  true },
    { "editor",   "Text Editor",    "Write notes and code",              ICON_EDITOR,   editor_main,    true },
    { "sysmon",   "System Monitor", "Per-core CPU, memory, processes",   ICON_MONITOR,  sysmon_main,    true },
    { "demo3d",   "Zenith 3D",      "Real-time software 3D renderer",    ICON_CUBE,     demo3d_main,    true },
    { "mandel",   "Mandelbrot",     "Fractal explorer on all CPU cores", ICON_FRACTAL,  mandel_main,    true },
    { "tetris",   "Blocks",         "Falling-blocks puzzle game",        ICON_TETRIS,   tetris_main,    true },
    { "snake",    "Snake",          "The classic snake game",            ICON_SNAKE,    snake_main,     true },
    { "chess",    "Chess",          "Play against a multi-core engine",  ICON_CHESS,    chess_main,     true },
    { "mines",    "Minesweeper",    "Clear the field without a bang",    ICON_MINES,    mines_main,     true },
    { "2048",     "2048",           "Slide and merge the tiles",         ICON_2048,     g2048_main,     true },
    { "piano",    "Piano",          "Play music on eight instruments",   ICON_PIANO,    piano_main,     true },
    { "paint",    "Paint",          "Draw with brushes and colors",      ICON_PAINT,    paint_main,     true },
    { "calc",     "Calculator",     "Scientific calculator",             ICON_CALC,     calc_main,      true },
    { "clock",    "Clock",          "Clock, calendar and stopwatch",     ICON_CLOCK,    clock_main,     true },
    { "images",   "Images",         "View pictures and screenshots",     ICON_IMAGE,    imageview_main, true },
    { "plasma",   "Plasma",         "Ring-3 user program demo",          ICON_PLASMA,   user_app_main,  true },
    { "logs",     "System Log",     "Kernel messages",                   ICON_LOG,      logview_main,   true },
    { "browser",  "Zenith Web",     "Browse the web over HTTP",          ICON_BROWSER,  browser_main,   true },
    { "netinfo",  "Network",        "Network status and tools",          ICON_NETWORK,  netinfo_main,   true },
    { "settings", "Settings",       "Personalize ZenithOS",              ICON_SETTINGS, settings_main,  true },
    { "installer", "Install ZenithOS", "Copy the system to a disk",       ICON_INSTALL,  installer_main, true },
    { "about",    "About",          "About this computer",               ICON_INFO,     about_main,     true },
    { "welcome",  "Welcome",        "Tour of ZenithOS",                  ICON_HOME,     welcome_main,   true },
};
const int app_count = ARRAY_SIZE(app_table);

const struct app_info *app_find(const char *id)
{
    for (int i = 0; i < app_count; i++)
        if (!strcmp(app_table[i].id, id)) return &app_table[i];
    return NULL;
}

/* ------------------------------------------------------------------------
 * special commands run on their own thread (the caller may hold wm_mtx)
 * ---------------------------------------------------------------------- */

int user_app_main(void *arg)
{
    UNUSED(arg);
    char *argv[] = { "/bin/plasma", NULL };
    if (!proc_spawn("/bin/plasma", 1, argv, NULL, session_home()))
        wm_notify("Could not start Plasma", "/bin/plasma is missing", ICON_PLASMA);
    return 0;
}

static int cmd_thread(void *arg)
{
    const char *cmd = arg;
    if (!strcmp(cmd, "@wallpaper")) {
        wm_set_wallpaper(wallpaper_style);
    } else if (!strcmp(cmd, "@reboot")) {
        sched_sleep(200);
        system_reboot();
    } else if (!strcmp(cmd, "@poweroff")) {
        sched_sleep(200);
        system_poweroff();
    }
    return 0;
}

struct launch_arg {
    char path[256];
};

void app_launch(const char *name)
{
    if (name[0] == '@') {
        thread_create("command", cmd_thread, (void *)name);
        return;
    }
    const struct app_info *a = app_find(name);
    if (!a) {
        klog("app: unknown app '%s'", name);
        return;
    }
    thread_create_ex(a->name, a->main, NULL, 0, -1, NULL, 128 * 1024);
}

static void launch_with(const char *id, const char *path)
{
    const struct app_info *a = app_find(id);
    if (!a) return;
    char *p = strdup(path);
    thread_create_ex(a->name, a->main, p, 0, -1, NULL, 128 * 1024);
}

static bool ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}

int file_icon_for(const char *name)
{
    if (image_is_supported(name)) return ICON_IMAGE;
    if (ends_with(name, ".html") || ends_with(name, ".htm")) return ICON_BROWSER;
    if (ends_with(name, ".wav")) return ICON_SOUND;
    if (ends_with(name, ".txt") || ends_with(name, ".md") || ends_with(name, ".c") || ends_with(name, ".h") ||
        ends_with(name, ".cfg") || ends_with(name, ".sh") || ends_with(name, ".log"))
        return ICON_TEXT;
    if (!strchr(name, '.')) return ICON_APP;
    return ICON_FILE;
}

/* open a file with the right app */
void open_path(const char *path)
{
    if (!strncasecmp(path, "http://", 7) || !strncasecmp(path, "https://", 8) || !strncasecmp(path, "file://", 7)) {
        launch_with("browser", path);
        return;
    }
    struct vfs_stat st;
    if (vfs_stat(path, &st)) return;
    if (st.type == VN_DIR) { launch_with("files", path); return; }
    if (image_is_supported(path)) { launch_with("images", path); return; }
    if (ends_with(path, ".html") || ends_with(path, ".htm")) { launch_with("browser", path); return; }
    if (ends_with(path, ".wav")) {
        size_t n;
        char *d = vfs_read_file(path, &n);
        if (d) { audio_play_wav(d, n); kfree(d); }
        return;
    }
    /* ELF programs start as user processes */
    char magic[4] = { 0 };
    vnode_t *vn = vfs_open(path, false);
    if (vn) {
        vfs_read(vn, 0, magic, 4);
        vfs_close(vn);
    }
    if (magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
        char *argv[] = { (char *)path, NULL };
        proc_spawn(path, 1, argv, NULL, session_home());
        return;
    }
    launch_with("editor", path);
}

/* ------------------------------------------------------------------------
 * generic loop
 * ---------------------------------------------------------------------- */

int app_run(app_t *a)
{
    struct gui_event ev;
    a->dirty = true;
    a->ui.mx = a->ui.my = -10000;
    while (!a->quit) {
        if (a->dirty) {
            a->dirty = false;
            surface_t *s = wm_begin(a->win);
            a->ui.s = s;
            a->on_paint(a, s);
            ui_frame_end(&a->ui);
            wm_present(a->win);
            if (a->ui.want_repaint) {
                a->ui.want_repaint = false;
                a->dirty = true;
            }
            if (a->quit) break;
        }
        if (!wm_wait_event(a->win, &ev, a->dirty ? 0 : -1)) continue;
        if (ev.type == EV_CLOSE) {
            if (a->on_close) a->on_close(a);
            else a->quit = true;
            continue;
        }
        ui_feed(&a->ui, &ev);
        int r = a->on_event ? a->on_event(a, &ev) : 1;
        /* events are drained before the next paint (wait with timeout 0) */
        if (ev.type == EV_TIMER || ev.type == EV_KEY_UP) {
            if (r) a->dirty = true;
        } else {
            a->dirty = true;
        }
    }
    wm_destroy(a->win);
    return 0;
}
