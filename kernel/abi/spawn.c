/*
 * Starting programs of either kind from the kernel (shell, launchers): ZenithOS
 * programs use the ZenithOS system call interface, everything else the System V
 * interface. A program says it is a ZenithOS one with a ".zenith.abi" section.
 */
#include <kernel.h>
#include <mm.h>
#include <sched.h>
#include <sys.h>
#include <tty.h>
#include <abierr.h>
#include "sv.h"

struct abi_proc {
    int pid;
    process_t *legacy;
    struct sv_group *sv;
};

typedef struct {
    uint8_t e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Eh;

typedef struct {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} Sh;

/* is this an executable written for ZenithOS itself? */
bool abi_is_native(const char *path)
{
    vnode_t *vn = vfs_open(path, false);
    if (!vn) return true;                                  /* let the old loader report the error */
    Eh eh;
    bool native = false;
    if (vfs_read(vn, 0, &eh, sizeof(eh)) == (long)sizeof(eh) && !memcmp(eh.e_ident, "\x7F" "ELF", 4) && eh.e_shoff &&
        eh.e_shentsize == sizeof(Sh) && eh.e_shnum && eh.e_shnum < 200 && eh.e_shstrndx < eh.e_shnum) {
        Sh *sh = kmalloc((size_t)eh.e_shnum * sizeof(Sh));
        if (sh && vfs_read(vn, eh.e_shoff, sh, (size_t)eh.e_shnum * sizeof(Sh)) == (long)((size_t)eh.e_shnum * sizeof(Sh))) {
            Sh *str = &sh[eh.e_shstrndx];
            if (str->sh_size && str->sh_size < 8192) {
                char *names = kmalloc(str->sh_size + 1);
                if (names && vfs_read(vn, str->sh_offset, names, str->sh_size) == (long)str->sh_size) {
                    names[str->sh_size] = 0;
                    for (int i = 0; i < eh.e_shnum; i++)
                        if (sh[i].sh_name < str->sh_size && !strcmp(names + sh[i].sh_name, ".zenith.abi")) native = true;
                }
                kfree(names);
            }
        }
        kfree(sh);
    }
    vfs_close(vn);
    return native;
}

/* the environment a program starts with, made from the session and the system settings */
static char **default_env(void)
{
    char **e = kzalloc(sizeof(char *) * 20);
    int n = 0;
    char b[512];
    const char *user = session.active ? session.user : "user";
    const char *home = session.active ? session.home : "/tmp";
    /* every installed package's bin folder is on the path */
    char path[400] = "/usr/local/bin:/usr/bin:/bin";
    struct vfs_dirent de;
    for (int i = 0; vfs_readdir("/opt", i, &de) == 0; i++) {
        char d[VFS_PATH_MAX];
        snprintf(d, sizeof(d), "/opt/%s/bin", de.name);
        if (de.type == VN_DIR && vfs_lookup(d) && strlen(path) + strlen(d) + 2 < sizeof(path)) { strlcat(path, ":", sizeof(path)); strlcat(path, d, sizeof(path)); }
    }
    snprintf(b, sizeof(b), "PATH=%s", path); e[n++] = strdup(b);
    snprintf(b, sizeof(b), "HOME=%s", home); e[n++] = strdup(b);
    snprintf(b, sizeof(b), "USER=%s", user); e[n++] = strdup(b);
    snprintf(b, sizeof(b), "LOGNAME=%s", user); e[n++] = strdup(b);
    e[n++] = strdup("SHELL=/bin/sh");
    e[n++] = strdup("TERM=xterm-256color");
    snprintf(b, sizeof(b), "HOSTNAME=%s", sys_hostname()); e[n++] = strdup(b);
    const char *lang = i18n_current();
    snprintf(b, sizeof(b), "LANG=%s_%s.UTF-8", lang, !strcmp(lang, "en") ? "US" : !strcmp(lang, "de") ? "DE" : !strcmp(lang, "fr") ? "FR" : !strcmp(lang, "es") ? "ES" : "US");
    e[n++] = strdup(b);
    snprintf(b, sizeof(b), "TZ=%s", tz_name()); e[n++] = strdup(b);
    snprintf(b, sizeof(b), "XDG_RUNTIME_DIR=/run/user/%d", session.active ? session.uid : 1000); e[n++] = strdup(b);
    snprintf(b, sizeof(b), "XDG_DATA_DIRS=/usr/share:/opt/share"); e[n++] = strdup(b);
    snprintf(b, sizeof(b), "XDG_CONFIG_HOME=%s/.config", home); e[n++] = strdup(b);
    e[n++] = strdup("LC_COLLATE=C");
    return e;
}

static void free_env(char **e)
{
    for (int i = 0; e && e[i]; i++) kfree(e[i]);
    kfree(e);
}

int abi_exec(const char *path, int argc, char **argv, struct tty *tty, const char *cwd, struct abi_proc *out)
{
    memset(out, 0, sizeof(*out));
    bool native = abi_is_native(path);
    klog("abi: %s is a %s program", path, native ? "ZenithOS" : "System V");
    if (native) {
        process_t *p = proc_spawn(path, argc, argv, tty, cwd);
        if (!p) return -ERR_NOEXEC;
        out->legacy = p;
        out->pid = p->pid;
        return 0;
    }
    char *av[34];
    int n = 0;
    for (; n < argc && n < 32; n++) av[n] = argv[n];
    av[n] = NULL;
    char **env = default_env();
    char run[VFS_PATH_MAX];
    strlcpy(run, path, sizeof(run));
    struct sv_group *g = sv_spawn(run, av, env, tty, cwd);
    free_env(env);
    if (!g) return -ERR_NOEXEC;
    out->sv = g;
    out->pid = g->tgid;
    return 0;
}

/* wait for the end; the result follows the old convention: the exit code, -9 killed, below -1000 a crash */
int abi_wait(struct abi_proc *p)
{
    if (p->legacy) return proc_wait(p->pid);
    int st = sv_wait_kernel(p->sv);
    sv_group_put(p->sv);
    p->sv = NULL;
    if (st & 0x7F) {
        int sig = st & 0x7F;
        switch (sig) {
        case SIGSEGV: return -1014;
        case SIGILL: return -1006;
        case SIGFPE: return -1000;
        case SIGBUS: return -1017;
        default: return -9;
        }
    }
    return (st >> 8) & 0xFF;
}

void proc_interrupt(int pid)
{
    process_t *p = proc_find(pid);
    if (p && p->sv) sv_send_signal(p->sv, NULL, SIGINT);
    else proc_kill(pid);
}
