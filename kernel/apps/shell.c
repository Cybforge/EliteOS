/*
 * zsh-like shell for ZenithOS ("zsh" = Zenith SHell): line editing,
 * history, tab completion, output redirection and ~50 built-in commands.
 */
#include <tty.h>
#include <vfs.h>
#include <sched.h>
#include <mm.h>
#include <cpu.h>
#include <dev.h>
#include <parallel.h>
#include <net.h>
#include <block.h>
#include <audio.h>
#include <crypto.h>
#include <sys.h>
#include <pkg.h>
#include <gpu.h>
#include <module.h>
#include <uefi.h>
struct abi_proc { int pid; process_t *legacy; struct sv_group *sv; };
int abi_exec(const char *path, int argc, char **argv, struct tty *tty, const char *cwd, struct abi_proc *out);
int abi_wait(struct abi_proc *p);

#define HIST 64
#define MAXARGS 32

struct shell {
    struct tty *tty;
    char cwd[VFS_PATH_MAX];
    char hist[HIST][256];
    int nhist;
    /* output redirection */
    char *rbuf;
    size_t rlen, rcap;
    int last_status;
    int pw;                          /* display width of the prompt, to redraw input that wraps over several rows */
    bool quit;
};

extern struct bootinfo *boot_info;

static void out(struct shell *sh, const char *s, size_t n)
{
    if (sh->rbuf) {
        if (sh->rlen + n + 1 > sh->rcap) {
            size_t cap = MAX(sh->rcap * 2, sh->rlen + n + 256);
            char *nb = krealloc(sh->rbuf, cap);
            if (!nb) return;
            sh->rbuf = nb;
            sh->rcap = cap;
        }
        memcpy(sh->rbuf + sh->rlen, s, n);
        sh->rlen += n;
        return;
    }
    tty_write(sh->tty, s, n);
}

static void pr(struct shell *sh, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void pr(struct shell *sh, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out(sh, buf, (size_t)MIN(n, (int)sizeof(buf) - 1));
}

#define C_RESET "\x1b[0m"
#define C_DIM   "\x1b[90m"
#define C_RED   "\x1b[91m"
#define C_GREEN "\x1b[92m"
#define C_YEL   "\x1b[93m"
#define C_BLUE  "\x1b[94m"
#define C_MAG   "\x1b[95m"
#define C_CYAN  "\x1b[96m"
#define C_BOLD  "\x1b[1m"

static void err(struct shell *sh, const char *cmd, const char *msg)
{
    pr(sh, C_RED "%s: %s" C_RESET "\n", cmd, msg);
}

static const char *errstr(int e)
{
    switch (e) {
    case E_NOENT: return "no such file or directory";
    case E_EXIST: return "already exists";
    case E_NOTDIR: return "not a directory";
    case E_ISDIR: return "is a directory";
    case E_NOTEMPTY: return "directory not empty (use -r)";
    case E_NOMEM: return "out of memory";
    case E_INVAL: return "invalid argument";
    default: return "error";
    }
}

static void resolve(struct shell *sh, const char *p, char *outp)
{
    char tmp[VFS_PATH_MAX];
    if (p[0] == '~') {
        snprintf(tmp, sizeof(tmp), "%s%s", session_home(), p + 1);
        p = tmp;
    }
    vfs_resolve(sh->cwd, p, outp, VFS_PATH_MAX);
}

static void human(uint64_t b, char *buf, size_t n)
{
    if (b < 1024) snprintf(buf, n, "%lu B", b);
    else if (b < 1024 * 1024) snprintf(buf, n, "%lu.%lu KiB", b / 1024, (b % 1024) * 10 / 1024);
    else if (b < 1024ul * 1024 * 1024) snprintf(buf, n, "%lu.%lu MiB", b >> 20, ((b >> 10) % 1024) * 10 / 1024);
    else snprintf(buf, n, "%lu.%lu GiB", b >> 30, ((b >> 20) % 1024) * 10 / 1024);
}

/* ------------------------------------------------------------------------
 * commands
 * ---------------------------------------------------------------------- */

typedef int (*cmd_fn)(struct shell *sh, int argc, char **argv);

struct cmd {
    const char *name;
    cmd_fn fn;
    const char *help;
};

static const struct cmd commands[];

static int c_help(struct shell *sh, int argc, char **argv);

static int c_echo(struct shell *sh, int argc, char **argv)
{
    bool nl = true;
    int i = 1;
    if (argc > 1 && !strcmp(argv[1], "-n")) { nl = false; i++; }
    for (; i < argc; i++) pr(sh, "%s%s", argv[i], i + 1 < argc ? " " : "");
    if (nl) out(sh, "\n", 1);
    return 0;
}

static int c_clear(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    pr(sh, "\x1b[2J\x1b[H");
    return 0;
}

static int c_pwd(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    pr(sh, "%s\n", sh->cwd);
    return 0;
}

static int c_cd(struct shell *sh, int argc, char **argv)
{
    char p[VFS_PATH_MAX];
    resolve(sh, argc > 1 ? argv[1] : "~", p);
    struct vfs_stat st;
    if (vfs_stat(p, &st)) { err(sh, "cd", errstr(E_NOENT)); return 1; }
    if (st.type != VN_DIR) { err(sh, "cd", errstr(E_NOTDIR)); return 1; }
    strlcpy(sh->cwd, p, sizeof(sh->cwd));
    return 0;
}

static int ls_one(struct shell *sh, const char *target, bool lng, bool all, bool header)
{
    char p[VFS_PATH_MAX];
    resolve(sh, target, p);
    struct vfs_stat st;
    if (vfs_stat(p, &st)) { err(sh, target, errstr(E_NOENT)); return 1; }
    if (st.type != VN_DIR) { pr(sh, "%s\n", vfs_basename(p)); return 0; }
    if (header) pr(sh, C_BOLD "%s:" C_RESET "\n", target);
    struct vfs_dirent *de = kmalloc(sizeof(*de) * 256);
    int n = vfs_list(p, de, 256);
    int col = 0, width = sh->tty->cols;
    int maxlen = 0;
    for (int i = 0; i < n; i++) maxlen = MAX(maxlen, (int)strlen(de[i].name));
    int cellw = MIN(maxlen + 2, 40);
    for (int i = 0; i < n; i++) {
        if (!all && de[i].name[0] == '.') continue;
        const char *color = de[i].type == VN_DIR ? C_BLUE C_BOLD : de[i].type == VN_DEV ? C_YEL : C_RESET;
        char path[VFS_PATH_MAX + 64];
        snprintf(path, sizeof(path), "%s/%s", p, de[i].name);
        vnode_t *vn = NULL;
        /* programs have no extension; peeking at other files would read them from disk */
        if (de[i].type == VN_FILE && !strchr(de[i].name, '.') && de[i].size >= 4 && (vn = vfs_open(path, false))) {
            char m[4] = { 0 };
            vfs_read(vn, 0, m, 4);
            vfs_close(vn);
            if (m[0] == 0x7F && m[1] == 'E' && m[2] == 'L' && m[3] == 'F') color = C_GREEN C_BOLD;
        }
        if (lng) {
            char sz[24], tm[24];
            struct datetime dt;
            epoch_to_datetime(de[i].mtime, &dt);
            snprintf(tm, sizeof(tm), "%04d-%02d-%02d %02d:%02d", dt.year, dt.month, dt.day, dt.hour, dt.minute);
            if (de[i].type == VN_DIR) strcpy(sz, "-");
            else human(de[i].size, sz, sizeof(sz));
            pr(sh, "%c  %10s  %s  %s%s" C_RESET "%s\n", de[i].type == VN_DIR ? 'd' : de[i].type == VN_DEV ? 'c' : '-',
               sz, tm, color, de[i].name, de[i].type == VN_DIR ? "/" : "");
        } else {
            if (col + cellw > width && col) { out(sh, "\n", 1); col = 0; }
            pr(sh, "%s%-*s" C_RESET, color, cellw, de[i].name);
            col += cellw;
        }
    }
    if (!lng && col) out(sh, "\n", 1);
    kfree(de);
    return 0;
}

static int c_ls(struct shell *sh, int argc, char **argv)
{
    bool lng = false, all = false;
    int targets = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strchr(argv[i], 'l')) lng = true;
            if (strchr(argv[i], 'a')) all = true;
        } else targets++;
    }
    if (!targets) return ls_one(sh, ".", lng, all, false);
    int r = 0, k = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        if (k++) out(sh, "\n", 1);
        r |= ls_one(sh, argv[i], lng, all, targets > 1);
    }
    return r;
}

static int c_cat(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) { err(sh, "cat", "usage: cat <file>..."); return 1; }
    for (int i = 1; i < argc; i++) {
        char p[VFS_PATH_MAX];
        resolve(sh, argv[i], p);
        size_t n;
        char *d = vfs_read_file(p, &n);
        if (!d) { err(sh, argv[i], errstr(E_NOENT)); continue; }
        out(sh, d, n);
        if (n && d[n - 1] != '\n' && !sh->rbuf) out(sh, "\n", 1);
        kfree(d);
    }
    return 0;
}

static int c_mkdir(struct shell *sh, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        char p[VFS_PATH_MAX];
        resolve(sh, argv[i], p);
        int r = vfs_mkdir(p);
        if (r) err(sh, "mkdir", errstr(r));
    }
    return 0;
}

static int c_rm(struct shell *sh, int argc, char **argv)
{
    bool rec = false;
    for (int i = 1; i < argc; i++)
        if (argv[i][0] == '-' && strchr(argv[i], 'r')) rec = true;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        char p[VFS_PATH_MAX];
        resolve(sh, argv[i], p);
        int r = vfs_unlink(p, rec);
        if (r) err(sh, argv[i], errstr(r));
    }
    return 0;
}

static int c_touch(struct shell *sh, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        char p[VFS_PATH_MAX];
        resolve(sh, argv[i], p);
        vnode_t *vn = vfs_open(p, true);
        if (!vn) err(sh, "touch", errstr(E_NOENT));
        else { vn->mtime = rtc_epoch(); vfs_close(vn); }
    }
    return 0;
}

static int c_cp(struct shell *sh, int argc, char **argv)
{
    if (argc < 3) { err(sh, "cp", "usage: cp <from> <to>"); return 1; }
    char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
    resolve(sh, argv[argc - 2], a);
    resolve(sh, argv[argc - 1], b);
    struct vfs_stat st;
    if (!vfs_stat(b, &st) && st.type == VN_DIR) {
        strlcat(b, "/", sizeof(b));
        strlcat(b, vfs_basename(a), sizeof(b));
    }
    int r = vfs_copy(a, b);
    if (r) err(sh, "cp", errstr(r));
    return r ? 1 : 0;
}

static int c_mv(struct shell *sh, int argc, char **argv)
{
    if (argc < 3) { err(sh, "mv", "usage: mv <from> <to>"); return 1; }
    char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
    resolve(sh, argv[1], a);
    resolve(sh, argv[2], b);
    struct vfs_stat st;
    if (!vfs_stat(b, &st) && st.type == VN_DIR) {
        strlcat(b, "/", sizeof(b));
        strlcat(b, vfs_basename(a), sizeof(b));
    }
    int r = vfs_rename(a, b);
    if (r) err(sh, "mv", errstr(r));
    return r ? 1 : 0;
}

static int c_write(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) { err(sh, "write", "usage: write <file> <text...>"); return 1; }
    char p[VFS_PATH_MAX];
    resolve(sh, argv[1], p);
    char buf[1024] = "";
    for (int i = 2; i < argc; i++) {
        strlcat(buf, argv[i], sizeof(buf));
        strlcat(buf, i + 1 < argc ? " " : "\n", sizeof(buf));
    }
    int r = vfs_write_file(p, buf, strlen(buf));
    if (r) err(sh, "write", errstr(r));
    return 0;
}

static int c_wc(struct shell *sh, int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        char p[VFS_PATH_MAX];
        resolve(sh, argv[i], p);
        size_t n;
        char *d = vfs_read_file(p, &n);
        if (!d) { err(sh, argv[i], errstr(E_NOENT)); continue; }
        size_t lines = 0, words = 0;
        bool inw = false;
        for (size_t k = 0; k < n; k++) {
            if (d[k] == '\n') lines++;
            if (isspace(d[k])) inw = false;
            else if (!inw) { inw = true; words++; }
        }
        pr(sh, "%6lu %6lu %6lu %s\n", lines, words, n, argv[i]);
        kfree(d);
    }
    return 0;
}

static int c_grep(struct shell *sh, int argc, char **argv)
{
    if (argc < 3) { err(sh, "grep", "usage: grep <pattern> <file>..."); return 1; }
    int hits = 0;
    for (int i = 2; i < argc; i++) {
        char p[VFS_PATH_MAX];
        resolve(sh, argv[i], p);
        char *d = vfs_read_file(p, NULL);
        if (!d) { err(sh, argv[i], errstr(E_NOENT)); continue; }
        char *line = d;
        int ln = 1;
        while (*line) {
            char *e = strchr(line, '\n');
            if (e) *e = 0;
            char *m = strstr_ci(line, argv[1]);
            if (m) {
                hits++;
                if (argc > 3) pr(sh, C_MAG "%s" C_RESET ":", argv[i]);
                pr(sh, C_GREEN "%d" C_RESET ":", ln);
                out(sh, line, (size_t)(m - line));
                pr(sh, C_RED C_BOLD);
                out(sh, m, strlen(argv[1]));
                pr(sh, C_RESET "%s\n", m + strlen(argv[1]));
            }
            if (!e) break;
            line = e + 1;
            ln++;
        }
        kfree(d);
    }
    return hits ? 0 : 1;
}

static int c_head(struct shell *sh, int argc, char **argv)
{
    int lines = 10;
    const char *f = NULL;
    bool tail = !strcmp(argv[0], "tail");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) lines = atoi(argv[++i]);
        else f = argv[i];
    }
    if (!f) { err(sh, argv[0], "missing file"); return 1; }
    char p[VFS_PATH_MAX];
    resolve(sh, f, p);
    size_t n;
    char *d = vfs_read_file(p, &n);
    if (!d) { err(sh, f, errstr(E_NOENT)); return 1; }
    size_t start = 0, end = n;
    if (!tail) {
        int c = 0;
        for (size_t i = 0; i < n; i++) if (d[i] == '\n' && ++c == lines) { end = i + 1; break; }
    } else {
        int c = 0;
        for (size_t i = n; i > 0; i--) if (d[i - 1] == '\n' && i != n && ++c == lines) { start = i; break; }
    }
    out(sh, d + start, end - start);
    kfree(d);
    return 0;
}

static void tree_rec(struct shell *sh, const char *path, char *prefix, int depth)
{
    if (depth > 8) return;
    struct vfs_dirent *de = kmalloc(sizeof(*de) * 128);
    int n = vfs_list(path, de, 128);
    for (int i = 0; i < n; i++) {
        bool last = i == n - 1;
        pr(sh, C_DIM "%s%s" C_RESET "%s%s" C_RESET "\n", prefix, last ? "└── " : "├── ",
           de[i].type == VN_DIR ? C_BLUE C_BOLD : "", de[i].name);
        if (de[i].type == VN_DIR) {
            char child[VFS_PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", strcmp(path, "/") ? path : "", de[i].name);
            size_t pl = strlen(prefix);
            strcat(prefix, last ? "    " : "│   ");
            tree_rec(sh, child, prefix, depth + 1);
            prefix[pl] = 0;
        }
    }
    kfree(de);
}

static int c_tree(struct shell *sh, int argc, char **argv)
{
    char p[VFS_PATH_MAX];
    resolve(sh, argc > 1 ? argv[1] : ".", p);
    pr(sh, C_BLUE C_BOLD "%s" C_RESET "\n", p);
    char prefix[256] = "";
    tree_rec(sh, p, prefix, 0);
    return 0;
}

static int c_hexdump(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) { err(sh, "hexdump", "usage: hexdump <file>"); return 1; }
    char p[VFS_PATH_MAX];
    resolve(sh, argv[1], p);
    size_t n;
    uint8_t *d = (uint8_t *)vfs_read_file(p, &n);
    if (!d) { err(sh, argv[1], errstr(E_NOENT)); return 1; }
    size_t lim = MIN(n, (size_t)512);
    for (size_t off = 0; off < lim; off += 16) {
        pr(sh, C_DIM "%08lx  " C_RESET, off);
        for (size_t i = 0; i < 16; i++) {
            if (off + i < lim) pr(sh, "%02x ", d[off + i]);
            else pr(sh, "   ");
            if (i == 7) pr(sh, " ");
        }
        pr(sh, C_DIM " |" C_RESET);
        for (size_t i = 0; i < 16 && off + i < lim; i++) {
            char c = (char)d[off + i];
            pr(sh, "%c", isprint(c) ? c : '.');
        }
        pr(sh, C_DIM "|" C_RESET "\n");
    }
    if (n > lim) pr(sh, C_DIM "... %lu more bytes" C_RESET "\n", n - lim);
    kfree(d);
    return 0;
}

static int c_date(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    static const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *mon[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                 "September", "October", "November", "December" };
    struct datetime dt;
    rtc_now(&dt);
    pr(sh, "%s, %d %s %d  %02d:%02d:%02d\n", days[dt.weekday], dt.day, mon[dt.month - 1], dt.year, dt.hour,
       dt.minute, dt.second);
    return 0;
}

static int c_cal(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    static const char *mon[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                 "September", "October", "November", "December" };
    struct datetime dt;
    rtc_now(&dt);
    int y = dt.year, m = dt.month;
    static const int mdays[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int days = mdays[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0));
    /* weekday of the 1st */
    int64_t today = rtc_epoch() / 86400;
    int wd_first = (int)((today - (dt.day - 1) + 4) % 7);   /* 0 = Sunday */
    wd_first = (wd_first + 6) % 7;                          /* Monday first */
    char title[32];
    snprintf(title, sizeof(title), "%s %d", mon[m - 1], y);
    int pad = (20 - (int)strlen(title)) / 2;
    pr(sh, "%*s" C_BOLD "%s" C_RESET "\n", pad, "", title);
    pr(sh, C_DIM "Mo Tu We Th Fr " C_RESET C_MAG "Sa Su" C_RESET "\n");
    for (int i = 0; i < wd_first; i++) pr(sh, "   ");
    for (int d = 1; d <= days; d++) {
        int col = (wd_first + d - 1) % 7;
        if (d == dt.day) pr(sh, "\x1b[7m\x1b[45m%2d" C_RESET " ", d);
        else if (col >= 5) pr(sh, C_MAG "%2d" C_RESET " ", d);
        else pr(sh, "%2d ", d);
        if (col == 6) pr(sh, "\n");
    }
    pr(sh, "\n");
    return 0;
}

static int c_uptime(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    uint64_t s = uptime_ms() / 1000;
    int load = 0;
    for (int i = 0; i < ncpus; i++) load += (int)cpus[i].load;
    pr(sh, "up %lu:%02lu:%02lu, %d threads, load %d%%\n", s / 3600, (s / 60) % 60, s % 60, sched_thread_count(),
       load / MAX(1, ncpus));
    return 0;
}

static int c_free(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    uint64_t total = pmm_total_pages() * PAGE_SIZE, freeb = pmm_free_pages() * PAGE_SIZE;
    char a[24], b[24], c[24], d[24];
    human(total, a, sizeof(a));
    human(total - freeb, b, sizeof(b));
    human(freeb, c, sizeof(c));
    human(heap_used(), d, sizeof(d));
    pr(sh, C_BOLD "          total        used        free   kernel heap" C_RESET "\n");
    pr(sh, "Mem:  %10s  %10s  %10s  %12s\n", a, b, c, d);
    int pct = (int)((total - freeb) * 40 / MAX(total, 1));
    pr(sh, "      [" C_GREEN);
    for (int i = 0; i < 40; i++) pr(sh, i < pct ? "█" : C_DIM "·" C_GREEN);
    pr(sh, C_RESET "]\n");
    return 0;
}

static int c_df(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    char a[24], b[24], c[24];
    human(vfs_total_bytes(), a, sizeof(a));
    pr(sh, C_BOLD "Filesystem  Type   Size        Used        Avail       Mounted on" C_RESET "\n");
    pr(sh, "ramfs       ramfs  -           %-11s -           /\n", a);
    for (struct fs_mount *m = vfs_mounts(); m; m = m->next) {
        uint64_t total, free;
        m->ops->statfs(m, &total, &free);
        human(total, a, sizeof(a));
        human(total - free, b, sizeof(b));
        human(free, c, sizeof(c));
        pr(sh, "%-11s %-6s %-11s %-11s %-11s %s\n", m->dev, m->fstype, a, b, c, m->path);
    }
    return 0;
}

static const char *mount_point_of(struct blockdev *d)
{
    for (struct fs_mount *m = vfs_mounts(); m; m = m->next)
        if (!strcmp(m->dev, d->name)) return m->path;
    return "";
}

static int c_lsblk(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    if (!blk_count()) { pr(sh, "no disks found (ZenithOS supports SATA/AHCI disks)\n"); return 0; }
    pr(sh, C_BOLD "NAME      SIZE       FS      LABEL        MOUNT     MODEL" C_RESET "\n");
    for (int i = 0; i < blk_count(); i++) {
        struct blockdev *d = blk_get(i);
        char sz[24], label[16] = "";
        blk_format_size(d->sectors * SECTOR_SIZE, sz, sizeof(sz));
        bool fat = fat_probe(d, label, sizeof(label));
        pr(sh, "%s%-9s%s %-10s %-7s %-12s %-9s %s\n", d->parent ? "  " : C_BOLD, d->name, C_RESET, sz,
           fat ? "fat32" : "", label, mount_point_of(d), d->parent ? "" : d->model);
    }
    return 0;
}

static int c_mount(struct shell *sh, int argc, char **argv)
{
    if (argc < 3) {
        if (!vfs_mounts()) pr(sh, "no disk file systems mounted\n");
        for (struct fs_mount *m = vfs_mounts(); m; m = m->next)
            pr(sh, "%s on %s type %s (%s)\n", m->dev, m->path, m->fstype, m->label[0] ? m->label : "no label");
        return 0;
    }
    struct blockdev *d = blk_find(argv[1]);
    if (!d) { err(sh, "mount", "no such device"); return 1; }
    if (d->busy) { err(sh, "mount", "device is busy"); return 1; }
    char p[VFS_PATH_MAX];
    resolve(sh, argv[2], p);
    int r = fat_mount(d, p);
    if (r) { err(sh, "mount", "not a FAT32 file system, or the mount point is in use"); return 1; }
    return 0;
}

static int c_sync(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    int r = vfs_sync();
    if (r) { err(sh, "sync", "write error"); return 1; }
    return 0;
}

static int c_play(struct shell *sh, int argc, char **argv)
{
    if (!audio_available()) { err(sh, "play", "no sound card (ZenithOS supports Intel HD Audio)"); return 1; }
    if (argc < 2) { err(sh, "play", "usage: play <file.wav> | play startup|notify|error|success"); return 1; }
    static const char *names[] = { "startup", "notify", "error", "click", "success", "shutdown", "pop" };
    for (unsigned i = 0; i < ARRAY_SIZE(names); i++)
        if (!strcmp(argv[1], names[i])) { audio_sound((int)i); return 0; }
    char p[VFS_PATH_MAX];
    resolve(sh, argv[1], p);
    size_t n;
    char *d = vfs_read_file(p, &n);
    if (!d) { err(sh, "play", errstr(E_NOENT)); return 1; }
    int r = audio_play_wav(d, n);
    kfree(d);
    if (r) { err(sh, "play", "not a PCM WAV file (8/16-bit, mono/stereo)"); return 1; }
    return 0;
}

static int c_beep(struct shell *sh, int argc, char **argv)
{
    if (!audio_available()) { err(sh, "beep", "no sound card"); return 1; }
    float f = argc > 1 ? (float)strtol(argv[1], NULL, 10) : 880.0f;
    int ms = argc > 2 ? (int)strtol(argv[2], NULL, 10) : 200;
    audio_note_on(CLAMP(f, 20.0f, 20000.0f), WAVE_SINE, 0.6f, CLAMP(ms, 1, 10000));
    return 0;
}

static int c_volume(struct shell *sh, int argc, char **argv)
{
    if (argc > 1) {
        if (!strcmp(argv[1], "mute")) audio_set_muted(true);
        else if (!strcmp(argv[1], "unmute")) audio_set_muted(false);
        else audio_set_volume((int)strtol(argv[1], NULL, 10));
    }
    pr(sh, "volume %d%%%s  ·  %s\n", audio_volume(), audio_muted() ? " (muted)" : "",
       audio_available() ? hda_name() : "no sound card");
    return 0;
}

static int c_sha256sum(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) { err(sh, "sha256sum", "usage: sha256sum <file>..."); return 1; }
    for (int i = 1; i < argc; i++) {
        char p[VFS_PATH_MAX];
        resolve(sh, argv[i], p);
        size_t n;
        char *d = vfs_read_file(p, &n);
        if (!d) { err(sh, argv[i], errstr(E_NOENT)); continue; }
        uint8_t h[32];
        sha256(d, n, h);
        kfree(d);
        char hex[65];
        for (int k = 0; k < 32; k++) snprintf(hex + 2 * k, 3, "%02x", h[k]);
        pr(sh, "%s  %s\n", hex, argv[i]);
    }
    return 0;
}

static int c_umount(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) { err(sh, "umount", "usage: umount <mount point>"); return 1; }
    char p[VFS_PATH_MAX];
    resolve(sh, argv[1], p);
    if (vfs_umount(p)) { err(sh, "umount", "not a mount point (or write error)"); return 1; }
    return 0;
}

static int c_install(struct shell *sh, int argc, char **argv)
{
    struct install_opts o;
    install_defaults(&o);
    const char *dev = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--erase")) o.erase = true;
        else if (!strcmp(a, "--no-gpu-driver")) o.gpu_driver = false;
        else if (!strcmp(a, "--autologin")) o.autologin = true;
        else if (!strcmp(a, "--user")) { strlcpy(o.user, val, sizeof(o.user)); i++; }
        else if (!strcmp(a, "--name")) { strlcpy(o.fullname, val, sizeof(o.fullname)); i++; }
        else if (!strcmp(a, "--password")) { strlcpy(o.password, val, sizeof(o.password)); i++; }
        else if (!strcmp(a, "--hostname")) { strlcpy(o.hostname, val, sizeof(o.hostname)); i++; }
        else if (!strcmp(a, "--language")) { strlcpy(o.language, val, sizeof(o.language)); i++; }
        else if (!strcmp(a, "--keyboard")) { strlcpy(o.keyboard, val, sizeof(o.keyboard)); i++; }
        else if (!strcmp(a, "--timezone")) { strlcpy(o.timezone, val, sizeof(o.timezone)); i++; }
        else dev = a;
    }
    if (!dev || !o.user[0] || !o.password[0]) {
        err(sh, "install", "usage: install <disk> --user <name> --password <pw> [--erase] [--name <full name>] [--hostname <h>]\n"
                           "       [--language de] [--keyboard de] [--timezone Europe/Berlin] [--autologin] [--no-gpu-driver]\n"
                           "       (see lsblk; without --erase the FAT32 volume is kept; the graphical installer asks for all of this)");
        return 1;
    }
    struct blockdev *d = blk_find(dev);
    if (!d) { err(sh, "install", "no such disk"); return 1; }
    o.disk = d;
    struct install_progress p = { 0 };
    pr(sh, "installing ZenithOS on %s%s...\n", d->name, o.erase ? " (erasing it)" : "");
    int r = zenith_install(&o, &p);
    pr(sh, "%s%s" C_RESET "\n", r ? C_RED : C_GREEN, p.msg);
    if (p.note[0]) pr(sh, "%s\n", p.note);
    if (!r) pr(sh, "firmware boot entry: %s\n", p.boot_entry ? "created and first in the boot order" : "not created");
    return r ? 1 : 0;
}

static int c_mkfs(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) { err(sh, "mkfs", "usage: mkfs <device> [label]   (see lsblk)"); return 1; }
    struct blockdev *d = blk_find(argv[1]);
    if (!d) { err(sh, "mkfs", "no such device"); return 1; }
    if (d->busy) { err(sh, "mkfs", "device is mounted"); return 1; }
    for (int i = 0; i < blk_count(); i++)
        if (blk_get(i)->parent == d && blk_get(i)->busy) { err(sh, "mkfs", "a partition of this disk is mounted"); return 1; }
    pr(sh, "formatting %s as FAT32...\n", d->name);
    int r = fat_mkfs(d, argc > 2 ? argv[2] : "ZENITH");
    if (r) { err(sh, "mkfs", "failed (the device must be at least 33 MiB)"); return 1; }
    pr(sh, "done. mount it with: mount %s /disk\n", d->name);
    return 0;
}

static const char *state_name(int s)
{
    switch (s) {
    case T_READY: return "ready";
    case T_RUNNING: return C_GREEN "run" C_RESET "  ";
    case T_BLOCKED: return "sleep";
    default: return "dead";
    }
}

static int c_ps(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    struct thread_snapshot *s = kmalloc(sizeof(*s) * 128);
    int n = sched_snapshot(s, 128);
    pr(sh, C_BOLD "  TID   PID  CPU%%  STATE   CORE  TIME      NAME" C_RESET "\n");
    for (int i = n - 1; i >= 0; i--) {
        pr(sh, "%5d %5d %3u.%u  %-5s   %4d  %6lu.%01lu  %s%s" C_RESET "\n", s[i].tid, s[i].pid, s[i].cpu_pct / 10,
           s[i].cpu_pct % 10, state_name(s[i].state), s[i].cpu, s[i].ticks / 1000, (s[i].ticks / 100) % 10,
           s[i].user ? C_CYAN : "", s[i].name);
    }
    kfree(s);
    return 0;
}

static int c_kill(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) { err(sh, "kill", "usage: kill <pid>"); return 1; }
    int pid = atoi(argv[1]);
    if (!proc_kill(pid)) { err(sh, "kill", "no such process (only user processes can be killed)"); return 1; }
    return 0;
}

static int c_uname(struct shell *sh, int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "-a"))
        pr(sh, "ZenithOS zenith %s \"%s\" SMP x86_64 %s\n", ZENITH_VERSION, ZENITH_CODENAME, __DATE__);
    else
        pr(sh, "ZenithOS\n");
    return 0;
}

static int c_lspci(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *d = pci_get(i);
        const char *dn = pci_device_name(d->vendor, d->device);
        pr(sh, C_DIM "%02x:%02x.%x" C_RESET " %s: " C_BOLD "%s" C_RESET " %s " C_DIM "[%04x:%04x]" C_RESET "\n",
           d->bus, d->dev, d->fn, pci_class_name(d->cls, d->subcls), pci_vendor_name(d->vendor), dn ? dn : "",
           d->vendor, d->device);
    }
    return 0;
}

static int c_lscpu(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    pr(sh, "Model name:    %s\n", cpu_info.brand);
    pr(sh, "Vendor:        %s\n", cpu_info.vendor);
    pr(sh, "Family/model:  %u / %u, stepping %u\n", cpu_info.family, cpu_info.model, cpu_info.stepping);
    pr(sh, "CPUs online:   %d\n", ncpus);
    pr(sh, "TSC:           %lu MHz\n", cpu_info.tsc_hz / 1000000);
    pr(sh, "Flags:         sse sse2%s%s%s%s%s%s%s%s%s\n", cpu_info.sse3 ? " sse3" : "", cpu_info.ssse3 ? " ssse3" : "",
       cpu_info.sse41 ? " sse4.1" : "", cpu_info.sse42 ? " sse4.2" : "", cpu_info.avx ? " avx" : "",
       cpu_info.avx2 ? " avx2" : "", cpu_info.aes ? " aes" : "", cpu_info.nx ? " nx" : "",
       cpu_info.hypervisor ? " hypervisor" : "");
    for (int i = 0; i < ncpus; i++) pr(sh, "  cpu%-2d  APIC %-3u  load %3u%%\n", i, cpus[i].lapic_id, cpus[i].load);
    return 0;
}

static int c_dmesg(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    char *buf = kmalloc(65536);
    size_t n = klog_read(buf, 65536);
    out(sh, buf, n);
    kfree(buf);
    return 0;
}

static int c_neofetch(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    static const char *logo[] = {
        "   ▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄   ",
        "  ████████████████▀   ",
        "            ▄████▀    ",
        "          ▄████▀      ",
        "        ▄████▀        ",
        "      ▄████▀          ",
        "    ▄████▀            ",
        "  ▄████████████████   ",
        "  ▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀   ",
    };
    static const int grad[][3] = { { 150, 110, 255 }, { 140, 115, 255 }, { 125, 125, 255 }, { 110, 140, 255 },
                                   { 90, 160, 255 }, { 70, 180, 255 }, { 50, 200, 255 }, { 30, 215, 255 },
                                   { 20, 225, 255 } };
    char info[12][96];
    int ni = 0;
    uint64_t up = uptime_ms() / 1000;
    uint64_t total = pmm_total_pages() * PAGE_SIZE, used = total - pmm_free_pages() * PAGE_SIZE;
    snprintf(info[ni++], 96, "\x1b[1;95m%s\x1b[0m@\x1b[1;95m%s\x1b[0m", session.user[0] ? session.user : "nobody", sys_hostname());
    snprintf(info[ni++], 96, "\x1b[90m-----------\x1b[0m");
    snprintf(info[ni++], 96, "\x1b[95mOS\x1b[0m: ZenithOS %s \"%s\" x86_64", ZENITH_VERSION, ZENITH_CODENAME);
    snprintf(info[ni++], 96, "\x1b[95mKernel\x1b[0m: zenith %s SMP", ZENITH_VERSION);
    snprintf(info[ni++], 96, "\x1b[95mUptime\x1b[0m: %lu h %lu min %lu s", up / 3600, (up / 60) % 60, up % 60);
    snprintf(info[ni++], 96, "\x1b[95mShell\x1b[0m: zsh (Zenith SHell) 1.0");
    snprintf(info[ni++], 96, "\x1b[95mResolution\x1b[0m: %dx%d", wm_screen_w(), wm_screen_h());
    snprintf(info[ni++], 96, "\x1b[95mWM\x1b[0m: Zenith Compositor (%u fps)", wm_fps());
    snprintf(info[ni++], 96, "\x1b[95mCPU\x1b[0m: %.30s (%d) @ %lu MHz", cpu_info.brand, ncpus, cpu_info.tsc_hz / 1000000);
    snprintf(info[ni++], 96, "\x1b[95mMemory\x1b[0m: %lu MiB / %lu MiB", used >> 20, total >> 20);
    snprintf(info[ni++], 96, " ");
    char *p = info[ni++];
    p[0] = 0;
    for (int i = 0; i < 8; i++) {
        char b[24];
        snprintf(b, sizeof(b), "\x1b[4%dm   ", i);
        strlcat(p, b, 96);
    }
    strlcat(p, "\x1b[0m", 96);
    pr(sh, "\n");
    for (int i = 0; i < MAX(ni, (int)ARRAY_SIZE(logo)); i++) {
        if (i < (int)ARRAY_SIZE(logo))
            pr(sh, "\x1b[38;2;%d;%d;%dm%s\x1b[0m", grad[i][0], grad[i][1], grad[i][2], logo[i]);
        else
            pr(sh, "%22s", "");
        pr(sh, "  %s\n", i < ni ? info[i] : "");
    }
    pr(sh, "\n");
    return 0;
}

static int c_calc(struct shell *sh, int argc, char **argv)
{
    char e[256] = "";
    for (int i = 1; i < argc; i++) { strlcat(e, argv[i], sizeof(e)); strlcat(e, " ", sizeof(e)); }
    double v;
    const char *er;
    if (!expr_eval(e, &v, &er)) { err(sh, "calc", er); return 1; }
    char b[64];
    expr_format(v, b, sizeof(b));
    pr(sh, "%s\n", b);
    return 0;
}

/* SMP benchmark: count primes, first on one core, then on all cores */
struct bench_ctx { int chunk; int n; volatile int count; };

static int is_prime(int n)
{
    if (n < 2) return 0;
    if (n % 2 == 0) return n == 2;
    for (int d = 3; d * d <= n; d += 2)
        if (n % d == 0) return 0;
    return 1;
}

static void bench_item(int i, void *ctx)
{
    struct bench_ctx *b = ctx;
    int from = i * b->chunk, to = MIN(b->n, from + b->chunk), c = 0;
    for (int k = from; k < to; k++) c += is_prime(k);
    __atomic_add_fetch(&b->count, c, __ATOMIC_RELAXED);
}

static int c_bench(struct shell *sh, int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 1500000;
    pr(sh, "Counting primes below %d...\n", n);
    uint64_t t0 = uptime_ms();
    int c1 = 0;
    for (int k = 0; k < n; k++) c1 += is_prime(k);
    uint64_t t1 = uptime_ms();
    struct bench_ctx b = { 5000, n, 0 };
    parallel_for((n + b.chunk - 1) / b.chunk, bench_item, &b);
    uint64_t t2 = uptime_ms();
    uint64_t single = MAX(t1 - t0, 1ul), multi = MAX(t2 - t1, 1ul);
    pr(sh, "  1 core:   " C_BOLD "%6lu ms" C_RESET "  (%d primes)\n", single, c1);
    pr(sh, "  %d cores: " C_BOLD C_GREEN "%6lu ms" C_RESET "  (%d primes)\n", parallel_workers(), multi, b.count);
    pr(sh, "  speedup:  " C_BOLD C_MAG "%lu.%02lux" C_RESET "\n", single / multi, (single * 100 / multi) % 100);
    return 0;
}

static int c_fractal(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    int W_ = MIN(sh->tty->cols - 1, 100), H_ = MIN(sh->tty->rows - 2, 34);
    for (int y = 0; y < H_; y++) {
        for (int x = 0; x < W_; x++) {
            double cr = -2.2 + 3.0 * x / W_, ci = -1.2 + 2.4 * y / H_;
            double zr = 0, zi = 0;
            int it = 0;
            while (it < 64 && zr * zr + zi * zi < 4) {
                double t = zr * zr - zi * zi + cr;
                zi = 2 * zr * zi + ci;
                zr = t;
                it++;
            }
            if (it == 64) pr(sh, " ");
            else {
                color_t c = color_hsv(260.0f - (float)it * 7.0f, 0.75f, 1.0f);
                pr(sh, "\x1b[38;2;%u;%u;%um%c", C_R(c), C_G(c), C_B(c), " .:-=+*#%@"[MIN(it / 3, 9)]);
            }
        }
        pr(sh, C_RESET "\n");
    }
    return 0;
}

static int c_colors(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    for (int i = 0; i < 16; i++) {
        pr(sh, "\x1b[%dm %2d " C_RESET, i < 8 ? 40 + i : 100 + i - 8, i);
        if (i == 7) pr(sh, "\n");
    }
    pr(sh, "\n");
    for (int i = 0; i < 72; i++) {
        color_t c = color_hsv((float)i * 5.0f, 0.8f, 1.0f);
        pr(sh, "\x1b[48;2;%u;%u;%um ", C_R(c), C_G(c), C_B(c));
    }
    pr(sh, C_RESET "\n");
    return 0;
}

static int c_matrix(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    int W_ = sh->tty->cols, H_ = sh->tty->rows;
    int *drop = kzalloc(sizeof(int) * (size_t)W_);
    for (int x = 0; x < W_; x++) drop[x] = -(int)(krand() % (uint32_t)H_);
    pr(sh, "\x1b[2J\x1b[?25l");
    struct gui_event ev;
    while (!tty_getkey(sh->tty, &ev, 50)) {
        if (sh->tty->closed) break;
        for (int x = 0; x < W_; x += 2) {
            int y = drop[x];
            if (y >= 0 && y < H_) pr(sh, "\x1b[%d;%dH\x1b[97m%c", y + 1, x + 1, 33 + (int)(krand() % 90));
            if (y - 1 >= 0 && y - 1 < H_) pr(sh, "\x1b[%d;%dH\x1b[92m%c", y, x + 1, 33 + (int)(krand() % 90));
            if (y - 12 >= 0 && y - 12 < H_) pr(sh, "\x1b[%d;%dH ", y - 11, x + 1);
            drop[x]++;
            if (drop[x] - 12 > H_) drop[x] = -(int)(krand() % 10);
        }
    }
    kfree(drop);
    pr(sh, C_RESET "\x1b[2J\x1b[H\x1b[?25h");
    return 0;
}

static int c_sleep(struct shell *sh, int argc, char **argv)
{
    UNUSED(sh);
    sched_sleep(argc > 1 ? (uint64_t)atoi(argv[1]) * 1000 : 1000);
    return 0;
}

static int c_open(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) { err(sh, "open", "usage: open <file|app|url>"); return 1; }
    if (app_find(argv[1])) { app_launch(argv[1]); return 0; }
    if (strstr(argv[1], "://")) { open_path(argv[1]); return 0; }
    char p[VFS_PATH_MAX];
    resolve(sh, argv[1], p);
    struct vfs_stat st;
    if (vfs_stat(p, &st)) { err(sh, "open", "no such file or app"); return 1; }
    open_path(p);
    return 0;
}

static int c_edit(struct shell *sh, int argc, char **argv)
{
    char p[VFS_PATH_MAX];
    resolve(sh, argc > 1 ? argv[1] : "untitled.txt", p);
    vnode_t *v = vfs_open(p, true);
    if (v) vfs_close(v);
    open_path(p);
    return 0;
}

static int c_apps(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    for (int i = 0; i < app_count; i++)
        pr(sh, "  " C_CYAN "%-10s" C_RESET " %-16s " C_DIM "%s" C_RESET "\n", app_table[i].id, app_table[i].name,
           app_table[i].desc);
    return 0;
}

static int c_layout(struct shell *sh, int argc, char **argv)
{
    if (argc > 1) {
        if (!keymap_set(argv[1])) {
            err(sh, "layout", "no such layout (run 'layout' to list them)");
            return 1;
        }
        conf_set(&sysconf, "keyboard", keymap_current());
        sys_save_conf();
        wm_invalidate_all();
    }
    struct kbd_info k[16];
    int n = keymap_layouts(k, 16);
    for (int i = 0; i < n; i++)
        pr(sh, "  %s %-8s %s\n", !strcmp(k[i].id, keymap_current()) ? C_GREEN "*" C_RESET : " ", k[i].id, k[i].name);
    return 0;
}

static int c_wallpaper(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        for (int i = 0; i < wallpaper_count; i++)
            pr(sh, "  %d  %s%s\n", i, wallpaper_names[i], i == wallpaper_style ? C_GREEN "  (current)" C_RESET : "");
        return 0;
    }
    wm_set_wallpaper(atoi(argv[1]));
    return 0;
}

static int c_screenshot(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    char p[160];
    if (wm_save_screenshot(p, sizeof(p))) pr(sh, "saved %s\n", p);
    else err(sh, "screenshot", "failed");
    return 0;
}

static int c_reboot(struct shell *sh, int argc, char **argv)
{
    UNUSED(sh); UNUSED(argc);
    app_launch(!strcmp(argv[0], "reboot") ? "@reboot" : "@poweroff");
    return 0;
}

static int c_exit(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    sh->quit = true;
    return 0;
}

static int c_history(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    for (int i = 0; i < sh->nhist; i++) pr(sh, "%4d  %s\n", i + 1, sh->hist[i]);
    return 0;
}

static int c_whoami(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    pr(sh, "user\n");
    return 0;
}

static int c_hostname(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    pr(sh, "%s\n", sys_hostname());
    return 0;
}

/* ---- graphics, packages, modules, firmware boot entries ---- */

static int c_gpu(struct shell *sh, int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "policy")) {
        if (strcmp(argv[2], "auto") && strcmp(argv[2], "gop") && strcmp(argv[2], "driver")) { err(sh, "gpu", "policy is auto, gop or driver"); return 1; }
        gpu_set_policy(argv[2]);
        pr(sh, "graphics policy: %s (takes effect at the next start)\n", argv[2]);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "enable")) { gpu_reenable(); pr(sh, "graphics drivers are enabled again (next start)\n"); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "modes")) {
        struct gpu_timing m[EDID_MAX_MODES];
        int n = gpu_modes(m, EDID_MAX_MODES);
        if (!n) { pr(sh, "no graphics driver is active, so the display mode cannot be changed\n"); return 0; }
        for (int i = 0; i < n; i++)
            pr(sh, "%4dx%-4d  %3u.%03u Hz   pixel clock %u kHz%s\n", m[i].hactive, m[i].vactive, m[i].refresh_mhz / 1000,
               m[i].refresh_mhz % 1000, m[i].pclk_khz, m[i].from_edid_timing ? "" : "  (computed)");
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "set")) {
        char *end;
        long w = strtol(argv[2], &end, 10), h = 0, hz = 0;
        if (*end == 'x') h = strtol(end + 1, &end, 10);
        if (*end == '@') hz = strtol(end + 1, &end, 10);
        struct gpu_timing m[EDID_MAX_MODES];
        int n = gpu_modes(m, EDID_MAX_MODES);
        for (int i = 0; i < n; i++)
            if (m[i].hactive == w && m[i].vactive == h && (!hz || (m[i].refresh_mhz + 500) / 1000 == (uint32_t)hz)) {
                int r = gpu_apply_mode(&m[i]);
                if (r) { err(sh, "gpu", "the adapter refused that mode"); return 1; }
                char v[32];
                snprintf(v, sizeof(v), "%dx%d@%u", m[i].hactive, m[i].vactive, (m[i].refresh_mhz + 500) / 1000);
                conf_set(&sysconf, "display_mode", v);
                size_t cn;
                char *t = conf_format(&sysconf, &cn);
                if (t) { sys_write_etc("zenith/system.conf", t, cn); kfree(t); }
                pr(sh, "display mode %s\n", v);
                return 0;
            }
        err(sh, "gpu", "no such mode (see: gpu modes)");
        return 1;
    }
    struct gpu_status st;
    gpu_get_status(&st);
    pr(sh, "adapter   %s\ndriver    %s%s%s\nmonitor   %s\nmode      %dx%d at %u.%03u Hz\npolicy    %s\nstatus    %s\n", st.adapter, st.driver,
       st.package[0] ? "  (package " : "", st.package, st.monitor[0] ? st.monitor : "unknown", st.w, st.h, st.refresh_mhz / 1000,
       st.refresh_mhz % 1000, st.mode_policy, st.state);
    return 0;
}

static void pkg_print_log(struct shell *sh, const struct pkg_job *j)
{
    for (int i = 0; i < j->nlog; i++) pr(sh, "  %s\n", j->log[i]);
}

static int c_pkg(struct shell *sh, int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "";
    if (!strcmp(cmd, "update")) {
        struct pkg_job *j = kzalloc(sizeof(*j));
        int n = pkg_update(j);
        pkg_print_log(sh, j);
        pr(sh, "%d package(s) known\n", n);
        kfree(j);
        return 0;
    }
    if (!strcmp(cmd, "sources")) {
        const struct pkg_source *src;
        int n = pkg_sources(&src);
        for (int i = 0; i < n; i++)
            pr(sh, "%-12s %s  %s%s%s\n", src[i].name, src[i].url, src[i].require_signature ? "signed" : "unsigned",
               src[i].error[0] ? "  ERROR: " : "", src[i].error);
        return 0;
    }
    if (!strcmp(cmd, "search") || !strcmp(cmd, "list")) {
        int idx[64];
        int n = pkg_search(argc > 2 ? argv[2] : "", idx, 64);
        for (int i = 0; i < n; i++) {
            const struct pkg *p = pkg_at(idx[i]);
            pr(sh, "%s%-22s %-10s %-8s %s\n", p->installed ? C_GREEN "*" C_RESET : " ", p->name, p->version, p->type, p->desc);
        }
        if (!n) pr(sh, "nothing found (try: pkg update)\n");
        return 0;
    }
    if (!strcmp(cmd, "installed")) {
        const struct pkg *l[64];
        int n = pkg_installed_list(l, 64);
        for (int i = 0; i < n; i++) pr(sh, "%-22s %-10s %s\n", l[i]->name, l[i]->version, l[i]->type);
        return 0;
    }
    if (!strcmp(cmd, "info") && argc > 2) {
        const struct pkg *p = pkg_lookup(argv[2]);
        if (!p) { err(sh, "pkg", "unknown package"); return 1; }
        pr(sh, "%s %s (%s, %s)\n%s\nsource %d, %s, %lu bytes\n", p->name, p->version, p->type, p->format[0] ? p->format : "zpk", p->desc,
           p->source, p->verified ? "signed" : "NOT signed", (unsigned long)p->size);
        for (int i = 0; i < p->ndeps; i++) pr(sh, "needs %s\n", p->depends[i]);
        return 0;
    }
    if ((!strcmp(cmd, "install") || !strcmp(cmd, "remove")) && argc > 2) {
        struct pkg_job *j = kzalloc(sizeof(*j));
        int r = !strcmp(cmd, "install") ? pkg_install(argv[2], j) : pkg_remove(argv[2], j);
        pkg_print_log(sh, j);
        if (r) pr(sh, C_RED "%s" C_RESET "\n", j->error);
        else pr(sh, C_GREEN "done" C_RESET "\n");
        kfree(j);
        return r ? 1 : 0;
    }
    err(sh, "pkg", "usage: pkg update | sources | search [text] | info <name> | install <name> | remove <name> | installed");
    return 1;
}

int sv_dump(char *out, size_t n);

static int c_svps(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    char *b = kmalloc(8192);
    if (!b) return 1;
    int n = sv_dump(b, 8192);
    out(sh, b, (size_t)n);
    kfree(b);
    return 0;
}

static int c_module(struct shell *sh, int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "load")) {
        size_t n;
        char *img = vfs_read_file(argv[2], &n);
        if (!img) { err(sh, "module", "cannot read that file"); return 1; }
        char e[120] = "";
        int r = module_load(img, n, argv[2], e, sizeof(e));
        kfree(img);
        if (r) { pr(sh, C_RED "%s" C_RESET "\n", e); return 1; }
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "unload")) return module_unload(argv[2]) ? 1 : 0;
    struct module_status m[16];
    int n = module_list(m, 16);
    for (int i = 0; i < n; i++) pr(sh, "%-12s %-6s %s  (%s)\n", m[i].name, m[i].version, m[i].description, m[i].origin);
    if (!n) pr(sh, "no modules loaded\n");
    return 0;
}

static int c_bootmgr(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    if (!uefi_available()) { pr(sh, "the firmware's runtime services are not available\n"); return 1; }
    static char list[16][64];
    int n = uefi_list_boot_entries(list, 16);
    for (int i = 0; i < n; i++) pr(sh, "%d. %s\n", i + 1, list[i]);
    if (!n) pr(sh, "no boot entries\n");
    return 0;
}

static int c_time(struct shell *sh, int argc, char **argv);
static int run_argv(struct shell *sh, int argc, char **argv);

static int c_net(struct shell *sh, int argc, char **argv);
static int c_netcards(struct shell *sh, int argc, char **argv);

static const struct cmd commands[] = {
    { "help", c_help, "list commands" },
    { "ls", c_ls, "list directory [-l] [-a]" },
    { "cd", c_cd, "change directory" },
    { "pwd", c_pwd, "print working directory" },
    { "cat", c_cat, "print files" },
    { "head", c_head, "first lines of a file [-n N]" },
    { "tail", c_head, "last lines of a file [-n N]" },
    { "grep", c_grep, "search text in files" },
    { "wc", c_wc, "count lines, words, bytes" },
    { "tree", c_tree, "show a directory tree" },
    { "hexdump", c_hexdump, "hex view of a file" },
    { "mkdir", c_mkdir, "create directories" },
    { "rm", c_rm, "remove files [-r]" },
    { "touch", c_touch, "create an empty file" },
    { "cp", c_cp, "copy files or directories" },
    { "mv", c_mv, "move / rename" },
    { "write", c_write, "write text to a file" },
    { "echo", c_echo, "print text" },
    { "edit", c_edit, "open a file in the text editor" },
    { "open", c_open, "open a file or app" },
    { "apps", c_apps, "list graphical apps" },
    { "clear", c_clear, "clear the screen" },
    { "date", c_date, "show date and time" },
    { "cal", c_cal, "show a calendar" },
    { "uptime", c_uptime, "time since boot" },
    { "free", c_free, "memory usage" },
    { "df", c_df, "file system usage" },
    { "lsblk", c_lsblk, "list disks and partitions" },
    { "mount", c_mount, "list or mount file systems" },
    { "sync", c_sync, "write cached changes to disk" },
    { "mkfs", c_mkfs, "format a disk or partition (FAT32)" },
    { "umount", c_umount, "unmount a disk" },
    { "sha256sum", c_sha256sum, "SHA-256 checksum of files" },
    { "play", c_play, "play a WAV file or a system sound" },
    { "beep", c_beep, "play a tone: beep [Hz] [ms]" },
    { "volume", c_volume, "show or set the volume (0-100, mute)" },
    { "install", c_install, "install ZenithOS to a disk" },
    { "gpu", c_gpu, "graphics adapter, driver and display mode" },
    { "pkg", c_pkg, "install and remove packages (update, search, install ...)" },
    { "svps", c_svps, "programs of the System V interface and their threads" },
    { "module", c_module, "list, load and unload kernel modules" },
    { "bootmgr", c_bootmgr, "firmware boot entries (UEFI)" },
    { "ps", c_ps, "list threads and processes" },
    { "kill", c_kill, "terminate a process" },
    { "uname", c_uname, "system name [-a]" },
    { "lscpu", c_lscpu, "processor information" },
    { "lspci", c_lspci, "list PCI devices" },
    { "dmesg", c_dmesg, "kernel log" },
    { "neofetch", c_neofetch, "system summary with logo" },
    { "calc", c_calc, "evaluate an expression" },
    { "bench", c_bench, "single vs. multi-core benchmark" },
    { "fractal", c_fractal, "ASCII Mandelbrot" },
    { "colors", c_colors, "show terminal colours" },
    { "matrix", c_matrix, "digital rain (any key to stop)" },
    { "sleep", c_sleep, "wait N seconds" },
    { "time", c_time, "measure a command" },
    { "layout", c_layout, "keyboard layout de|us" },
    { "wallpaper", c_wallpaper, "list or set the wallpaper" },
    { "screenshot", c_screenshot, "save a screenshot" },
    { "history", c_history, "command history" },
    { "whoami", c_whoami, "current user" },
    { "hostname", c_hostname, "machine name" },
    { "ifconfig", c_net, "network interface status" },
    { "dhcp", c_net, "request an IP address" },
    { "netcards", c_netcards, "supported network adapters" },
    { "ping", c_net, "ping a host" },
    { "nslookup", c_net, "resolve a host name" },
    { "wget", c_net, "download a file over HTTP or HTTPS" },
    { "curl", c_net, "print a web page (HTTP or HTTPS)" },
    { "reboot", c_reboot, "restart the computer" },
    { "shutdown", c_reboot, "power off" },
    { "exit", c_exit, "close the shell" },
};

static int c_help(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    pr(sh, C_BOLD "ZenithOS shell - built-in commands" C_RESET "\n\n");
    int n = (int)ARRAY_SIZE(commands);
    int half = (n + 1) / 2;
    for (int i = 0; i < half; i++) {
        pr(sh, "  " C_CYAN "%-10s" C_RESET " %-30.30s", commands[i].name, commands[i].help);
        if (i + half < n) pr(sh, "  " C_CYAN "%-10s" C_RESET " %.30s", commands[i + half].name, commands[i + half].help);
        pr(sh, "\n");
    }
    pr(sh, "\n" C_DIM "Programs in /bin run as ring-3 user processes. Redirect output with > and >>." C_RESET "\n");
    return 0;
}

static int c_time(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) return 0;
    uint64_t t0 = uptime_ms();
    int r = run_argv(sh, argc - 1, argv + 1);
    uint64_t t = uptime_ms() - t0;
    pr(sh, C_DIM "real %lu.%03lus" C_RESET "\n", t / 1000, t % 1000);
    return r;
}

/* ------------------------------------------------------------------------
 * network commands
 * ---------------------------------------------------------------------- */

struct card_ctx { struct shell *sh; const char *last; int n; };

static void card_line(void *ctx, const char *driver, const char *family, uint16_t vendor, uint16_t device, const char *model)
{
    struct card_ctx *c = ctx;
    if (!c->last || strcmp(c->last, driver)) {
        pr(c->sh, "\n" C_BOLD "%s" C_RESET "  " C_DIM "%s" C_RESET "\n", driver, family);
        c->last = driver;
    }
    pr(c->sh, "  %04x:%04x  %s\n", vendor, device, model);
    c->n++;
}

static int c_netcards(struct shell *sh, int argc, char **argv)
{
    UNUSED(argc); UNUSED(argv);
    struct card_ctx c = { sh, NULL, 0 };
    net_card_list(card_line, &c);
    pr(sh, "\n" C_BOLD "usb" C_RESET "  " C_DIM "USB network adapters and phones (USB tethering)" C_RESET "\n");
    pr(sh, "  any        CDC Ethernet (ECM) class devices\n");
    pr(sh, "  any        RNDIS devices (Android USB tethering)\n");
    pr(sh, "\n%d PCI models plus USB class devices\n", c.n);
    return 0;
}

static void print_iface(struct shell *sh, struct net_info *ni)
{
    char ip[20], gw[20], dns[20], mask[20];
    ip_to_str(ni->ip, ip);
    ip_to_str(ni->gateway, gw);
    ip_to_str(ni->dns, dns);
    ip_to_str(ni->netmask, mask);
    pr(sh, C_BOLD "%s" C_RESET ": %s  link %s%s\n", ni->ifname, ni->driver,
       ni->link ? C_GREEN "up" C_RESET : C_RED "down" C_RESET, ni->is_default ? C_CYAN "  (default route)" C_RESET : "");
    pr(sh, "    ether %02x:%02x:%02x:%02x:%02x:%02x  driver %s\n", ni->mac[0], ni->mac[1], ni->mac[2], ni->mac[3],
       ni->mac[4], ni->mac[5], ni->drv);
    if (ni->ip) {
        pr(sh, "    inet  %s  netmask %s\n", ip, mask);
        pr(sh, "    gateway %s, dns %s\n", gw, dns);
    } else {
        pr(sh, "    inet  (none)\n");
    }
    pr(sh, "    rx %lu packets (%lu bytes), tx %lu packets (%lu bytes)%s\n", ni->rx_packets, ni->rx_bytes, ni->tx_packets,
       ni->tx_bytes, ni->rx_dropped ? ", bad checksums dropped" : "");
}

static int c_net(struct shell *sh, int argc, char **argv)
{
    struct net_info ni;
    if (!net_get_info(&ni)) {
        err(sh, argv[0], "no network adapter found (see netcards for supported ones)");
        return 1;
    }
    char ip[20], gw[20];
    if (!strcmp(argv[0], "ifconfig")) {
        for (int i = 0; i < net_iface_count(); i++) {
            struct net_info x;
            if (!net_iface_get(i, &x)) continue;
            if (i) pr(sh, "\n");
            print_iface(sh, &x);
        }
        return 0;
    }
    if (!strcmp(argv[0], "dhcp")) {
        pr(sh, "Requesting addresses...\n");
        if (net_dhcp(3000)) {
            for (int i = 0; i < net_iface_count(); i++) {
                struct net_info x;
                if (!net_iface_get(i, &x) || !x.ip) continue;
                ip_to_str(x.ip, ip);
                ip_to_str(x.gateway, gw);
                pr(sh, "%s: " C_GREEN "bound to %s" C_RESET " (gateway %s)\n", x.ifname, ip, gw);
            }
            return 0;
        }
        err(sh, "dhcp", "no answer");
        return 1;
    }
    if (!ni.ip && !net_dhcp(3000)) { err(sh, argv[0], "no IP address (dhcp failed)"); return 1; }
    if (argc < 2) { err(sh, argv[0], "missing host"); return 1; }
    if (!strcmp(argv[0], "nslookup")) {
        uint32_t a;
        if (!net_resolve(argv[1], &a, 3000)) { err(sh, "nslookup", "could not resolve"); return 1; }
        ip_to_str(a, ip);
        pr(sh, "%s has address " C_BOLD "%s" C_RESET "\n", argv[1], ip);
        return 0;
    }
    if (!strcmp(argv[0], "ping")) {
        uint32_t a;
        if (!net_resolve(argv[1], &a, 3000)) { err(sh, "ping", "could not resolve"); return 1; }
        ip_to_str(a, ip);
        int count = argc > 2 ? atoi(argv[2]) : 4, ok = 0;
        pr(sh, "PING %s (%s)\n", argv[1], ip);
        for (int i = 0; i < count && !sh->tty->interrupt; i++) {
            int rtt = net_ping(a, (uint16_t)i, 2000);
            if (rtt >= 0) { pr(sh, "reply from %s: seq=%d time=%d ms\n", ip, i, rtt); ok++; }
            else pr(sh, C_YEL "request timed out (seq=%d)" C_RESET "\n", i);
            if (i + 1 < count) sched_sleep(500);
        }
        pr(sh, "%d packets sent, %d received\n", count, ok);
        return ok ? 0 : 1;
    }
    if (!strcmp(argv[0], "wget") || !strcmp(argv[0], "curl")) {
        bool save = !strcmp(argv[0], "wget");
        char url[512];
        strlcpy(url, argv[1], sizeof(url));
        if (!strstr(url, "://")) snprintf(url, sizeof(url), "http://%s", argv[1]);
        struct http_result *r = kmalloc(sizeof(*r));
        int status = -1;
        for (int hop = 0; hop < 5; hop++) {
            status = net_http_request(url, r, 15000);
            if (status >= 300 && status < 400 && r->location[0]) {
                char next[512];
                if (strstr(r->location, "://")) strlcpy(next, r->location, sizeof(next));
                else {
                    /* relative redirect: keep scheme and host */
                    const char *h = strstr(url, "://");
                    const char *slash = h ? strchr(h + 3, '/') : NULL;
                    size_t keep = slash ? (size_t)(slash - url) : strlen(url);
                    snprintf(next, sizeof(next), "%.*s%s%s", (int)keep, url, r->location[0] == '/' ? "" : "/", r->location);
                }
                if (save) pr(sh, C_DIM "HTTP %d -> %s" C_RESET "\n", status, next);
                kfree(r->body);
                strlcpy(url, next, sizeof(url));
                continue;
            }
            break;
        }
        if (status < 0) {
            err(sh, argv[0], r->error[0] ? r->error : "request failed");
            kfree(r);
            return 1;
        }
        if (save) {
            if (r->tls[0]) pr(sh, C_GREEN "secure connection:" C_RESET " %s\n", r->tls);
            pr(sh, "HTTP %d, %lu bytes%s%s\n", status, r->len, r->content_type[0] ? ", " : "", r->content_type);
            const char *name = argc > 2 ? argv[2] : "index.html";
            char p[VFS_PATH_MAX];
            resolve(sh, name, p);
            vfs_write_file(p, r->body, r->len);
            pr(sh, "saved to %s\n", p);
        } else {
            out(sh, r->body, MIN(r->len, (size_t)200000));
            if (r->len && r->body[r->len - 1] != '\n') out(sh, "\n", 1);
        }
        kfree(r->body);
        kfree(r);
        return status >= 400;
    }
    return 1;
}

/* ------------------------------------------------------------------------
 * running things
 * ---------------------------------------------------------------------- */

static bool find_program(struct shell *sh, const char *name, char *path)
{
    if (strchr(name, '/')) {
        resolve(sh, name, path);
    } else {
        snprintf(path, VFS_PATH_MAX, "/bin/%s", name);
    }
    struct vfs_stat st;
    return !vfs_stat(path, &st) && st.type == VN_FILE;
}

static int run_argv(struct shell *sh, int argc, char **argv)
{
    for (unsigned i = 0; i < ARRAY_SIZE(commands); i++)
        if (!strcmp(commands[i].name, argv[0])) return commands[i].fn(sh, argc, argv);

    char path[VFS_PATH_MAX];
    if (find_program(sh, argv[0], path)) {
        struct abi_proc ap;
        if (abi_exec(path, argc, argv, sh->tty, sh->cwd, &ap)) { err(sh, argv[0], "cannot execute"); return 127; }
        int pid = ap.pid;
        sh->tty->raw = false;
        sh->tty->fg_pid = pid;
        int code = abi_wait(&ap);
        sh->tty->fg_pid = 0;
        sh->tty->raw = true;
        if (code == -9) pr(sh, C_YEL "\n[killed]" C_RESET "\n");
        else if (code < -1000) pr(sh, C_RED "\n[crashed: %s]" C_RESET "\n", code == -1014 ? "page fault" : "fault");
        return code;
    }
    if (app_find(argv[0])) {
        app_launch(argv[0]);
        return 0;
    }
    pr(sh, C_RED "zsh: command not found: %s" C_RESET "  " C_DIM "(try 'help')" C_RESET "\n", argv[0]);
    return 127;
}

static int tokenize(char *line, char **argv)
{
    int argc = 0;
    char *p = line, *w = line;
    while (*p && argc < MAXARGS - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = w;
        char q = 0;
        while (*p && (q || (*p != ' ' && *p != '\t'))) {
            if (!q && (*p == '"' || *p == '\'')) { q = *p++; continue; }
            if (!q && *p == '\\' && p[1]) { p++; *w++ = *p++; continue; }
            if (q && *p == q) { q = 0; p++; continue; }
            *w++ = *p++;
        }
        if (*p) p++;
        *w++ = 0;
    }
    argv[argc] = NULL;
    return argc;
}

static void execute(struct shell *sh, char *line)
{
    /* redirection:  cmd > file   /  cmd >> file */
    char *redir = NULL;
    bool append = false;
    char *gt = strchr(line, '>');
    if (gt) {
        append = gt[1] == '>';
        *gt = 0;
        redir = gt + (append ? 2 : 1);
        while (*redir == ' ') redir++;
        char *e = redir + strlen(redir);
        while (e > redir && e[-1] == ' ') *--e = 0;
    }
    char *argv[MAXARGS];
    int argc = tokenize(line, argv);
    if (!argc) return;
    sh->tty->interrupt = false;
    if (redir && *redir) {
        sh->rbuf = kmalloc(256);
        sh->rcap = 256;
        sh->rlen = 0;
    }
    sh->last_status = run_argv(sh, argc, argv);
    if (sh->rbuf) {
        char p[VFS_PATH_MAX];
        resolve(sh, redir, p);
        char *buf = sh->rbuf;
        size_t len = sh->rlen;
        sh->rbuf = NULL;
        if (append) {
            vnode_t *vn = vfs_open(p, true);
            if (vn) { vfs_write(vn, vn->size, buf, len); vfs_close(vn); }
        } else {
            vfs_write_file(p, buf, len);
        }
        kfree(buf);
    }
}

/* ------------------------------------------------------------------------
 * line editor
 * ---------------------------------------------------------------------- */

static void prompt(struct shell *sh)
{
    char dir[VFS_PATH_MAX];
    size_t hl = strlen(session_home());
    if (!strncmp(sh->cwd, session_home(), hl) && (!sh->cwd[hl] || sh->cwd[hl] == '/')) snprintf(dir, sizeof(dir), "~%s", sh->cwd + hl);
    else strlcpy(dir, sh->cwd, sizeof(dir));
    const char *user = session.user[0] ? session.user : "nobody";
    pr(sh, "\x1b[38;2;176;140;255m\x1b[1m%s@%s\x1b[0m \x1b[38;2;80;200;255m%s\x1b[0m %s❯\x1b[0m ", user, sys_hostname(), dir,
       sh->last_status ? C_RED : C_GREEN);
    sh->pw = (int)(strlen(user) + 1 + strlen(sys_hostname()) + 1 + strlen(dir) + 3);         /* "user@host dir ❯ " */
}

static int display_len(const char *s, int bytes)
{
    int n = 0;
    for (int i = 0; i < bytes; i++)
        if (((uint8_t)s[i] & 0xC0) != 0x80) n++;
    return n;
}

/* Rewrite the input line. The cursor is `*shown_cur` characters into it; the line may have wrapped over several rows,
 * so go up to the first row, write everything, clear what is left and put the cursor where it belongs. */
static void redraw(struct shell *sh, const char *buf, int len, int cur, int *shown_cur)
{
    int cols = sh->tty->cols > 20 ? sh->tty->cols : 80;
    int at = sh->pw + *shown_cur;
    if (at / cols) pr(sh, "\x1b[%dA", at / cols);
    pr(sh, "\r");
    if (sh->pw) pr(sh, "\x1b[%dC", sh->pw);
    out(sh, buf, (size_t)len);
    int end = sh->pw + display_len(buf, len);
    if (len && end % cols == 0) pr(sh, " \r");                /* the cursor waits at the end of the row: really start the next one */
    pr(sh, "\x1b[K\x1b[J");
    int want = sh->pw + display_len(buf, cur);
    if (end / cols > want / cols) pr(sh, "\x1b[%dA", end / cols - want / cols);
    pr(sh, "\x1b[%dG", want % cols + 1);
    *shown_cur = display_len(buf, cur);
}

static bool complete(struct shell *sh, char *buf, int *len, int *cur)
{
    /* complete the word before the cursor: commands first word, else paths */
    int start = *cur;
    while (start > 0 && buf[start - 1] != ' ') start--;
    char word[128];
    int wl = MIN(*cur - start, 127);
    memcpy(word, buf + start, (size_t)wl);
    word[wl] = 0;
    char cands[32][64];
    int nc = 0;
    bool first = true;
    for (int i = 0; i < start; i++) if (buf[i] != ' ') first = false;
    if (first) {
        for (unsigned i = 0; i < ARRAY_SIZE(commands) && nc < 32; i++)
            if (!strncmp(commands[i].name, word, (size_t)wl)) strlcpy(cands[nc++], commands[i].name, 64);
        struct vfs_dirent de[64];
        int n = vfs_list("/bin", de, 64);
        for (int i = 0; i < n && nc < 32; i++)
            if (!strncmp(de[i].name, word, (size_t)wl)) strlcpy(cands[nc++], de[i].name, 64);
    }
    const char *slash = strrchr(word, '/');
    char dir[VFS_PATH_MAX], base[64];
    if (!first || slash) {
        if (slash) {
            char d[128];
            memcpy(d, word, (size_t)(slash - word + 1));
            d[slash - word + 1] = 0;
            resolve(sh, d, dir);
            strlcpy(base, slash + 1, sizeof(base));
        } else {
            strlcpy(dir, sh->cwd, sizeof(dir));
            strlcpy(base, word, sizeof(base));
        }
        struct vfs_dirent de[64];
        int n = vfs_list(dir, de, 64);
        size_t bl = strlen(base);
        for (int i = 0; i < n && nc < 32; i++)
            if (!strncmp(de[i].name, base, bl)) {
                snprintf(cands[nc], 64, "%s%s", de[i].name, de[i].type == VN_DIR ? "/" : "");
                nc++;
            }
        if (nc) {
            /* candidates are relative to the last slash */
            wl = (int)bl;
            start = *cur - wl;
        }
    }
    if (!nc) return false;
    /* longest common prefix */
    int common = (int)strlen(cands[0]);
    for (int i = 1; i < nc; i++) {
        int k = 0;
        while (k < common && cands[i][k] == cands[0][k]) k++;
        common = k;
    }
    if (common > wl) {
        int add = common - wl;
        if (*len + add + 1 < 250) {
            memmove(buf + *cur + add, buf + *cur, (size_t)(*len - *cur));
            memcpy(buf + *cur, cands[0] + wl, (size_t)add);
            *len += add;
            *cur += add;
            if (nc == 1 && cands[0][common - 1] != '/' && *len + 1 < 250) {
                memmove(buf + *cur + 1, buf + *cur, (size_t)(*len - *cur));
                buf[*cur] = ' ';
                (*len)++;
                (*cur)++;
            }
        }
    } else if (nc > 1) {
        pr(sh, "\n");
        for (int i = 0; i < nc; i++) pr(sh, "%s  ", cands[i]);
        pr(sh, "\n");
        prompt(sh);
        return true;
    }
    return false;
}

static bool read_line(struct shell *sh, char *buf, int cap)
{
    int len = 0, cur = 0, shown = 0, hidx = sh->nhist;
    buf[0] = 0;
    /* a command queued by the terminal (e.g. "open with terminal") */
    if (sh->tty->line[0]) {
        len = cur = (int)strlcpy(buf, sh->tty->line, (size_t)cap);
        sh->tty->line[0] = 0;
        redraw(sh, buf, len, cur, &shown);
        pr(sh, "\n");
        return true;
    }
    struct gui_event ev;
    for (;;) {
        if (!tty_getkey(sh->tty, &ev, -1)) return false;
        bool ctrl = ev.mods & MOD_CTRL;
        if (ev.key == KEY_ENTER || ev.key == KEY_KPENTER) {
            buf[len] = 0;
            if (cur != len) redraw(sh, buf, len, len, &shown);       /* the line ends below the cursor */
            pr(sh, "\n");
            return true;
        }
        if (ctrl && ev.key == KEY_C) {
            pr(sh, C_DIM "^C" C_RESET "\n");
            buf[0] = 0;
            return true;
        }
        if (ctrl && ev.key == KEY_L) {
            pr(sh, "\x1b[2J\x1b[H");
            prompt(sh);
            shown = 0;
            redraw(sh, buf, len, cur, &shown);
            continue;
        }
        if (ctrl && ev.key == KEY_D && !len) { sh->quit = true; return true; }
        if (ctrl && ev.key == KEY_U) { len = cur = 0; }
        else if (ctrl && ev.key == KEY_A) cur = 0;
        else if (ctrl && ev.key == KEY_E) cur = len;
        else if (ev.key == KEY_BACKSPACE) {
            if (cur > 0) {
                int n = utf8_prev_len(buf, buf + cur);
                memmove(buf + cur - n, buf + cur, (size_t)(len - cur));
                len -= n;
                cur -= n;
            }
        } else if (ev.key == KEY_DELETE) {
            if (cur < len) {
                const char *p = buf + cur;
                utf8_next(&p);
                int n = (int)(p - (buf + cur));
                memmove(buf + cur, buf + cur + n, (size_t)(len - cur - n));
                len -= n;
            }
        } else if (ev.key == KEY_LEFT) {
            if (cur > 0) cur -= utf8_prev_len(buf, buf + cur);
        } else if (ev.key == KEY_RIGHT) {
            if (cur < len) { const char *p = buf + cur; utf8_next(&p); cur = (int)(p - buf); }
        } else if (ev.key == KEY_HOME) cur = 0;
        else if (ev.key == KEY_END) cur = len;
        else if (ev.key == KEY_UP || ev.key == KEY_DOWN) {
            if (ev.key == KEY_UP && hidx > 0) hidx--;
            else if (ev.key == KEY_DOWN && hidx < sh->nhist) hidx++;
            if (hidx < sh->nhist) len = (int)strlcpy(buf, sh->hist[hidx], (size_t)cap);
            else len = 0;
            cur = len;
        } else if (ev.key == KEY_TAB) {
            buf[len] = 0;
            if (complete(sh, buf, &len, &cur)) shown = 0;          /* it listed candidates and printed a new prompt */
            redraw(sh, buf, len, cur, &shown);
            continue;
        } else if (ev.ch >= 32 && !ctrl) {
            char enc[4];
            int n = utf8_encode(ev.ch, enc);
            if (len + n < cap - 1) {
                memmove(buf + cur + n, buf + cur, (size_t)(len - cur));
                memcpy(buf + cur, enc, (size_t)n);
                len += n;
                cur += n;
            }
        } else {
            continue;
        }
        buf[len] = 0;
        redraw(sh, buf, len, cur, &shown);
    }
}

int shell_main(void *arg)
{
    struct tty *tty = arg;
    struct shell *sh = kzalloc(sizeof(*sh));
    sh->tty = tty;
    strlcpy(sh->cwd, session_home(), sizeof(sh->cwd));
    pr(sh, "\x1b[1mZenithOS %s\x1b[0m \"%s\"  \x1b[90m·  type \x1b[0m\x1b[96mhelp\x1b[0m\x1b[90m for commands, "
           "\x1b[0m\x1b[96mneofetch\x1b[0m\x1b[90m for a summary\x1b[0m\n\n",
       ZENITH_VERSION, ZENITH_CODENAME);
    char line[256];
    while (!sh->quit && !tty->closed) {
        prompt(sh);
        if (!read_line(sh, line, sizeof(line))) break;
        char *p = line;
        while (*p == ' ') p++;
        if (!*p) continue;
        if (!sh->nhist || strcmp(sh->hist[sh->nhist - 1], p)) {
            if (sh->nhist == HIST) { memmove(sh->hist[0], sh->hist[1], sizeof(sh->hist[0]) * (HIST - 1)); sh->nhist--; }
            strlcpy(sh->hist[sh->nhist++], p, sizeof(sh->hist[0]));
        }
        execute(sh, p);
    }
    if (tty->hangup && !tty->closed) tty->hangup(tty->term);
    tty_put(tty);
    kfree(sh);
    return 0;
}
