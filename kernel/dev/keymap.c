/*
 * Keyboard layouts, read from /usr/share/keymaps/<id>.map:
 *
 *   #name=Deutsch (QWERTZ)           display name
 *   #short=DE                        two letters for the taskbar
 *   #altgr=yes                       right Alt acts as AltGr
 *   q  q  Q  @  -                    key, plain, shift, AltGr, AltGr+Shift
 *   compose acute a U+00E1           dead key + letter = composed letter
 *
 * A column is one character, U+hhhh, "dead:<acute|grave|circumflex|diaeresis|tilde>"
 * or "-" for nothing. The only thing built into the kernel is a US fallback
 * so a damaged image can still be typed on.
 */
#include <kernel.h>
#include <mm.h>
#include <vfs.h>
#include <input.h>
#include <sys.h>

#define DEAD_BIT 0x80000000u
enum { D_ACUTE = 1, D_GRAVE, D_CIRCUMFLEX, D_DIAERESIS, D_TILDE, D_COUNT };
static const char *dead_names[] = { "", "acute", "grave", "circumflex", "diaeresis", "tilde" };
static const uint32_t dead_spacing[] = { 0, 0xB4, '`', '^', 0xA8, '~' };

struct compose { uint8_t dead; uint32_t base, result; };

static struct {
    char id[12], name[48], shortname[4];
    bool altgr;
    uint32_t map[4][KEY_MAX];                /* plain, shift, AltGr, AltGr+Shift */
    struct compose comp[160];
    int ncomp;
} km;
static uint32_t pending_dead;

static const struct { const char *name; int key; } key_names[] = {
    { "1", KEY_1 }, { "2", KEY_2 }, { "3", KEY_3 }, { "4", KEY_4 }, { "5", KEY_5 }, { "6", KEY_6 },
    { "7", KEY_7 }, { "8", KEY_8 }, { "9", KEY_9 }, { "0", KEY_0 }, { "minus", KEY_MINUS }, { "equal", KEY_EQUAL },
    { "q", KEY_Q }, { "w", KEY_W }, { "e", KEY_E }, { "r", KEY_R }, { "t", KEY_T }, { "y", KEY_Y }, { "u", KEY_U },
    { "i", KEY_I }, { "o", KEY_O }, { "p", KEY_P }, { "lbrace", KEY_LBRACE }, { "rbrace", KEY_RBRACE },
    { "a", KEY_A }, { "s", KEY_S }, { "d", KEY_D }, { "f", KEY_F }, { "g", KEY_G }, { "h", KEY_H }, { "j", KEY_J },
    { "k", KEY_K }, { "l", KEY_L }, { "semicolon", KEY_SEMICOLON }, { "apostrophe", KEY_APOSTROPHE },
    { "grave", KEY_GRAVE }, { "backslash", KEY_BACKSLASH }, { "z", KEY_Z }, { "x", KEY_X }, { "c", KEY_C },
    { "v", KEY_V }, { "b", KEY_B }, { "n", KEY_N }, { "m", KEY_M }, { "comma", KEY_COMMA }, { "dot", KEY_DOT },
    { "slash", KEY_SLASH }, { "102nd", KEY_102ND }, { "space", KEY_SPACE }, { "kpdot", KEY_KPDOT },
};

/* the US layout, used until a map file is loaded and when the file is missing */
static void load_fallback(void)
{
    memset(&km, 0, sizeof(km));
    strlcpy(km.id, "us", sizeof(km.id));
    strlcpy(km.name, "English (US)", sizeof(km.name));
    strlcpy(km.shortname, "US", sizeof(km.shortname));
    static const char *plain = "1234567890-=qwertyuiop[]asdfghjkl;'`\\zxcvbnm,./ ";
    static const char *shift = "!@#$%^&*()_+QWERTYUIOP{}ASDFGHJKL:\"~|ZXCVBNM<>? ";
    static const uint8_t keys[] = {
        KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0, KEY_MINUS, KEY_EQUAL,
        KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P, KEY_LBRACE, KEY_RBRACE,
        KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE,
        KEY_BACKSLASH, KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_DOT, KEY_SLASH, KEY_SPACE,
    };
    for (unsigned i = 0; i < sizeof(keys); i++) {
        km.map[0][keys[i]] = (uint8_t)plain[i];
        km.map[1][keys[i]] = (uint8_t)shift[i];
    }
}

static int utf8_decode(const char *s, uint32_t *cp)
{
    uint8_t c = (uint8_t)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) { *cp = (uint32_t)(c & 0x1F) << 6 | (s[1] & 0x3F); return 2; }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = (uint32_t)(c & 0x0F) << 12 | (uint32_t)(s[1] & 0x3F) << 6 | (s[2] & 0x3F);
        return 3;
    }
    *cp = 0;
    return 1;
}

/* one column: a character, U+hhhh, dead:name or - */
static bool parse_token(const char *t, uint32_t *out)
{
    *out = 0;
    if (!strcmp(t, "-")) return true;
    if (!strncmp(t, "U+", 2)) { *out = (uint32_t)strtol(t + 2, NULL, 16); return true; }
    if (!strncmp(t, "dead:", 5)) {
        for (int i = 1; i < D_COUNT; i++)
            if (!strcmp(t + 5, dead_names[i])) { *out = DEAD_BIT | (uint32_t)i; return true; }
        return false;
    }
    uint32_t cp;
    int n = utf8_decode(t, &cp);
    if (t[n]) return false;
    *out = cp;
    return true;
}

static int key_by_name(const char *n)
{
    for (unsigned i = 0; i < ARRAY_SIZE(key_names); i++)
        if (!strcmp(key_names[i].name, n)) return key_names[i].key;
    return 0;
}

static bool parse_map(char *text, const char *id)
{
    memset(&km, 0, sizeof(km));
    strlcpy(km.id, id, sizeof(km.id));
    strlcpy(km.name, id, sizeof(km.name));
    for (char *line = text; line && *line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t ll = strlen(line);
        while (ll && (line[ll - 1] == '\r' || line[ll - 1] == ' ')) line[--ll] = 0;
        if (!strncmp(line, "#name=", 6)) strlcpy(km.name, line + 6, sizeof(km.name));
        else if (!strncmp(line, "#short=", 7)) strlcpy(km.shortname, line + 7, sizeof(km.shortname));
        else if (!strncmp(line, "#altgr=", 7)) km.altgr = !strcmp(line + 7, "yes");
        else if (*line && *line != '#') {
            char *tok[6];
            int nt = 0;
            for (char *p = line; *p && nt < 6;) {
                while (*p == ' ' || *p == '\t') p++;
                if (!*p) break;
                tok[nt++] = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                if (*p) *p++ = 0;
            }
            if (nt == 4 && !strcmp(tok[0], "compose") && km.ncomp < (int)ARRAY_SIZE(km.comp)) {
                uint32_t b, r;
                int d = 0;
                for (int i = 1; i < D_COUNT; i++) if (!strcmp(tok[1], dead_names[i])) d = i;
                if (d && parse_token(tok[2], &b) && parse_token(tok[3], &r))
                    km.comp[km.ncomp++] = (struct compose){ (uint8_t)d, b, r };
            } else if (nt >= 2) {
                int key = key_by_name(tok[0]);
                if (key) {
                    for (int c = 0; c < 4 && c + 1 < nt; c++) {
                        uint32_t v;
                        if (parse_token(tok[c + 1], &v)) km.map[c][key] = v;
                    }
                }
            }
        }
        line = next;
    }
    if (!km.shortname[0]) {
        strlcpy(km.shortname, id, sizeof(km.shortname));
        for (char *c = km.shortname; *c; c++) *c = (char)toupper(*c);
    }
    return true;
}

bool keymap_set(const char *id)
{
    if (!id || !*id || strlen(id) >= sizeof(km.id) || strchr(id, '/') || strchr(id, '.')) return false;
    char path[64];
    snprintf(path, sizeof(path), "/usr/share/keymaps/%s.map", id);
    size_t n;
    char *d = vfs_read_file(path, &n);
    if (!d) return false;
    parse_map(d, id);
    kfree(d);
    pending_dead = 0;
    return true;
}

const char *keymap_current(void) { return km.id[0] ? km.id : "us"; }
const char *keymap_short(void) { return km.shortname[0] ? km.shortname : "US"; }
const char *keymap_long(void) { return km.name[0] ? km.name : "English (US)"; }
bool keymap_altgr(void) { return km.altgr; }

int keymap_layouts(struct kbd_info *out, int max)
{
    int n = 0;
    struct vfs_dirent de;
    for (int i = 0; n < max && vfs_readdir("/usr/share/keymaps", i, &de) == 0; i++) {
        size_t l = strlen(de.name);
        if (l < 5 || strcmp(de.name + l - 4, ".map") || l - 4 >= sizeof(out[n].id)) continue;
        strlcpy(out[n].id, de.name, l - 3);
        strlcpy(out[n].name, out[n].id, sizeof(out[n].name));
        strlcpy(out[n].shortname, out[n].id, sizeof(out[n].shortname));
        char path[96];
        snprintf(path, sizeof(path), "/usr/share/keymaps/%s", de.name);
        size_t sz;
        char *d = vfs_read_file(path, &sz);
        if (d) {
            for (char *line = d; line && *line == '#';) {
                char *next = strchr(line, '\n');
                if (next) *next++ = 0;
                if (!strncmp(line, "#name=", 6)) strlcpy(out[n].name, line + 6, sizeof(out[n].name));
                else if (!strncmp(line, "#short=", 7)) strlcpy(out[n].shortname, line + 7, sizeof(out[n].shortname));
                line = next;
            }
            kfree(d);
        }
        n++;
    }
    return n;
}

/* the layouts the taskbar button cycles through: "keyboard_cycle=de,us", or every installed one */
void keymap_next(void)
{
    const char *list = conf_get(&sysconf, "keyboard_cycle", "");
    char ids[8][12];
    int n = 0;
    if (*list) {
        for (const char *p = list; *p && n < 8;) {
            const char *e = strchr(p, ',');
            size_t l = e ? (size_t)(e - p) : strlen(p);
            if (l && l < sizeof(ids[0])) { memcpy(ids[n], p, l); ids[n][l] = 0; n++; }
            p += l + (e ? 1 : 0);
        }
    }
    if (n < 2) {
        struct kbd_info k[8];
        int m = keymap_layouts(k, 8);
        n = 0;
        for (int i = 0; i < m; i++) strlcpy(ids[n++], k[i].id, sizeof(ids[0]));
    }
    if (n < 2) return;
    int cur = 0;
    for (int i = 0; i < n; i++) if (!strcmp(ids[i], km.id)) cur = i;
    keymap_set(ids[(cur + 1) % n]);
}

static bool is_letter(uint32_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xFF && c != 0xD7 && c != 0xF7);
}

/* what a key types in this layout (no state, dead keys give 0): for on-screen hints */
uint32_t keymap_char(int key, bool shift)
{
    if (key <= 0 || key >= KEY_MAX) return 0;
    if (!km.map[0][KEY_Q] && !km.map[0][KEY_A]) load_fallback();
    uint32_t cp = km.map[shift ? 1 : 0][key];
    if (!cp) cp = km.map[0][key];
    return cp & DEAD_BIT ? 0 : cp;
}

uint32_t keymap_translate(int key, int mods)
{
    if (key <= 0 || key >= KEY_MAX) return 0;
    if (key == KEY_ENTER || key == KEY_KPENTER) return '\n';
    if (key == KEY_BACKSPACE) return '\b';
    if (key == KEY_TAB) return '\t';
    if (!km.map[0][KEY_Q] && !km.map[0][KEY_A]) load_fallback();
    if (key >= KEY_KP7 && key <= KEY_KPDOT && key != KEY_KPMINUS && key != KEY_KPPLUS) {
        /* the number pad: digits from the layout-independent row, the decimal key from the map */
        static const char pad[] = "789-456+1230.";
        if (key == KEY_KPDOT && km.map[0][KEY_KPDOT]) return km.map[0][KEY_KPDOT];
        return (uint32_t)pad[key - KEY_KP7];
    }
    if (key == KEY_KPMINUS) return '-';
    if (key == KEY_KPPLUS) return '+';
    if (key == KEY_KPASTERISK) return '*';
    if (key == KEY_KPSLASH) return '/';
    bool shift = mods & MOD_SHIFT, altgr = (mods & MOD_ALTGR) && km.altgr;
    uint32_t base = km.map[altgr ? 2 : 0][key];
    if ((mods & MOD_CAPS) && is_letter(base)) shift = !shift;
    uint32_t cp = km.map[(shift ? 1 : 0) | (altgr ? 2 : 0)][key];
    if (!cp) cp = base;
    if (cp & DEAD_BIT) {
        pending_dead = cp & ~DEAD_BIT;
        return 0;
    }
    if (pending_dead) {
        uint32_t d = pending_dead, out = 0;
        pending_dead = 0;
        for (int i = 0; i < km.ncomp; i++)
            if (km.comp[i].dead == d && km.comp[i].base == cp) { out = km.comp[i].result; break; }
        if (out) return out;
        return cp == ' ' ? dead_spacing[d] : cp;       /* no combination: the plain character */
    }
    return cp;
}
