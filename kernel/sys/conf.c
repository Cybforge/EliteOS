/* key=value configuration files ("# comment" lines, no sections). */
#include <kernel.h>
#include <mm.h>
#include <vfs.h>
#include <sys.h>

static char *dup(const char *s)
{
    size_t n = strlen(s);
    char *d = kmalloc(n + 1);
    if (d) memcpy(d, s, n + 1);
    return d;
}

void conf_init(struct conf *c)
{
    memset(c, 0, sizeof(*c));
}

void conf_free(struct conf *c)
{
    for (int i = 0; i < c->n; i++) { kfree(c->e[i].k); kfree(c->e[i].v); }
    kfree(c->e);
    memset(c, 0, sizeof(*c));
}

static int find(const struct conf *c, const char *key)
{
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->e[i].k, key)) return i;
    return -1;
}

void conf_set(struct conf *c, const char *key, const char *val)
{
    int i = find(c, key);
    if (i >= 0) {
        char *v = dup(val);
        if (!v) return;
        kfree(c->e[i].v);
        c->e[i].v = v;
        return;
    }
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 16;
        struct conf_ent *e = krealloc(c->e, (size_t)cap * sizeof(*e));
        if (!e) return;
        c->e = e;
        c->cap = cap;
    }
    c->e[c->n].k = dup(key);
    c->e[c->n].v = dup(val);
    if (c->e[c->n].k && c->e[c->n].v) c->n++;
}

void conf_set_int(struct conf *c, const char *key, long v)
{
    char b[24];
    snprintf(b, sizeof(b), "%ld", v);
    conf_set(c, key, b);
}

void conf_del(struct conf *c, const char *key)
{
    int i = find(c, key);
    if (i < 0) return;
    kfree(c->e[i].k);
    kfree(c->e[i].v);
    memmove(&c->e[i], &c->e[i + 1], (size_t)(c->n - i - 1) * sizeof(*c->e));
    c->n--;
}

const char *conf_get(const struct conf *c, const char *key, const char *def)
{
    int i = find(c, key);
    return i >= 0 ? c->e[i].v : def;
}

long conf_int(const struct conf *c, const char *key, long def)
{
    const char *v = conf_get(c, key, NULL);
    if (!v || !*v) return def;
    char *end;
    long r = strtol(v, &end, 10);
    return *end ? def : r;
}

bool conf_parse(struct conf *c, const char *text)
{
    char *buf = dup(text);
    if (!buf) return false;
    for (char *line = buf; line && *line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        while (*line == ' ' || *line == '\t') line++;
        char *eq = strchr(line, '=');
        if (*line && *line != '#' && eq) {
            *eq = 0;
            char *k = line, *v = eq + 1;
            for (char *e = eq - 1; e >= k && (*e == ' ' || *e == '\t'); e--) *e = 0;
            while (*v == ' ' || *v == '\t') v++;
            size_t vl = strlen(v);
            while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' ' || v[vl - 1] == '\t')) v[--vl] = 0;
            if (*k) conf_set(c, k, v);
        }
        line = next;
    }
    kfree(buf);
    return true;
}

bool conf_load(struct conf *c, const char *path)
{
    size_t n;
    char *d = vfs_read_file(path, &n);
    if (!d) return false;
    bool ok = conf_parse(c, d);
    kfree(d);
    return ok;
}

char *conf_format(const struct conf *c, size_t *len)
{
    size_t need = 1;
    for (int i = 0; i < c->n; i++) need += strlen(c->e[i].k) + strlen(c->e[i].v) + 2;
    char *out = kmalloc(need);
    if (!out) return NULL;
    size_t o = 0;
    for (int i = 0; i < c->n; i++) o += (size_t)snprintf(out + o, need - o, "%s=%s\n", c->e[i].k, c->e[i].v);
    if (len) *len = o;
    return out;
}
