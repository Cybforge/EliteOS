/*
 * Translations. The program's strings are English; a catalog
 * /usr/share/locale/<id>.cat maps each English text to its translation
 * (one "english<TAB>translation" pair per line, \n \t \\ escaped, '#' lines
 * are comments, "#name=Deutsch" gives the language's own name). Dropping a
 * new .cat file into that directory adds a language.
 */
#include <kernel.h>
#include <mm.h>
#include <vfs.h>
#include <dev.h>
#include <sys.h>

#define HASH_SIZE 8192

struct entry { const char *k, *v; };

static char lang[8] = "en";
static char date_order[4] = "ymd";          /* "dmy", "mdy", "ymd" from the catalog's #date=dmy. line */
static char date_sep = '-';
static char *catalog;                       /* the file, modified in place */
static struct entry *table;                 /* open addressing, NULL key = empty */
static int count;

static uint32_t fnv(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void unescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *o++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static void insert(const char *k, const char *v)
{
    uint32_t h = fnv(k) & (HASH_SIZE - 1);
    while (table[h].k) {
        if (!strcmp(table[h].k, k)) { table[h].v = v; return; }
        h = (h + 1) & (HASH_SIZE - 1);
    }
    table[h].k = k;
    table[h].v = v;
    count++;
}

static void unload(void)
{
    kfree(catalog);
    kfree(table);
    catalog = NULL;
    table = NULL;
    count = 0;
}

bool i18n_set(const char *id)
{
    if (!id || !*id || strlen(id) >= sizeof(lang) || strchr(id, '/')) return false;
    if (!strcmp(id, "en")) {                          /* the source language needs no catalog */
        unload();
        strcpy(date_order, "ymd");
        date_sep = '-';
        strlcpy(lang, "en", sizeof(lang));
        return true;
    }
    char path[64];
    snprintf(path, sizeof(path), "/usr/share/locale/%s.cat", id);
    size_t n;
    char *d = vfs_read_file(path, &n);
    if (!d) return false;
    struct entry *t = kzalloc(HASH_SIZE * sizeof(*t));
    if (!t) { kfree(d); return false; }
    unload();
    catalog = d;
    table = t;
    for (char *line = d; line && *line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!strncmp(line, "#date=", 6) && strlen(line) >= 10) {
            memcpy(date_order, line + 6, 3);
            date_order[3] = 0;
            date_sep = line[9];
        }
        if (*line && *line != '#') {
            char *tab = strchr(line, '\t');
            if (tab && count < HASH_SIZE * 3 / 4) {
                *tab = 0;
                unescape(line);
                unescape(tab + 1);
                if (tab[1]) insert(line, tab + 1);
            }
        }
        line = next;
    }
    strlcpy(lang, id, sizeof(lang));
    klog("i18n: %s, %d texts", id, count);
    return true;
}

const char *i18n_current(void) { return lang; }

const char *i18n_tr(const char *msgid)
{
    if (!table || !msgid || !*msgid) return msgid;
    uint32_t h = fnv(msgid) & (HASH_SIZE - 1);
    while (table[h].k) {
        if (!strcmp(table[h].k, msgid)) return table[h].v;
        h = (h + 1) & (HASH_SIZE - 1);
    }
    return msgid;
}

int i18n_languages(struct lang_info *out, int max)
{
    int n = 0;
    if (n < max) {
        strlcpy(out[n].id, "en", sizeof(out[n].id));
        strlcpy(out[n].name, "English", sizeof(out[n].name));
        n++;
    }
    struct vfs_dirent de;
    for (int i = 0; n < max && vfs_readdir("/usr/share/locale", i, &de) == 0; i++) {
        size_t l = strlen(de.name);
        if (l < 5 || strcmp(de.name + l - 4, ".cat") || l - 4 >= sizeof(out[n].id)) continue;
        strlcpy(out[n].id, de.name, l - 3);
        strlcpy(out[n].name, out[n].id, sizeof(out[n].name));
        char path[96];
        snprintf(path, sizeof(path), "/usr/share/locale/%s", de.name);
        size_t sz;
        char *d = vfs_read_file(path, &sz);
        if (d) {
            for (char *line = d; line && *line == '#';) {
                char *next = strchr(line, '\n');
                if (next) *next++ = 0;
                if (!strncmp(line, "#name=", 6)) { strlcpy(out[n].name, line + 6, sizeof(out[n].name)); break; }
                line = next;
            }
            kfree(d);
        }
        n++;
    }
    return n;
}

void format_date(const struct datetime *dt, char *out, size_t n)
{
    char tmp[24];
    int o = 0;
    for (int i = 0; i < 3; i++) {
        if (i) tmp[o++] = date_sep;
        o += snprintf(tmp + o, sizeof(tmp) - (size_t)o, date_order[i] == 'y' ? "%04d" : "%02d",
                      date_order[i] == 'y' ? dt->year : date_order[i] == 'm' ? dt->month : dt->day);
    }
    strlcpy(out, tmp, n);
}
