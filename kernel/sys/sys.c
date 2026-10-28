/*
 * Boot-time system setup: find the persistent system volume (if any), load
 * the system configuration from /etc and apply it: language, keyboard
 * layout, time zone, host name, clock.
 */
#include <kernel.h>
#include <mm.h>
#include <vfs.h>
#include <dev.h>
#include <sys.h>

struct conf sysconf;
static char persist_root[64];
static char hostname[64] = "";

bool sys_persistent(void) { return persist_root[0]; }
const char *sys_persist_root(void) { return persist_root[0] ? persist_root : NULL; }

int sys_mkdir_p(const char *path)
{
    char tmp[VFS_PATH_MAX];
    strlcpy(tmp, path, sizeof(tmp));
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        int r = vfs_mkdir(tmp);
        *p = '/';
        if (r && r != E_EXIST) return r;
    }
    int r = vfs_mkdir(tmp);
    return r == E_EXIST ? 0 : r;
}

static int write_file_p(const char *path, const void *data, size_t n)
{
    char dir[VFS_PATH_MAX];
    vfs_dirname(path, dir, sizeof(dir));
    sys_mkdir_p(dir);
    return vfs_write_file(path, data, n);
}

int sys_write_etc(const char *rel, const void *data, size_t n)
{
    char path[VFS_PATH_MAX];
    snprintf(path, sizeof(path), "/etc/%s", rel);
    int r = write_file_p(path, data, n);
    if (persist_root[0]) {
        snprintf(path, sizeof(path), "%s/zenith/etc/%s", persist_root, rel);
        int r2 = write_file_p(path, data, n);
        if (!r) r = r2;
    }
    return r;
}

/* copy <volume>/zenith/etc over /etc */
static void overlay(const char *from, const char *to)
{
    struct vfs_dirent de;
    for (int i = 0; vfs_readdir(from, i, &de) == 0; i++) {
        char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
        snprintf(a, sizeof(a), "%s/%s", from, de.name);
        snprintf(b, sizeof(b), "%s/%s", to, de.name);
        if (de.type == VN_DIR) {
            vfs_mkdir(b);
            overlay(a, b);
        } else {
            size_t n;
            char *d = vfs_read_file(a, &n);
            if (d) { vfs_write_file(b, d, n); kfree(d); }
        }
    }
}

bool sys_valid_hostname(const char *name)
{
    size_t l = strlen(name);
    if (!l || l > 63 || name[0] == '-' || name[l - 1] == '-') return false;
    for (size_t i = 0; i < l; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '-') return false;
    return true;
}

const char *sys_hostname(void)
{
    if (!hostname[0]) {
        size_t n;
        char *d = vfs_read_file("/etc/hostname", &n);
        if (d) {
            for (size_t i = 0; i < n && d[i] != '\n' && d[i] != '\r' && i + 1 < sizeof(hostname); i++) hostname[i] = d[i];
            kfree(d);
        }
        if (!sys_valid_hostname(hostname)) strlcpy(hostname, "localhost", sizeof(hostname));
    }
    return hostname;
}

int sys_set_hostname(const char *name)
{
    if (!sys_valid_hostname(name)) return E_INVAL;
    strlcpy(hostname, name, sizeof(hostname));
    char line[72];
    int n = snprintf(line, sizeof(line), "%s\n", name);
    return sys_write_etc("hostname", line, (size_t)n);
}

void sys_save_conf(void)
{
    size_t n;
    char *t = conf_format(&sysconf, &n);
    if (!t) return;
    sys_write_etc("zenith/system.conf", t, n);
    kfree(t);
}

void sys_init(void)
{
    conf_init(&sysconf);
    conf_load(&sysconf, "/etc/zenith/system.conf");            /* the image's defaults ... */
    /* a volume with zenith/persist is a system volume: its zenith/etc replaces the image's defaults */
    for (struct fs_mount *m = vfs_mounts(); m && !persist_root[0]; m = m->next) {
        char p[96];
        snprintf(p, sizeof(p), "%s/zenith/persist", m->path);
        if (vfs_lookup(p)) strlcpy(persist_root, m->path, sizeof(persist_root));
    }
    if (persist_root[0]) {
        char from[96];
        snprintf(from, sizeof(from), "%s/zenith/etc", persist_root);
        if (vfs_lookup(from)) overlay(from, "/etc");
        /* /home, /opt (installed apps and drivers) and /var live on the volume */
        static const struct { const char *at, *rel; } dirs[] = {
            { "/home", "home" }, { "/opt", "zenith/opt" }, { "/var", "zenith/var" },
        };
        for (unsigned i = 0; i < ARRAY_SIZE(dirs); i++) {
            char target[96];
            snprintf(target, sizeof(target), "%s/%s", persist_root, dirs[i].rel);
            sys_mkdir_p(target);
            vfs_alias(dirs[i].at, target);
        }
        klog("sys: system volume at %s", persist_root);
    } else {
        klog("sys: live session, nothing is stored");
    }
    conf_load(&sysconf, "/etc/zenith/system.conf");            /* ... overridden by what the installed system saved */

    const char *lang = conf_get(&sysconf, "language", "en");
    if (!i18n_set(lang)) i18n_set("en");
    const char *kbd = conf_get(&sysconf, "keyboard", "us");
    if (!keymap_set(kbd)) keymap_set("us");
    rtc_is_local = conf_int(&sysconf, "rtc_local", 0) != 0;
    const char *zone = conf_get(&sysconf, "timezone", "UTC");
    if (!tz_set(zone)) tz_set("UTC");
    rtc_apply_zone();
    klog("sys: host %s, language %s, keyboard %s, time zone %s", sys_hostname(), i18n_current(), keymap_current(),
         tz_name());
    users_init();
}
