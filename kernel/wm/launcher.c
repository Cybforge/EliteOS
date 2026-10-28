/*
 * Application launchers: one small .zapp file per app, in
 * /usr/share/applications (system-wide, also where the store installs) and
 * ~/.local/share/applications:
 *
 *     name=Terminal
 *     name[de]=Terminal            (optional per-language names)
 *     comment=Command line shell
 *     icon=terminal                (a built-in icon name)
 *     exec=@terminal               (a built-in app) or /apps/foo/bin/foo --arg
 *     category=System
 *
 * The start menu, the desktop and the taskbar are built from these files, so
 * installing or removing an app is nothing more than adding or deleting one.
 */
#include <wm.h>
#include <vfs.h>
#include <mm.h>
#include <sched.h>
#include <sys.h>
struct abi_proc { int pid; process_t *legacy; struct sv_group *sv; };
int abi_exec(const char *path, int argc, char **argv, struct tty *tty, const char *cwd, struct abi_proc *out);

#define MAX_LAUNCHERS 256

static struct launcher *list;
static int nlist;

static int cmp_name(const void *a, const void *b)
{
    return strcasecmp(((const struct launcher *)a)->name, ((const struct launcher *)b)->name);
}

static void load_dir(const char *dir)
{
    struct vfs_dirent de;
    for (int i = 0; nlist < MAX_LAUNCHERS && vfs_readdir(dir, i, &de) == 0; i++) {
        size_t l = strlen(de.name);
        if (l < 6 || strcmp(de.name + l - 5, ".zapp")) continue;
        char path[VFS_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, de.name);
        struct conf c;
        conf_init(&c);
        if (conf_load(&c, path)) {
            const char *exec = conf_get(&c, "exec", "");
            if (*exec) {
                struct launcher *e = &list[nlist++];
                memset(e, 0, sizeof(*e));
                strlcpy(e->id, de.name, MIN(l - 4, sizeof(e->id)));
                char key[24];
                snprintf(key, sizeof(key), "name[%s]", i18n_current());
                strlcpy(e->name, conf_get(&c, key, conf_get(&c, "name", e->id)), sizeof(e->name));
                snprintf(key, sizeof(key), "comment[%s]", i18n_current());
                strlcpy(e->desc, conf_get(&c, key, conf_get(&c, "comment", "")), sizeof(e->desc));
                strlcpy(e->exec, exec, sizeof(e->exec));
                strlcpy(e->category, conf_get(&c, "category", "Other"), sizeof(e->category));
                const char *ic = conf_get(&c, "icon", "app");
                e->icon = icon_by_name(ic);
                if (!e->icon) e->icon = ICON_APP;
                e->builtin = exec[0] == '@';
            }
        }
        conf_free(&c);
    }
}

void launchers_reload(void)
{
    if (!list) list = kzalloc(MAX_LAUNCHERS * sizeof(*list));
    if (!list) return;
    nlist = 0;
    load_dir("/usr/share/applications");
    load_dir("/opt/share/applications");                      /* what the store installed */
    char p[VFS_PATH_MAX];
    if (session_path(".local/share/applications", p, sizeof(p))) load_dir(p);
    qsort(list, (size_t)nlist, sizeof(*list), cmp_name);
}

int launchers_count(void) { return nlist; }
const struct launcher *launcher_get(int i) { return i >= 0 && i < nlist ? &list[i] : NULL; }

const struct launcher *launcher_find(const char *id)
{
    for (int i = 0; i < nlist; i++)
        if (!strcmp(list[i].id, id)) return &list[i];
    return NULL;
}

/* "@terminal" starts a built-in app, anything else is a program path with arguments */
void launcher_run(const struct launcher *l)
{
    if (!l) return;
    if (l->exec[0] == '@') {
        app_launch(l->exec + 1);
        return;
    }
    char buf[sizeof(l->exec)];
    strlcpy(buf, l->exec, sizeof(buf));
    char *argv[16];
    int argc = 0;
    for (char *p = buf; *p && argc < 15;) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    argv[argc] = NULL;
    if (!argc) return;
    struct abi_proc ap;
    if (abi_exec(argv[0], argc, argv, NULL, session_home(), &ap)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s could not be started", argv[0]);
        wm_notify(l->name, msg, l->icon);
    }
}
