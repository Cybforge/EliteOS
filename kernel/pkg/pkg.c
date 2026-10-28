/*
 * The package manager: sources, signed indexes, dependency resolution and
 * transactional install/remove. See pkg.h for the formats.
 */
#include <kernel.h>
#include <mm.h>
#include <vfs.h>
#include <net.h>
#include <crypto.h>
#include <archive.h>
#include <sys.h>
#include <pkg.h>
#include <sched.h>
#include <audio.h>

#define MAX_PKGS 768
#define MAX_SOURCES 8
#define MAX_PLAN 24
#define MAX_PKG_BYTES (1ull << 30)

static struct pkg *pkgs;
static int npkgs;
static struct pkg_source sources[MAX_SOURCES];
static int nsources;
static struct pkg *inst;                        /* what /var/lib/zenith/pkg says */
static int ninst;
static mutex_t pkg_mtx = MUTEX_INIT("pkg");
static spinlock_t dlock = SPINLOCK_INIT("pkgdata");      /* short: guards the lists the store reads while a transaction runs */
static char proot[96];                          /* "" normally; the zenith folder of another volume while installing onto it */

/* a path below the package root: under(buf, n, "/opt/%s", name) */
static void under(char *out, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void under(char *out, size_t n, const char *fmt, ...)
{
    size_t l = (size_t)snprintf(out, n, "%s", proot);
    va_list ap;
    va_start(ap, fmt);
    if (l < n) vsnprintf(out + l, n - l, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------------
 * small helpers
 * ---------------------------------------------------------------------- */

int ver_cmp(const char *a, const char *b)
{
    while (*a || *b) {
        while (*a && !isdigit((unsigned char)*a) && !isalpha((unsigned char)*a)) a++;
        while (*b && !isdigit((unsigned char)*b) && !isalpha((unsigned char)*b)) b++;
        if (isdigit((unsigned char)*a) || isdigit((unsigned char)*b)) {
            long x = 0, y = 0;
            while (isdigit((unsigned char)*a)) x = x * 10 + (*a++ - '0');
            while (isdigit((unsigned char)*b)) y = y * 10 + (*b++ - '0');
            if (x != y) return x < y ? -1 : 1;
        } else {
            while (isalpha((unsigned char)*a) || isalpha((unsigned char)*b)) {
                if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
                if (*a) a++;
                if (*b) b++;
            }
        }
    }
    return 0;
}

void pkg_log(struct pkg_job *job, const char *fmt, ...)
{
    if (!job) return;
    char b[110];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (job->nlog == 16) { memmove(job->log[0], job->log[1], sizeof(job->log[0]) * 15); job->nlog = 15; }
    strlcpy(job->log[job->nlog], b, sizeof(job->log[0]));
    job->nlog++;
    klog("pkg: %s", b);
}

static void stage(struct pkg_job *job, int percent, const char *fmt, ...)
{
    if (!job) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(job->stage, sizeof(job->stage), fmt, ap);
    va_end(ap);
    job->percent = percent;
}

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static bool parse_hex(const char *s, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int a = hexval(s[2 * i]), b = a < 0 ? -1 : hexval(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return true;
}

/* ------------------------------------------------------------------------
 * stanzas
 * ---------------------------------------------------------------------- */

static void split_words(const char *s, char out[][64], int max, int *n, char sep)
{
    *n = 0;
    while (*s && *n < max) {
        while (*s == ' ' || *s == sep) s++;
        if (!*s) break;
        const char *e = s;
        int depth = 0;
        while (*e && (depth || *e != sep)) {
            if (*e == '(') depth++;
            else if (*e == ')') depth--;
            e++;
        }
        size_t l = (size_t)(e - s);
        while (l && s[l - 1] == ' ') l--;
        if (l && l < 64) { memcpy(out[*n], s, l); out[*n][l] = 0; (*n)++; }
        s = e;
    }
}

static void apply_field(struct pkg *p, const char *k, const char *v)
{
    if (!strcasecmp(k, "Package")) strlcpy(p->name, v, sizeof(p->name));
    else if (!strcasecmp(k, "Version")) strlcpy(p->version, v, sizeof(p->version));
    else if (!strcasecmp(k, "Type")) strlcpy(p->type, v, sizeof(p->type));
    else if (!strcasecmp(k, "Arch") || !strcasecmp(k, "Architecture")) strlcpy(p->arch, v, sizeof(p->arch));
    else if (!strcasecmp(k, "Category")) strlcpy(p->category, v, sizeof(p->category));
    else if (!strcasecmp(k, "Description")) strlcpy(p->desc, v, sizeof(p->desc));
    else if (!strcasecmp(k, "Homepage")) strlcpy(p->homepage, v, sizeof(p->homepage));
    else if (!strcasecmp(k, "Filename")) strlcpy(p->filename, v, sizeof(p->filename));
    else if (!strcasecmp(k, "Module")) strlcpy(p->module, v, sizeof(p->module));
    else if (!strcasecmp(k, "Personality")) strlcpy(p->personality, v, sizeof(p->personality));
    else if (!strcasecmp(k, "Format")) strlcpy(p->format, v, sizeof(p->format));
    else if (!strcasecmp(k, "Title") || !strcasecmp(k, "Name")) strlcpy(p->title, v, sizeof(p->title));
    else if (!strcasecmp(k, "Icon")) strlcpy(p->icon, v, sizeof(p->icon));
    else if (!strcasecmp(k, "Exec")) strlcpy(p->exec, v, sizeof(p->exec));
    else if (!strcasecmp(k, "Size")) p->size = (uint64_t)strtol(v, NULL, 10);
    else if (!strcasecmp(k, "Installed-Size")) p->installed_size = (uint64_t)strtol(v, NULL, 10);
    else if (!strcasecmp(k, "SHA256")) parse_hex(v, p->sha256, 32);
    else if (!strcasecmp(k, "Depends")) split_words(v, p->depends, PKG_MAX_DEPS, &p->ndeps, ',');
    else if (!strcasecmp(k, "Provides")) split_words(v, p->provides, 6, &p->nprov, ',');
    else if (!strcasecmp(k, "Requires")) {
        char tmp[4][64];
        int n = 0;
        split_words(v, tmp, 4, &n, ',');
        for (int i = 0; i < n; i++) strlcpy(p->requires[p->nreq++], tmp[i], sizeof(p->requires[0]));
    }
    else if (!strcasecmp(k, "Match-PCI")) {
        for (const char *s = v; *s && p->npci < PKG_MAX_PCI;) {
            while (*s == ' ') s++;
            char *end;
            long ven = strtol(s, &end, 16);
            if (*end != ':') break;
            long dev = strtol(end + 1, &end, 16);
            p->pci[p->npci++] = (uint32_t)(ven << 16 | dev);
            s = end;
        }
    }
}

/* parse the stanzas of `text` into out[]; returns how many were valid */
static int parse_stanzas(char *text, struct pkg *out, int max, int source, bool verified)
{
    int n = 0;
    char *line = text;
    struct pkg cur;
    bool have = false;
    char lastkey[32] = "";
    memset(&cur, 0, sizeof(cur));
    while (line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t l = strlen(line);
        if (l && line[l - 1] == '\r') line[--l] = 0;
        if (!l) {
            if (have && cur.name[0] && cur.version[0] && n < max) {
                cur.source = source;
                cur.verified = verified;
                if (!cur.type[0]) strlcpy(cur.type, "app", sizeof(cur.type));
                if (!cur.arch[0]) strlcpy(cur.arch, "x86_64", sizeof(cur.arch));
                if (!cur.format[0]) strlcpy(cur.format, "zpk", sizeof(cur.format));
                if (!strcmp(cur.arch, "x86_64") || !strcmp(cur.arch, "all")) out[n++] = cur;
            }
            memset(&cur, 0, sizeof(cur));
            have = false;
            lastkey[0] = 0;
        } else if (line[0] != '#') {
            if ((line[0] == ' ' || line[0] == '\t') && lastkey[0]) {
                /* a continuation of the description */
                if (!strcasecmp(lastkey, "Description")) {
                    size_t dl = strlen(cur.desc);
                    if (dl + 2 < sizeof(cur.desc)) {
                        cur.desc[dl] = ' ';
                        strlcpy(cur.desc + dl + 1, line + 1, sizeof(cur.desc) - dl - 1);
                    }
                }
            } else {
                char *colon = strchr(line, ':');
                if (colon) {
                    *colon = 0;
                    char *v = colon + 1;
                    while (*v == ' ') v++;
                    strlcpy(lastkey, line, sizeof(lastkey));
                    apply_field(&cur, line, v);
                    have = true;
                }
            }
        }
        line = next;
    }
    if (have && cur.name[0] && cur.version[0] && n < max) {
        cur.source = source;
        cur.verified = verified;
        if (!cur.type[0]) strlcpy(cur.type, "app", sizeof(cur.type));
        if (!cur.arch[0]) strlcpy(cur.arch, "x86_64", sizeof(cur.arch));
        if (!cur.format[0]) strlcpy(cur.format, "zpk", sizeof(cur.format));
        if (!strcmp(cur.arch, "x86_64") || !strcmp(cur.arch, "all")) out[n++] = cur;
    }
    return n;
}

/* ------------------------------------------------------------------------
 * signatures
 * ---------------------------------------------------------------------- */

static bool verify_signature(const char *data, size_t n, const char *sigtext)
{
    char tmp[256];
    strlcpy(tmp, sigtext, sizeof(tmp));
    if (strncmp(tmp, "zenith-sig-v1 ", 14)) return false;
    char *b = tmp + 14;
    for (char *e = b + strlen(b); e > b && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); *--e = 0) {}
    uint8_t sig[72];
    int sn = base64_decode(b, sig, sizeof(sig));
    if (sn != 64) return false;
    uint8_t hash[32];
    sha256(data, n, hash);
    struct vfs_dirent de[16];
    int nk = vfs_list("/etc/zenith/keys", de, 16);
    for (int i = 0; i < nk; i++) {
        size_t l = strlen(de[i].name);
        if (l < 5 || strcmp(de[i].name + l - 4, ".pub")) continue;
        char path[96];
        snprintf(path, sizeof(path), "/etc/zenith/keys/%s", de[i].name);
        size_t kn;
        char *k = vfs_read_file(path, &kn);
        if (!k) continue;
        bool ok = false;
        if (!strncmp(k, "zenith-key-v1 ", 14)) {
            char *kb = k + 14;
            for (char *e = kb + strlen(kb); e > kb && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); *--e = 0) {}
            uint8_t pub[72];
            if (base64_decode(kb, pub, sizeof(pub)) == 65)
                ok = ecdsa_verify(CURVE_P256, pub, 65, hash, 32, sig, 32, sig + 32, 32);
        }
        kfree(k);
        if (ok) return true;
    }
    return false;
}

/* ------------------------------------------------------------------------
 * sources and indexes
 * ---------------------------------------------------------------------- */

/* one line per source: "<name> <url> [unsigned]" */
static void parse_sources(char *t, bool allow_unsigned, const char *prefix)
{
    for (char *line = t; line && *line && nsources < MAX_SOURCES;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        while (*line == ' ') line++;
        if (*line && *line != '#') {
            char *w[4];
            int nw = 0;
            for (char *p = line; *p && nw < 4;) {
                while (*p == ' ' || *p == '\t' || *p == '\r') p++;
                if (!*p) break;
                w[nw++] = p;
                while (*p && *p != ' ' && *p != '\t' && *p != '\r') p++;
                if (*p) *p++ = 0;
            }
            if (nw >= 2) {
                struct pkg_source *s = &sources[nsources++];
                memset(s, 0, sizeof(*s));
                snprintf(s->name, sizeof(s->name), "%s%s", prefix, w[0]);
                strlcpy(s->url, w[1], sizeof(s->url));
                size_t l = strlen(s->url);
                while (l && s->url[l - 1] == '/') s->url[--l] = 0;
                s->require_signature = !(allow_unsigned && nw >= 3 && !strcmp(w[2], "unsigned"));
            }
        }
        line = next;
    }
}

/* /etc/zenith/sources.list, plus zenith/sources.list on any mounted volume (a USB stick can bring its own
 * repository; those are always checked against the trusted keys) */
static void load_sources(void)
{
    nsources = 0;
    size_t n;
    char *t = vfs_read_file("/etc/zenith/sources.list", &n);
    if (t) { parse_sources(t, true, ""); kfree(t); }
    for (struct fs_mount *m = vfs_mounts(); m; m = m->next) {
        char path[96];
        snprintf(path, sizeof(path), "%s/zenith/sources.list", m->path);
        t = vfs_read_file(path, &n);
        if (t) { parse_sources(t, false, "vol-"); kfree(t); }
    }
}

static void list_path(const struct pkg_source *s, const char *what, char *out, size_t n)
{
    snprintf(out, n, "/var/lib/zenith/lists/%s.%s", s->name, what);
}

/* read one source's cached index into the in-memory list */
static int load_cached_source(int si, struct pkg *out, int max)
{
    char pp[96], sp[96];
    list_path(&sources[si], "Packages", pp, sizeof(pp));
    list_path(&sources[si], "sig", sp, sizeof(sp));
    size_t n;
    char *text = vfs_read_file(pp, &n);
    if (!text) return 0;
    size_t sn;
    char *sig = vfs_read_file(sp, &sn);
    bool verified = sig && verify_signature(text, n, sig);
    kfree(sig);
    if (sources[si].require_signature && !verified) {
        snprintf(sources[si].error, sizeof(sources[si].error), "%s", "the index is not signed by a trusted key");
        kfree(text);
        return 0;
    }
    int got = parse_stanzas(text, out, max, si, verified);
    kfree(text);
    return got;
}

static void mark_installed(void)
{
    spin_lock(&dlock);
    for (int i = 0; i < npkgs; i++) {
        pkgs[i].installed = false;
        for (int k = 0; k < ninst; k++)
            if (!strcmp(pkgs[i].name, inst[k].name)) {
                pkgs[i].installed = true;
                strlcpy(pkgs[i].installed_version, inst[k].version, sizeof(pkgs[i].installed_version));
            }
    }
    spin_unlock(&dlock);
}

/* read every source's cached index; the visible list is replaced in one step */
static void reload_cached(void)
{
    struct pkg *fresh = kzalloc(MAX_PKGS * sizeof(*fresh));
    if (!fresh) return;
    int n = 0;
    for (int i = 0; i < nsources; i++) {
        sources[i].count = load_cached_source(i, fresh + n, MAX_PKGS - n);
        n += sources[i].count;
    }
    spin_lock(&dlock);
    memcpy(pkgs, fresh, (size_t)n * sizeof(*fresh));
    if (n < npkgs) memset(pkgs + n, 0, (size_t)(npkgs - n) * sizeof(*fresh));
    npkgs = n;
    spin_unlock(&dlock);
    kfree(fresh);
}

static void load_installed(void)
{
    struct pkg *fresh = kzalloc(MAX_PKGS * sizeof(*fresh));
    int count = 0;
    struct vfs_dirent de[128];
    char dbdir[VFS_PATH_MAX];
    under(dbdir, sizeof(dbdir), "/var/lib/zenith/pkg");
    int n = vfs_list(dbdir, de, 128);
    for (int i = 0; i < n && count < MAX_PKGS; i++) {
        size_t l = strlen(de[i].name);
        if (l < 6 || strcmp(de[i].name + l - 5, ".info")) continue;
        char path[VFS_PATH_MAX];
        under(path, sizeof(path), "/var/lib/zenith/pkg/%s", de[i].name);
        size_t sz;
        char *t = vfs_read_file(path, &sz);
        if (!t) continue;
        count += parse_stanzas(t, fresh + count, MAX_PKGS - count, -1, true);
        kfree(t);
    }
    spin_lock(&dlock);
    memcpy(inst, fresh, (size_t)count * sizeof(*fresh));
    ninst = count;
    spin_unlock(&dlock);
    kfree(fresh);
    mark_installed();
}

void pkg_init(void)
{
    if (!pkgs) pkgs = kzalloc(MAX_PKGS * sizeof(*pkgs));
    if (!inst) inst = kzalloc(MAX_PKGS * sizeof(*inst));
    mutex_lock(&pkg_mtx);
    load_sources();
    reload_cached();
    load_installed();
    mutex_unlock(&pkg_mtx);
    klog("pkg: %d source(s), %d known package(s), %d installed", nsources, npkgs, ninst);
}

/* install onto another volume (the installer): prefix is that volume's zenith folder, "" goes back to this system */
void pkg_set_root(const char *prefix)
{
    mutex_lock(&pkg_mtx);
    strlcpy(proot, prefix ? prefix : "", sizeof(proot));
    load_installed();
    mutex_unlock(&pkg_mtx);
}

int pkg_update(struct pkg_job *job)
{
    if (!pkgs) pkg_init();
    mutex_lock(&pkg_mtx);
    sys_mkdir_p("/var/lib/zenith/lists");
    load_sources();
    struct pkg *fresh = kzalloc(MAX_PKGS * sizeof(*fresh));
    int nfresh = 0;
    for (int i = 0; i < nsources; i++) {
        struct pkg_source *s = &sources[i];
        stage(job, 100 * i / MAX(1, nsources), "Reading the package list of %s", s->name);
        char url[260], pp[96], sp[96], err[120] = "";
        list_path(s, "Packages", pp, sizeof(pp));
        list_path(s, "sig", sp, sizeof(sp));
        snprintf(url, sizeof(url), "%s/Packages", s->url);
        s->ok = false;
        s->error[0] = 0;
        if (net_http_download(url, pp, 8u << 20, NULL, NULL, err, sizeof(err))) {
            snprintf(s->error, sizeof(s->error), "%s", err);
            pkg_log(job, "%s: %s", s->name, err);
            s->count = load_cached_source(i, fresh + nfresh, MAX_PKGS - nfresh);          /* fall back to what we had */
            nfresh += s->count;
            continue;
        }
        snprintf(url, sizeof(url), "%s/Packages.sig", s->url);
        if (net_http_download(url, sp, 4096, NULL, NULL, err, sizeof(err))) vfs_unlink(sp, false);
        s->count = load_cached_source(i, fresh + nfresh, MAX_PKGS - nfresh);
        nfresh += s->count;
        s->ok = s->count > 0 || !s->error[0];
        if (s->error[0]) pkg_log(job, "%s: %s", s->name, s->error);
        else pkg_log(job, "%s: %d package(s)", s->name, s->count);
    }
    spin_lock(&dlock);
    memcpy(pkgs, fresh, (size_t)nfresh * sizeof(*fresh));
    if (nfresh < npkgs) memset(pkgs + nfresh, 0, (size_t)(npkgs - nfresh) * sizeof(*fresh));
    npkgs = nfresh;
    spin_unlock(&dlock);
    kfree(fresh);
    mark_installed();
    mutex_unlock(&pkg_mtx);
    return npkgs;
}

int pkg_count(void) { return npkgs; }
const struct pkg *pkg_at(int i) { return i >= 0 && i < npkgs ? &pkgs[i] : NULL; }
int pkg_sources(const struct pkg_source **out) { *out = sources; return nsources; }

/* the best candidate for a name (or a package that provides it): highest version, verified sources first */
static const struct pkg *best(const char *name)
{
    const struct pkg *b = NULL;
    for (int i = 0; i < npkgs; i++) {
        bool match = !strcmp(pkgs[i].name, name);
        for (int k = 0; k < pkgs[i].nprov && !match; k++) match = !strcmp(pkgs[i].provides[k], name);
        if (!match) continue;
        if (!b || (pkgs[i].verified && !b->verified) || (pkgs[i].verified == b->verified && ver_cmp(pkgs[i].version, b->version) > 0)) b = &pkgs[i];
    }
    return b;
}

const struct pkg *pkg_lookup(const char *name) { return best(name); }

int pkg_search(const char *q, int *out, int max)
{
    int n = 0;
    for (int score = 4; score >= 1; score--) {
        for (int i = 0; i < npkgs && n < max; i++) {
            /* one entry per name: only the best version is listed */
            if (best(pkgs[i].name) != &pkgs[i]) continue;
            if (!strcmp(pkgs[i].type, "driver") || !strcmp(pkgs[i].type, "library")) { if (score != 0 && !*q) continue; }
            int s = 0;
            if (!*q) s = 1;
            else if (!strncasecmp(pkgs[i].name, q, strlen(q))) s = 4;
            else if (strstr_ci(pkgs[i].name, q)) s = 3;
            else if (strstr_ci(pkgs[i].desc, q)) s = 2;
            else if (strstr_ci(pkgs[i].category, q)) s = 1;
            if (s == score) out[n++] = i;
        }
    }
    return n;
}

int pkg_for_pci(uint16_t vendor, uint16_t device, int *out, int max)
{
    int n = 0;
    uint32_t id = (uint32_t)vendor << 16 | device;
    for (int i = 0; i < npkgs && n < max; i++) {
        if (strcmp(pkgs[i].type, "driver") || best(pkgs[i].name) != &pkgs[i]) continue;
        for (int k = 0; k < pkgs[i].npci; k++)
            if (pkgs[i].pci[k] == id) { out[n++] = i; break; }
    }
    return n;
}

bool pkg_is_installed(const char *name)
{
    for (int i = 0; i < ninst; i++) if (!strcmp(inst[i].name, name)) return true;
    return false;
}

int pkg_installed_list(const struct pkg **out, int max)
{
    int n = MIN(ninst, max);
    for (int i = 0; i < n; i++) out[i] = &inst[i];
    return n;
}

/* ------------------------------------------------------------------------
 * dependencies
 * ---------------------------------------------------------------------- */

/* does `have` satisfy a dependency written like "name (>= 1.2)"? also returns the bare name */
static bool dep_split(const char *dep, char *name, size_t nn, char *op, char *ver, size_t vn)
{
    op[0] = 0;
    ver[0] = 0;
    const char *p = strchr(dep, '(');
    size_t l = p ? (size_t)(p - dep) : strlen(dep);
    while (l && dep[l - 1] == ' ') l--;
    if (l >= nn) return false;
    memcpy(name, dep, l);
    name[l] = 0;
    if (p) {
        p++;
        while (*p == ' ') p++;
        int k = 0;
        while ((*p == '<' || *p == '>' || *p == '=') && k < 2) op[k++] = *p++;
        op[k] = 0;
        while (*p == ' ') p++;
        size_t i = 0;
        while (*p && *p != ')' && *p != ' ' && i + 1 < vn) ver[i++] = *p++;
        ver[i] = 0;
    }
    return true;
}

static bool ver_ok(const char *have, const char *op, const char *want)
{
    if (!op[0]) return true;
    int c = ver_cmp(have, want);
    if (!strcmp(op, ">=")) return c >= 0;
    if (!strcmp(op, "<=")) return c <= 0;
    if (!strcmp(op, ">")) return c > 0;
    if (!strcmp(op, "<")) return c < 0;
    return c == 0;
}

static int plan_rec(const char *name, const char *op, const char *ver, const struct pkg **order, int *n, int max,
                    int depth, char *err, size_t errn)
{
    for (int i = 0; i < ninst; i++)
        if (!strcmp(inst[i].name, name) && ver_ok(inst[i].version, op, ver)) return 0;      /* already there */
    for (int i = 0; i < *n; i++) if (!strcmp(order[i]->name, name)) return 0;
    const struct pkg *p = best(name);
    if (!p) { if (err) snprintf(err, errn, "%s is not in any source", name); return E_NOENT; }
    if (!ver_ok(p->version, op, ver)) {
        if (err) snprintf(err, errn, "%s %s is too old (%s %s needed)", name, p->version, op, ver);
        return E_NOENT;
    }
    if (depth > 12) { if (err) snprintf(err, errn, "dependency loop at %s", name); return E_INVAL; }
    for (int i = 0; i < p->ndeps; i++) {
        char dn[64], dop[4], dv[24];
        if (!dep_split(p->depends[i], dn, sizeof(dn), dop, dv, sizeof(dv))) continue;
        int r = plan_rec(dn, dop, dv, order, n, max, depth + 1, err, errn);
        if (r) {
            if (err && r == E_NOENT) {
                char tmp[200];
                snprintf(tmp, sizeof(tmp), "%s needs %s", name, err);
                strlcpy(err, tmp, errn);
            }
            return r;
        }
    }
    if (*n >= max) { if (err) snprintf(err, errn, "too many packages in one install"); return E_NOMEM; }
    order[(*n)++] = p;
    return 0;
}

int pkg_plan(const char *name, const struct pkg **order, int max, char *err, size_t errn)
{
    int n = 0;
    int r = plan_rec(name, "", "", order, &n, max, 0, err, errn);
    return r ? r : n;
}

/* ------------------------------------------------------------------------
 * install and remove
 * ---------------------------------------------------------------------- */

struct dlctx { struct pkg_job *job; const struct pkg *p; int base, span; };

static void dl_progress(void *c, uint64_t done, uint64_t total)
{
    struct dlctx *x = c;
    if (!x->job || !total) return;
    x->job->percent = x->base + (int)((uint64_t)x->span * done / total);
}

static bool check_sha256(const char *path, const uint8_t *want)
{
    vnode_t *vn = vfs_open(path, false);
    if (!vn) return false;
    struct hash_ctx h;
    hash_init(&h, HASH_SHA256);
    uint8_t *buf = kmalloc(65536);
    uint64_t off = 0;
    long n;
    while (buf && (n = vfs_read(vn, off, buf, 65536)) > 0) {
        hash_update(&h, buf, (size_t)n);
        off += (uint64_t)n;
    }
    kfree(buf);
    vfs_close(vn);
    uint8_t got[32];
    hash_final(&h, got);
    return ct_equal(got, want, 32);
}

static void write_info(const struct pkg *p)
{
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", p->sha256[i]);
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "Package: %s\nVersion: %s\nType: %s\nArch: %s\nCategory: %s\nDescription: %s\n"
                     "Module: %s\nPersonality: %s\nFormat: %s\nSHA256: %s\nInstalled-Size: %lu\nHomepage: %s\n",
                     p->name, p->version, p->type, p->arch, p->category, p->desc, p->module, p->personality, p->format,
                     hex, p->installed_size, p->homepage);
    if (p->npci) {
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "Match-PCI:");
        for (int i = 0; i < p->npci; i++) n += snprintf(buf + n, sizeof(buf) - (size_t)n, " %04x:%04x", p->pci[i] >> 16, p->pci[i] & 0xFFFF);
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\n");
    }
    n += snprintf(buf + n, sizeof(buf) - (size_t)n, "\n");
    char path[VFS_PATH_MAX];
    under(path, sizeof(path), "/var/lib/zenith/pkg/%s.info", p->name);
    char dbdir[VFS_PATH_MAX];
    under(dbdir, sizeof(dbdir), "/var/lib/zenith/pkg");
    sys_mkdir_p(dbdir);
    vfs_write_file(path, buf, (size_t)n);
}

/* copy the package's launchers into /opt/share/applications with @PKG@ replaced by its folder */
static void install_launchers(const char *name, const char *dir)
{
    char sdir[VFS_PATH_MAX];
    snprintf(sdir, sizeof(sdir), "%s/share/applications", dir);
    struct vfs_dirent de;
    char adir[VFS_PATH_MAX];
    under(adir, sizeof(adir), "/opt/share/applications");
    sys_mkdir_p(adir);
    for (int i = 0; vfs_readdir(sdir, i, &de) == 0; i++) {
        size_t l = strlen(de.name);
        if (l < 6 || strcmp(de.name + l - 5, ".zapp")) continue;
        char from[VFS_PATH_MAX], to[VFS_PATH_MAX];
        snprintf(from, sizeof(from), "%s/%s", sdir, de.name);
        under(to, sizeof(to), "/opt/share/applications/%s", de.name);
        size_t sz;
        char *t = vfs_read_file(from, &sz);
        if (!t) continue;
        char *out = kmalloc(sz * 2 + 512);
        size_t o = 0;
        for (char *p = t; *p;) {
            if (!strncmp(p, "@PKG@", 5)) {
                o += (size_t)snprintf(out + o, 200, "/opt/%s", name);
                p += 5;
            } else {
                out[o++] = *p++;
            }
        }
        out[o] = 0;
        vfs_write_file(to, out, o);
        kfree(out);
        kfree(t);
    }
}

static void remove_launchers(const char *name)
{
    char dir[VFS_PATH_MAX];
    under(dir, sizeof(dir), "/opt/%s/share/applications", name);
    struct vfs_dirent de;
    for (int i = 0; vfs_readdir(dir, i, &de) == 0; i++) {
        char p[VFS_PATH_MAX];
        under(p, sizeof(p), "/opt/share/applications/%s", de.name);
        vfs_unlink(p, false);
    }
}

/* download, check and unpack one package; leaves it in /opt/<name> and the info file written */
static int install_one(const struct pkg *p, struct pkg_job *job, int base, int span)
{
    char url[400], cache[VFS_PATH_MAX], err[160] = "";
    const struct pkg_source *s = &sources[p->source];
    snprintf(url, sizeof(url), "%s/%s", s->url, p->filename);
    sys_mkdir_p("/var/cache/zenith");
    snprintf(cache, sizeof(cache), "/var/cache/zenith/%s_%s.zpk", p->name, p->version);
    stage(job, base, "Downloading %s %s", p->name, p->version);
    struct dlctx dc = { job, p, base, span * 6 / 10 };
    if (net_http_download(url, cache, MAX_PKG_BYTES, dl_progress, &dc, err, sizeof(err))) {
        if (job) snprintf(job->error, sizeof(job->error), "%s: %s", p->name, err);
        return E_IO;
    }
    stage(job, base + span * 6 / 10, "Checking %s", p->name);
    if (!check_sha256(cache, p->sha256)) {
        vfs_unlink(cache, false);
        if (job) snprintf(job->error, sizeof(job->error), "%s: the download does not match its checksum", p->name);
        return E_IO;
    }
    if (job && job->cancel) { vfs_unlink(cache, false); snprintf(job->error, sizeof(job->error), "cancelled"); return E_IO; }
    stage(job, base + span * 7 / 10, "Unpacking %s", p->name);
    size_t zn;
    uint8_t *z = (uint8_t *)vfs_read_file(cache, &zn);
    vfs_unlink(cache, false);
    if (!z) { if (job) snprintf(job->error, sizeof(job->error), "%s: out of memory", p->name); return E_NOMEM; }
    size_t tn;
    uint8_t *tar = gunzip_alloc(z, zn, &tn, 1ull << 30);
    kfree(z);
    if (!tar) { if (job) snprintf(job->error, sizeof(job->error), "%s: the archive is damaged", p->name); return E_IO; }
    /* the manifest has to say this is the package we asked for */
    size_t mn;
    const uint8_t *man = tar_find(tar, tn, "MANIFEST", &mn);
    bool manifest_ok = false;
    if (man) {
        char *mt = kmalloc(mn + 1);
        memcpy(mt, man, mn);
        mt[mn] = 0;
        struct pkg m;
        memset(&m, 0, sizeof(m));
        char *copy = mt;
        if (parse_stanzas(copy, &m, 1, -1, true) == 1 && !strcmp(m.name, p->name)) manifest_ok = true;
        kfree(mt);
    }
    if (!manifest_ok) {
        kfree(tar);
        if (job) snprintf(job->error, sizeof(job->error), "%s: the package has no valid MANIFEST", p->name);
        return E_INVAL;
    }
    char part[VFS_PATH_MAX], fin[VFS_PATH_MAX], old[VFS_PATH_MAX];
    under(part, sizeof(part), "/opt/%s.partial", p->name);
    under(fin, sizeof(fin), "/opt/%s", p->name);
    under(old, sizeof(old), "/opt/%s.old", p->name);
    vfs_unlink(part, true);
    int files = 0;
    int r = tar_extract(tar, tn, part, err, sizeof(err), &files);
    kfree(tar);
    if (r) {
        vfs_unlink(part, true);
        if (job) snprintf(job->error, sizeof(job->error), "%s: %s", p->name, err);
        return r;
    }
    vfs_unlink(old, true);
    if (vfs_lookup(fin)) vfs_rename(fin, old);               /* an older version: kept until this one works */
    if (vfs_rename(part, fin)) {
        if (vfs_lookup(old)) vfs_rename(old, fin);
        vfs_unlink(part, true);
        if (job) snprintf(job->error, sizeof(job->error), "%s: cannot move into place", p->name);
        return E_IO;
    }
    stage(job, base + span * 9 / 10, "Setting up %s", p->name);
    install_launchers(p->name, fin);
    write_info(p);
    vfs_unlink(old, true);
    pkg_log(job, "installed %s %s (%d files)", p->name, p->version, files);
    return 0;
}

static void remove_files(const char *name)
{
    char path[VFS_PATH_MAX];
    remove_launchers(name);
    under(path, sizeof(path), "/opt/%s", name);
    vfs_unlink(path, true);
    under(path, sizeof(path), "/var/lib/zenith/pkg/%s.info", name);
    vfs_unlink(path, false);
}

int pkg_remove(const char *name, struct pkg_job *job)
{
    mutex_lock(&pkg_mtx);
    int r = 0;
    if (!pkg_is_installed(name)) {
        if (job) snprintf(job->error, sizeof(job->error), "%s is not installed", name);
        r = E_NOENT;
    } else {
        /* refuse while something installed still needs it */
        for (int i = 0; i < ninst && !r; i++) {
            for (int k = 0; k < inst[i].ndeps; k++) {
                char dn[64], op[4], v[24];
                dep_split(inst[i].depends[k], dn, sizeof(dn), op, v, sizeof(v));
                if (!strcmp(dn, name) && strcmp(inst[i].name, name)) {
                    if (job) snprintf(job->error, sizeof(job->error), "%s is needed by %s", name, inst[i].name);
                    r = E_INVAL;
                }
            }
        }
        if (!r) {
            remove_files(name);
            pkg_log(job, "removed %s", name);
            load_installed();
        }
    }
    mutex_unlock(&pkg_mtx);
    if (job) { job->result = r; job->percent = 100; job->finished = true; }
    return r;
}

int pkg_install(const char *name, struct pkg_job *job)
{
    mutex_lock(&pkg_mtx);
    const struct pkg *plan[MAX_PLAN];
    char err[200] = "";
    stage(job, 0, "Working out what %s needs", name);
    int n = pkg_plan(name, plan, MAX_PLAN, err, sizeof(err));
    int r = n < 0 ? n : 0;
    const char *done_names[MAX_PLAN];
    int ndone = 0;
    if (n < 0) {
        if (job) strlcpy(job->error, err, sizeof(job->error));
    } else if (n == 0) {
        if (job) strlcpy(job->error, "already installed", sizeof(job->error));
    } else {
        if (n > 1) pkg_log(job, "%s needs %d package(s) in total", name, n);
        for (int i = 0; i < n && !r; i++) {
            int base = 100 * i / n, span = 100 / n;
            r = install_one(plan[i], job, base, span);
            if (!r) done_names[ndone++] = plan[i]->name;
            load_installed();
        }
        if (r) {
            /* all or nothing: take back everything this transaction put on the disk */
            pkg_log(job, "installing failed, removing what was downloaded");
            for (int i = 0; i < ndone; i++) remove_files(done_names[i]);
            for (int i = 0; i < n; i++) {
                char p[VFS_PATH_MAX];
                under(p, sizeof(p), "/opt/%s.partial", plan[i]->name);
                vfs_unlink(p, true);
                snprintf(p, sizeof(p), "/var/cache/zenith/%s_%s.zpk", plan[i]->name, plan[i]->version);
                vfs_unlink(p, false);
            }
            load_installed();
        }
    }
    mutex_unlock(&pkg_mtx);
    if (job) { job->result = r; job->percent = 100; job->finished = true; if (!r) stage(job, 100, "Done"); }
    return r;
}

/* ------------------------------------------------------------------------
 * what the store shows
 * ---------------------------------------------------------------------- */

static void brief_of(const struct pkg *p, struct pkg_brief *b)
{
    memset(b, 0, sizeof(*b));
    strlcpy(b->name, p->name, sizeof(b->name));
    strlcpy(b->title, p->title[0] ? p->title : p->name, sizeof(b->title));
    strlcpy(b->version, p->version, sizeof(b->version));
    strlcpy(b->type, p->type, sizeof(b->type));
    strlcpy(b->category, p->category[0] ? p->category : "Other", sizeof(b->category));
    strlcpy(b->desc, p->desc, sizeof(b->desc));
    strlcpy(b->installed_version, p->installed_version, sizeof(b->installed_version));
    if (p->source >= 0 && p->source < nsources) strlcpy(b->source, sources[p->source].name, sizeof(b->source));
    b->size = p->size;
    b->installed = p->installed;
    b->verified = p->verified;
    b->update = p->installed && ver_cmp(p->version, p->installed_version) > 0;
}

int pkg_search_brief(const char *q, const char *category, bool installed_only, struct pkg_brief *out, int max)
{
    int n = 0;
    spin_lock(&dlock);
    if (installed_only) {
        /* what is installed, whether or not any source still lists it */
        for (int i = 0; i < ninst && n < max; i++) {
            if (!strcmp(inst[i].type, "driver") && !*q) { /* drivers are listed too: they are installed software */ }
            if (*q && !strstr_ci(inst[i].name, q) && !strstr_ci(inst[i].title, q) && !strstr_ci(inst[i].desc, q)) continue;
            brief_of(&inst[i], &out[n]);
            out[n].installed = true;
            strlcpy(out[n].installed_version, inst[i].version, sizeof(out[n].installed_version));
            for (int k = 0; k < npkgs; k++)
                if (!strcmp(pkgs[k].name, inst[i].name) && ver_cmp(pkgs[k].version, inst[i].version) > 0) out[n].update = true;
            n++;
        }
        spin_unlock(&dlock);
        return n;
    }
    for (int score = 4; score >= 1; score--) {
        for (int i = 0; i < npkgs && n < max; i++) {
            const struct pkg *p = &pkgs[i];
            if (best(p->name) != p) continue;                    /* one entry per name: the best version */
            if (!*q && (!strcmp(p->type, "driver") || !strcmp(p->type, "library"))) continue;
            if (category && *category && strcasecmp(category, p->category[0] ? p->category : "Other")) continue;
            int s = 0;
            if (!*q) s = 1;
            else if (!strncasecmp(p->name, q, strlen(q)) || !strncasecmp(p->title, q, strlen(q))) s = 4;
            else if (strstr_ci(p->name, q) || strstr_ci(p->title, q)) s = 3;
            else if (strstr_ci(p->desc, q)) s = 2;
            else if (strstr_ci(p->category, q)) s = 1;
            for (int k = 0; k < p->nprov && !s; k++) if (strstr_ci(p->provides[k], q)) s = 1;
            if (s != score) continue;
            brief_of(p, &out[n++]);
        }
    }
    spin_unlock(&dlock);
    return n;
}

int pkg_categories(char out[][24], int *counts, int max)
{
    int n = 0;
    spin_lock(&dlock);
    for (int i = 0; i < npkgs; i++) {
        const struct pkg *p = &pkgs[i];
        if (best(p->name) != p || !strcmp(p->type, "driver") || !strcmp(p->type, "library")) continue;
        const char *c = p->category[0] ? p->category : "Other";
        int k = 0;
        while (k < n && strcasecmp(out[k], c)) k++;
        if (k == n) {
            if (n >= max) continue;
            strlcpy(out[n], c, 24);
            counts[n++] = 0;
        }
        counts[k]++;
    }
    spin_unlock(&dlock);
    return n;
}

bool pkg_full(const char *name, struct pkg *out)
{
    spin_lock(&dlock);
    const struct pkg *p = best(name);
    if (p) *out = *p;
    spin_unlock(&dlock);
    return p != NULL;
}

/* ------------------------------------------------------------------------
 * will it run here?
 * ---------------------------------------------------------------------- */

__attribute__((weak)) bool abi_available(const char *personality)
{
    return !personality[0] || !strcmp(personality, "zenith");
}

bool sys_has_feature(const char *name)
{
    if (!strcmp(name, "net")) {
        struct net_info ni;
        return net_get_info(&ni);
    }
    if (!strcmp(name, "display:wm")) return true;
    if (!strcmp(name, "audio")) return audio_available();
    if (!strncmp(name, "abi:", 4)) return abi_available(name + 4);
    return false;
}

void pkg_verdict_for(const char *name, struct pkg_verdict *v)
{
    memset(v, 0, sizeof(*v));
    struct pkg p;
    if (!pkg_full(name, &p)) {
        v->code = PV_UNKNOWN;
        snprintf(v->text, sizeof(v->text), "%s is not in any package source", name);
        return;
    }
    if (strcmp(p.arch, "x86_64") && strcmp(p.arch, "all")) {
        v->code = PV_BAD_ARCH;
        snprintf(v->text, sizeof(v->text), "built for %s, this computer is x86_64", p.arch);
        return;
    }
    if (p.personality[0] && !abi_available(p.personality)) {
        v->code = PV_NO_ABI;
        snprintf(v->text, sizeof(v->text), "needs the \"%s\" program interface, which this system does not provide", p.personality);
        return;
    }
    for (int i = 0; i < p.nreq; i++) {
        if (!sys_has_feature(p.requires[i])) {
            v->code = PV_NO_FEATURE;
            snprintf(v->text, sizeof(v->text), "needs %s, which this computer or this version of ZenithOS does not offer", p.requires[i]);
            return;
        }
    }
    const struct pkg *plan[MAX_PLAN];
    char err[200] = "";
    int n = pkg_plan(name, plan, MAX_PLAN, err, sizeof(err));
    if (n < 0) {
        v->code = PV_MISSING_DEP;
        strlcpy(v->text, err, sizeof(v->text));
        return;
    }
    for (int i = 0; i < n; i++) {
        v->download += plan[i]->size;
        v->installed += plan[i]->installed_size;
    }
    v->extra_packages = n > 1 ? n - 1 : 0;
    if (n > 1) {
        v->code = PV_NEEDS_DEPS;
        snprintf(v->text, sizeof(v->text), "installs %d more package%s it needs", n - 1, n == 2 ? "" : "s");
    } else {
        v->code = PV_OK;
        snprintf(v->text, sizeof(v->text), "%s", n == 0 ? "already installed" : "ready to install");
    }
}
