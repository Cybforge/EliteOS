#ifndef ZENITH_SYS_H
#define ZENITH_SYS_H

/*
 * System configuration: nothing about the machine, its owner or the language
 * is built into the kernel. It comes from files:
 *
 *   /etc/zenith/system.conf    language, keyboard layout, time zone, clock, ...
 *   /etc/hostname              the host name
 *   /etc/zenith/users          the accounts (PBKDF2 password hashes)
 *   /usr/share/zoneinfo/       the time zone database (TZif)
 *   /usr/share/keymaps/        keyboard layouts (<id>.map)
 *   /usr/share/locale/         translations (<id>.cat)
 *
 * On a live image these files are the defaults from the image and live in
 * RAM. On an installed system a volume carries the marker file
 * zenith/persist; its zenith/etc tree is copied over /etc at boot, every
 * change is written to both, and home directories live on the volume.
 */
#include <kernel.h>
#include <dev.h>

/* ---- key=value files ---- */
struct conf_ent { char *k, *v; };
struct conf {
    struct conf_ent *e;
    int n, cap;
};

void        conf_init(struct conf *c);
void        conf_free(struct conf *c);
bool        conf_parse(struct conf *c, const char *text);
bool        conf_load(struct conf *c, const char *path);
const char *conf_get(const struct conf *c, const char *key, const char *def);
long        conf_int(const struct conf *c, const char *key, long def);
void        conf_set(struct conf *c, const char *key, const char *val);
void        conf_set_int(struct conf *c, const char *key, long v);
void        conf_del(struct conf *c, const char *key);
char       *conf_format(const struct conf *c, size_t *len);      /* kmalloc'd text */

/* ---- persistence, host name, system configuration ---- */
void        sys_init(void);
bool        sys_persistent(void);
const char *sys_persist_root(void);                  /* mount path of the system volume or NULL */
int         sys_mkdir_p(const char *path);
int         sys_write_etc(const char *rel, const void *data, size_t n);   /* /etc/<rel> (+ the volume) */
void        sys_save_conf(void);
extern struct conf sysconf;
const char *sys_hostname(void);
int         sys_set_hostname(const char *name);
bool        sys_valid_hostname(const char *name);

/* ---- accounts and the session ---- */
struct account {
    char name[32];
    char fullname[64];
    int uid;
    bool admin;
    char hash[128];                                  /* "$pbkdf2-sha256$iter$salt$hash", empty = no password */
};

struct session {
    bool active;
    char user[32];
    char fullname[64];
    char home[96];
    int uid;
    bool admin;
    struct conf prefs;                               /* per-user settings: <home>/.config/zenith/settings.conf */
};
extern struct session session;

void                   users_init(void);
int                    users_count(void);
const struct account  *users_get(int i);
const struct account  *users_find(const char *name);
bool                   users_verify(const char *name, const char *password);
int                    users_add(const char *name, const char *fullname, const char *password, bool admin);
int                    users_set_password(const char *name, const char *password);
int                    users_remove(const char *name);
bool                   users_valid_name(const char *name);
int                    users_make_line(const char *name, const char *fullname, const char *password, bool admin, int uid, char *out, size_t n);
bool                   users_need_login(void);       /* more than a password-less account? */
bool                   session_login(const char *name, const char *password);   /* false: wrong password */
void                   session_logout(void);
int                    session_create_home(const char *name, char *home, size_t n);
const char            *session_home(void);
bool                   session_path(const char *rel, char *out, size_t n);       /* relative to the home */
void                   session_save_prefs(void);

/* ---- time zones (TZif files) ---- */
struct tz_zone {
    char name[48];                                   /* "Europe/Berlin" */
    char cc[4];                                      /* country code(s), first one */
    char comment[40];
};

bool        tz_set(const char *name);
const char *tz_name(void);
int         tz_offset(int64_t utc, bool *dst, const char **abbr);       /* seconds east of UTC */
int64_t     tz_local(int64_t utc);
int         tz_zones(const struct tz_zone **out);                       /* from zone1970.tab */
int         tz_country_name(const char *cc, char *out, size_t n);      /* from iso3166.tab */
extern bool rtc_is_local;                                               /* hardware clock keeps local time */
void        rtc_set_epoch(int64_t utc);                                 /* set the clock (and the hardware clock) */
void        rtc_apply_zone(void);                                       /* after the zone or rtc_is_local changed */

/* ---- translations ---- */
struct lang_info {
    char id[8];                                      /* "de" */
    char name[40];                                   /* "Deutsch" */
};

bool        i18n_set(const char *id);
const char *i18n_current(void);
const char *i18n_tr(const char *msgid);
#define tr(s) i18n_tr(s)                /* function-like: local variables named tr are unaffected */
int         i18n_languages(struct lang_info *out, int max);
void        format_date(const struct datetime *dt, char *out, size_t n);   /* in the language's order */
#define _(s) i18n_tr(s)
#define N_(s) (s)

/* ---- keyboard layouts ---- */
struct kbd_info {
    char id[12];                                     /* "de" */
    char name[48];                                   /* "Deutsch (QWERTZ)" */
    char shortname[4];                               /* "DE" */
};

bool        keymap_set(const char *id);
const char *keymap_current(void);
const char *keymap_short(void);
const char *keymap_long(void);
bool        keymap_altgr(void);                      /* right Alt is AltGr in this layout */
int         keymap_layouts(struct kbd_info *out, int max);
void        keymap_next(void);                       /* cycle through the layouts in sysconf "keyboard_cycle" */

#endif
