/* Shared between the compositor (wm.c) and the desktop shell (desktop.c). */
#ifndef ZENITH_WM_INTERNAL_H
#define ZENITH_WM_INTERNAL_H

#include <wm.h>
#include <vfs.h>

extern mutex_t wm_mtx;
extern window_t *wm_list;          /* bottom to top */
extern window_t *wm_focus_win;
extern int wm_mx, wm_my;
extern int wm_mods;

void wm_dirty(rect_t r);
void wm_dirty_window(window_t *w);
rect_t wm_frame_rect(window_t *w);
rect_t wm_shadow_rect(window_t *w);
void wm_raise(window_t *w);
void wm_focus_locked(window_t *w);
void wm_minimize(window_t *w);
void wm_restore(window_t *w);
void wm_toggle_maximize(window_t *w);
void wm_request_close(window_t *w);
window_t *wm_top_visible(void);
void wm_kick(void);

/* desktop shell hooks, all called with wm_mtx held */
void desktop_init(int w, int h);
void desktop_resize(int w, int h);
void desktop_draw_background(surface_t *s, rect_t clip);
void desktop_draw_overlay(surface_t *s, rect_t clip);
bool desktop_mouse_overlay(int x, int y, int buttons, int pressed, int released, int wheel);
void desktop_mouse_background(int x, int y, int buttons, int pressed, int released, int clicks);
bool desktop_key(int key, uint32_t ch, int mods, bool pressed);
bool desktop_tick(uint64_t now);
int  desktop_taskbar_h(void);
void desktop_windows_changed(void);
void desktop_alt_tab(bool start, bool reverse);
void desktop_alt_release(void);
void desktop_toggle_start(void);
void desktop_notify(const char *title, const char *body, int icon);
void desktop_session_started(void);
void desktop_sign_out(void);          /* a user has signed in: apply their settings, open their desktop */

/* login and lock screen (wm/login.c) */
bool login_active(void);
void login_start(bool lock);
void login_draw(surface_t *s, rect_t clip, surface_t *blur);
bool login_mouse(int x, int y, int pressed);
bool login_key(int key, uint32_t ch, int mods, bool pressed);
bool login_tick(void);

#endif
