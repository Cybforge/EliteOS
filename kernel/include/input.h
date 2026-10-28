#ifndef ZENITH_INPUT_H
#define ZENITH_INPUT_H

#include <kernel.h>

/* key codes (PC set-1 positions, extended keys like Linux input codes) */
enum {
    KEY_ESC = 1, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
    KEY_MINUS, KEY_EQUAL, KEY_BACKSPACE, KEY_TAB,
    KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P,
    KEY_LBRACE, KEY_RBRACE, KEY_ENTER, KEY_LCTRL,
    KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,
    KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_LSHIFT, KEY_BACKSLASH,
    KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M,
    KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_RSHIFT, KEY_KPASTERISK, KEY_LALT, KEY_SPACE,
    KEY_CAPSLOCK, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,
    KEY_NUMLOCK, KEY_SCROLLLOCK, KEY_KP7, KEY_KP8, KEY_KP9, KEY_KPMINUS, KEY_KP4, KEY_KP5,
    KEY_KP6, KEY_KPPLUS, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP0, KEY_KPDOT,
    KEY_102ND = 86, KEY_F11 = 87, KEY_F12 = 88,
    KEY_KPENTER = 96, KEY_RCTRL = 97, KEY_KPSLASH = 98, KEY_SYSRQ = 99, KEY_RALT = 100,
    KEY_HOME = 102, KEY_UP = 103, KEY_PAGEUP = 104, KEY_LEFT = 105, KEY_RIGHT = 106,
    KEY_END = 107, KEY_DOWN = 108, KEY_PAGEDOWN = 109, KEY_INSERT = 110, KEY_DELETE = 111,
    KEY_LMETA = 125, KEY_RMETA = 126, KEY_MENU = 127,
    KEY_MAX = 128,
};

#define MOD_SHIFT 0x01
#define MOD_CTRL  0x02
#define MOD_ALT   0x04
#define MOD_SUPER 0x08
#define MOD_ALTGR 0x10
#define MOD_CAPS  0x20

#define BTN_LEFT   1
#define BTN_RIGHT  2
#define BTN_MIDDLE 4

/* raw events from drivers */
enum { RAW_KEY, RAW_MOUSE_REL, RAW_MOUSE_ABS };

struct raw_input {
    uint8_t type;
    uint8_t pressed;
    uint8_t buttons;
    int8_t  wheel;
    uint16_t key;
    int32_t x, y;          /* rel: delta; abs: 0..65535 */
};

void input_init(void);
void input_push(const struct raw_input *ev);
bool input_pop(struct raw_input *ev);
void *input_chan(void);

/* keymaps: layouts are files, see sys.h (keymap_set, keymap_layouts ...) */
uint32_t keymap_translate(int key, int mods);
uint32_t keymap_char(int key, bool shift);

#endif
