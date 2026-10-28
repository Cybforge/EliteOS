#ifndef ZENITH_WM_H
#define ZENITH_WM_H

#include <kernel.h>
#include <gfx.h>
#include <input.h>
#include <spinlock.h>
#include <dev.h>

/* ------------------------------------------------------------------------
 * theme
 * ---------------------------------------------------------------------- */
struct theme {
    color_t bg, panel, surface, surface2, border, text, text_dim, text_faint;
    color_t accent, accent2, danger, success, warning;
    color_t title_active, title_inactive;
};
extern struct theme theme;
void theme_set_accent(int idx);
extern const color_t accent_choices[];
extern const char *accent_names[];
extern int accent_count;
extern int accent_index;

/* ------------------------------------------------------------------------
 * events delivered to windows
 * ---------------------------------------------------------------------- */
enum {
    EV_NONE,
    EV_KEY_DOWN,
    EV_KEY_UP,
    EV_MOUSE_MOVE,
    EV_MOUSE_DOWN,
    EV_MOUSE_UP,
    EV_MOUSE_WHEEL,
    EV_MOUSE_LEAVE,
    EV_CLOSE,
    EV_RESIZE,
    EV_FOCUS,
    EV_BLUR,
    EV_TIMER,
};

struct gui_event {
    int type;
    int key;
    uint32_t ch;
    int mods;
    int x, y;
    int button;     /* the button that changed (down/up) */
    int buttons;    /* all buttons held */
    int wheel;
    int clicks;     /* 2 for double click */
};

/* ------------------------------------------------------------------------
 * windows
 * ---------------------------------------------------------------------- */
#define WF_RESIZABLE   0x01
#define WF_NO_DECOR    0x02
#define WF_TOPMOST     0x04
#define WF_TRANSLUCENT 0x08    /* client pixels carry alpha */
#define WF_CENTER      0x10
#define WF_NO_TASKBAR  0x20
#define WF_DIALOG      0x40

#define TITLE_H 36
#define WIN_RADIUS 10

enum { WS_NORMAL, WS_MINIMIZED, WS_MAXIMIZED };

#define EVQ_SIZE 128

typedef struct window {
    int id;
    char title[64];
    int icon;
    int flags;
    int state;
    int x, y;                  /* outer top-left */
    int cw, ch;                /* displayed client size (= front size) */
    int req_w, req_h;          /* size the app should draw at */
    int min_w, min_h;
    rect_t restore;
    surface_t *front, *back;
    uint8_t opacity;           /* current, animated */
    uint8_t user_opacity;      /* 255 unless the app asks for translucency */
    int anim;                  /* 0 none, 1 opening, 2 closing, 3 minimizing, 4 restoring */
    uint64_t anim_start;
    int anim_dy;
    bool destroyed;
    bool close_requested;
    struct process *proc;      /* owning user process, if any */
    void *owner_thread;

    spinlock_t evlock;
    struct gui_event evq[EVQ_SIZE];
    unsigned ev_head, ev_tail;
    uint64_t timer_ms, next_timer;

    int hover_button;          /* title bar button under the mouse */
    struct window *next;       /* z-order, bottom to top */
} window_t;

void wm_init(void);
window_t *wm_create(const char *title, int w, int h, int flags);
surface_t *wm_begin(window_t *w);          /* back buffer at the requested size */
void wm_present(window_t *w);
bool wm_wait_event(window_t *w, struct gui_event *ev, int timeout_ms);   /* -1 = forever */
void wm_post_event(window_t *w, const struct gui_event *ev);
void wm_destroy(window_t *w);
void wm_set_title(window_t *w, const char *t);
void wm_set_icon(window_t *w, int icon);
void wm_set_timer(window_t *w, uint64_t ms);
void wm_set_min_size(window_t *w, int mw, int mh);
void wm_focus(window_t *w);
void wm_request_resize(window_t *w, int cw, int ch);
int  wm_screen_w(void);
int  wm_screen_h(void);
void wm_notify(const char *title, const char *body, int icon);
void wm_set_wallpaper(int style);
void settings_load(void);
void settings_autosave(void);
bool settings_welcome_seen(void);
extern int wallpaper_style;
extern int wallpaper_count;
extern const char *wallpaper_names[];
bool wm_save_screenshot(char *path_out, size_t n);
void wm_invalidate_all(void);
int  wm_window_list(window_t **out, int max);
uint32_t wm_fps(void);
extern volatile bool wm_ready;
extern bool ui_transparency;
extern bool ui_animations;

/* ------------------------------------------------------------------------
 * icons (wm/icons.c)
 * ---------------------------------------------------------------------- */
enum {
    ICON_NONE, ICON_TERMINAL, ICON_FILES, ICON_EDITOR, ICON_MONITOR, ICON_CUBE,
    ICON_FRACTAL, ICON_TETRIS, ICON_PAINT, ICON_CALC, ICON_CLOCK, ICON_SETTINGS,
    ICON_INFO, ICON_IMAGE, ICON_LOG, ICON_SNAKE, ICON_FOLDER, ICON_FILE, ICON_TEXT,
    ICON_APP, ICON_POWER, ICON_RESTART, ICON_SEARCH, ICON_NETWORK, ICON_HOME,
    ICON_TRASH, ICON_GAME, ICON_PLASMA, ICON_BROWSER, ICON_LOGOUT, ICON_DISK, ICON_INSTALL, ICON_PIANO, ICON_SOUND, ICON_CHESS, ICON_MINES, ICON_2048, ICON_STORE, ICON_USER, ICON_LOCK, ICON_ETHERNET, ICON_COUNT,
};
void icon_draw(surface_t *s, int icon, int x, int y, int size);
int  icon_by_name(const char *name);               /* ICON_NONE if unknown */
void logo_draw(surface_t *s, int x, int y, int size, bool swap);

/* ------------------------------------------------------------------------
 * immediate-mode widgets (wm/ui.c)
 * ---------------------------------------------------------------------- */
typedef struct ui {
    surface_t *s;
    int mx, my;
    bool mdown, mpressed, mreleased, rclicked, dclicked;
    int wheel;
    uint64_t hot, active, focus;
    bool has_key;
    struct gui_event key;
    bool want_repaint;
    bool key_used;
} ui_t;

enum { BTN_NORMAL, BTN_PRIMARY, BTN_DANGER, BTN_FLAT, BTN_SUBTLE };

uint64_t ui_id(rect_t r);
void ui_feed(ui_t *u, const struct gui_event *ev);
void ui_frame_end(ui_t *u);
bool ui_hover(ui_t *u, rect_t r);
bool ui_button(ui_t *u, rect_t r, const char *label, int style);
bool ui_icon_button(ui_t *u, rect_t r, int icon, const char *tip);
bool ui_toggle(ui_t *u, rect_t r, bool *v);
bool ui_checkbox(ui_t *u, rect_t r, const char *label, bool *v);
bool ui_slider(ui_t *u, rect_t r, float *v, float lo, float hi);
bool ui_textbox(ui_t *u, rect_t r, char *buf, size_t cap, const char *placeholder);
bool ui_password(ui_t *u, rect_t r, char *buf, size_t cap, const char *placeholder);
bool ui_tab(ui_t *u, rect_t r, const char *label, bool active);
void ui_progress(ui_t *u, rect_t r, float v, color_t c);
void ui_panel(ui_t *u, rect_t r);
void ui_label(ui_t *u, int x, int y, const char *text, font_t *f, color_t c);
bool ui_list_item(ui_t *u, rect_t r, bool selected);
void ui_scrollbar(ui_t *u, rect_t r, int *scroll, int content, int view);

/* ------------------------------------------------------------------------
 * app helper (wm/app.c): a window + a paint/event loop
 * ---------------------------------------------------------------------- */
typedef struct app {
    window_t *win;
    ui_t ui;
    void *data;
    int  (*on_event)(struct app *a, struct gui_event *ev);   /* return 1 to repaint */
    void (*on_paint)(struct app *a, surface_t *s);
    void (*on_close)(struct app *a);
    bool quit;
    bool dirty;
} app_t;

int  app_run(app_t *a);
void app_launch(const char *name);
struct app_info {
    const char *id;
    const char *name;
    const char *desc;
    int icon;
    int (*main)(void *arg);
    bool desktop;
};
extern const struct app_info app_table[];
extern const int app_count;
const struct app_info *app_find(const char *id);
void open_path(const char *path);

/* launchers (wm/launcher.c): the .zapp files that make up the start menu */
struct launcher {
    char id[32];
    char name[48];
    char desc[96];
    char exec[200];
    char category[24];
    int icon;
    bool builtin;
};
void launchers_reload(void);
int  launchers_count(void);
const struct launcher *launcher_get(int i);
const struct launcher *launcher_find(const char *id);
void launcher_run(const struct launcher *l);
int  file_icon_for(const char *name);

#endif
