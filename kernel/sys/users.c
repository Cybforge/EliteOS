/*
 * User accounts and the login session.
 *
 * /etc/zenith/users has one account per line:
 *     name:uid:admin:full name:$pbkdf2-sha256$iterations$salt$hash
 * (salt and hash in base64; an empty password field means "no password").
 * Passwords are never stored, only salted PBKDF2-HMAC-SHA256 hashes.
 */
#include <kernel.h>
#include <mm.h>
#include <vfs.h>
#include <crypto.h>
#include <dev.h>
#include <sys.h>

#define MAX_USERS 16
#define SALT_LEN 16
#define HASH_LEN 32

struct session session;
static struct account accts[MAX_USERS];
static int nacct;

/* ---- base64 ---- */

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64enc(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = b64chars[v >> 18];
        out[o++] = b64chars[(v >> 12) & 63];
        out[o++] = i + 1 < n ? b64chars[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? b64chars[v & 63] : '=';
    }
    out[o] = 0;
}

static int b64dec(const char *in, uint8_t *out, size_t max)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (; *in && *in != '='; in++) {
        const char *p = strchr(b64chars, *in);
        if (!p || !*in) return -1;
        acc = acc << 6 | (uint32_t)(p - b64chars);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= max) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)n;
}

/* ---- hashing ---- */

static void make_hash(const char *password, char *out, size_t outn)
{
    if (!password || !*password) { out[0] = 0; return; }
    uint8_t salt[SALT_LEN], dk[HASH_LEN];
    random_bytes(salt, sizeof(salt));
    uint32_t iter = (uint32_t)conf_int(&sysconf, "pbkdf2_iterations", 30000);
    pbkdf2_sha256((const uint8_t *)password, strlen(password), salt, sizeof(salt), iter, dk, sizeof(dk));
    char s[32], h[64];
    b64enc(salt, sizeof(salt), s);
    b64enc(dk, sizeof(dk), h);
    snprintf(out, outn, "$pbkdf2-sha256$%u$%s$%s", iter, s, h);
}

static bool check_hash(const char *hash, const char *password)
{
    if (!hash[0]) return !password || !*password;           /* no password set: only the empty one fits */
    if (strncmp(hash, "$pbkdf2-sha256$", 15)) return false;
    char *end;
    uint32_t iter = (uint32_t)strtol(hash + 15, &end, 10);
    if (*end != '$' || !iter || iter > 5000000) return false;
    char tmp[128];
    strlcpy(tmp, end + 1, sizeof(tmp));
    char *dollar = strchr(tmp, '$');
    if (!dollar) return false;
    *dollar = 0;
    uint8_t salt[SALT_LEN + 4], want[HASH_LEN + 4], dk[HASH_LEN];
    int sl = b64dec(tmp, salt, sizeof(salt)), wl = b64dec(dollar + 1, want, sizeof(want));
    if (sl != SALT_LEN || wl != HASH_LEN) return false;
    pbkdf2_sha256((const uint8_t *)(password ? password : ""), password ? strlen(password) : 0, salt, (size_t)sl,
                  iter, dk, sizeof(dk));
    return ct_equal(dk, want, HASH_LEN);
}

/* ---- the accounts file ---- */

bool users_valid_name(const char *n)
{
    size_t l = strlen(n);
    if (!l || l > 31 || !(islower((unsigned char)n[0]) || n[0] == '_')) return false;
    for (size_t i = 0; i < l; i++) {
        unsigned char c = (unsigned char)n[i];
        if (!(islower(c) || isdigit(c) || c == '_' || c == '-')) return false;
    }
    return true;
}

static int save(void)
{
    char buf[MAX_USERS * 256];
    size_t o = 0;
    for (int i = 0; i < nacct; i++)
        o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%s:%d:%d:%s:%s\n", accts[i].name, accts[i].uid,
                              accts[i].admin ? 1 : 0, accts[i].fullname, accts[i].hash);
    return sys_write_etc("zenith/users", buf, o);
}

void users_init(void)
{
    nacct = 0;
    size_t n;
    char *d = vfs_read_file("/etc/zenith/users", &n);
    if (!d) { klog("users: no accounts file"); return; }
    for (char *line = d; line && *line && nacct < MAX_USERS;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *f[5];
        int nf = 0;
        for (char *p = line; nf < 5;) {
            f[nf++] = p;
            char *c = nf < 5 ? strchr(p, ':') : NULL;
            if (!c) break;
            *c = 0;
            p = c + 1;
        }
        if (nf == 5 && users_valid_name(f[0])) {
            struct account *a = &accts[nacct++];
            strlcpy(a->name, f[0], sizeof(a->name));
            a->uid = atoi(f[1]);
            a->admin = atoi(f[2]) != 0;
            strlcpy(a->fullname, f[3], sizeof(a->fullname));
            strlcpy(a->hash, f[4], sizeof(a->hash));
        }
        line = next;
    }
    kfree(d);
    klog("users: %d account(s)", nacct);
}

int users_count(void) { return nacct; }
const struct account *users_get(int i) { return i >= 0 && i < nacct ? &accts[i] : NULL; }

const struct account *users_find(const char *name)
{
    for (int i = 0; i < nacct; i++)
        if (!strcmp(accts[i].name, name)) return &accts[i];
    return NULL;
}

bool users_verify(const char *name, const char *password)
{
    const struct account *a = users_find(name);
    return a && check_hash(a->hash, password);
}

int users_add(const char *name, const char *fullname, const char *password, bool admin)
{
    if (!users_valid_name(name)) return E_INVAL;
    if (users_find(name)) return E_EXIST;
    if (nacct >= MAX_USERS) return E_NOMEM;
    struct account *a = &accts[nacct];
    memset(a, 0, sizeof(*a));
    strlcpy(a->name, name, sizeof(a->name));
    strlcpy(a->fullname, fullname && *fullname ? fullname : name, sizeof(a->fullname));
    for (char *c = a->fullname; *c; c++) if (*c == ':' || *c == '\n') *c = ' ';
    int uid = 1000;
    for (int i = 0; i < nacct; i++) if (accts[i].uid >= uid) uid = accts[i].uid + 1;
    a->uid = uid;
    a->admin = admin;
    make_hash(password, a->hash, sizeof(a->hash));
    nacct++;
    return save();
}

int users_set_password(const char *name, const char *password)
{
    for (int i = 0; i < nacct; i++)
        if (!strcmp(accts[i].name, name)) {
            make_hash(password, accts[i].hash, sizeof(accts[i].hash));
            return save();
        }
    return E_NOENT;
}

int users_remove(const char *name)
{
    for (int i = 0; i < nacct; i++)
        if (!strcmp(accts[i].name, name)) {
            memmove(&accts[i], &accts[i + 1], (size_t)(nacct - i - 1) * sizeof(accts[0]));
            nacct--;
            return save();
        }
    return E_NOENT;
}

bool users_need_login(void)
{
    const char *auto_user = conf_get(&sysconf, "autologin", "");
    if (*auto_user && users_find(auto_user)) return false;
    if (nacct == 1 && !accts[0].hash[0]) return false;
    return nacct > 0;
}

/* ---- session ---- */

static void copy_tree(const char *from, const char *to)
{
    struct vfs_dirent de;
    for (int i = 0; vfs_readdir(from, i, &de) == 0; i++) {
        char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
        snprintf(a, sizeof(a), "%s/%s", from, de.name);
        snprintf(b, sizeof(b), "%s/%s", to, de.name);
        if (de.type == VN_DIR) {
            vfs_mkdir(b);
            copy_tree(a, b);
        } else if (!vfs_lookup(b)) {
            vfs_copy(a, b);
        }
    }
}

int session_create_home(const char *name, char *home, size_t n)
{
    const char *root = sys_persist_root();
    snprintf(home, n, "%s/home/%s", root ? root : "", name);
    bool fresh = !vfs_lookup(home);
    int r = sys_mkdir_p(home);
    if (r) return r;
    if (fresh) copy_tree("/etc/skel", home);
    static const char *dirs[] = { "Desktop", "Documents", "Pictures", "Downloads", ".config/zenith" };
    for (unsigned i = 0; i < ARRAY_SIZE(dirs); i++) {
        char p[VFS_PATH_MAX];
        snprintf(p, sizeof(p), "%s/%s", home, dirs[i]);
        sys_mkdir_p(p);
    }
    return 0;
}

bool session_login(const char *name, const char *password)
{
    const struct account *a = users_find(name);
    if (!a || !check_hash(a->hash, password)) return false;
    if (session.active) session_logout();
    memset(&session, 0, sizeof(session));
    strlcpy(session.user, a->name, sizeof(session.user));
    strlcpy(session.fullname, a->fullname, sizeof(session.fullname));
    session.uid = a->uid;
    session.admin = a->admin;
    session_create_home(a->name, session.home, sizeof(session.home));
    conf_init(&session.prefs);
    char p[VFS_PATH_MAX];
    snprintf(p, sizeof(p), "%s/.config/zenith/settings.conf", session.home);
    conf_load(&session.prefs, p);
    session.active = true;
    klog("session: %s logged in, home %s", session.user, session.home);
    return true;
}

void session_logout(void)
{
    if (!session.active) return;
    session_save_prefs();
    conf_free(&session.prefs);
    session.active = false;
    session.user[0] = 0;
    session.home[0] = 0;
}

const char *session_home(void)
{
    return session.active ? session.home : "/tmp";
}

bool session_path(const char *rel, char *out, size_t n)
{
    int w = snprintf(out, n, "%s/%s", session_home(), rel);
    return w > 0 && (size_t)w < n;
}

void session_save_prefs(void)
{
    if (!session.active) return;
    size_t n;
    char *t = conf_format(&session.prefs, &n);
    if (!t) return;
    char p[VFS_PATH_MAX];
    snprintf(p, sizeof(p), "%s/.config/zenith/settings.conf", session.home);
    vfs_write_file(p, t, n);
    kfree(t);
}
