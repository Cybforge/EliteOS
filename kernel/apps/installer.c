/*
 * Installer: puts the running system on a disk and sets it up for the person
 * who is installing it.
 *
 * It copies the UEFI loader, kernel and system image, writes the choices
 * (language, keyboard, time zone, computer name, the first account with its
 * password hash, graphics mode) as the new system's configuration, optionally
 * downloads the graphics driver for this computer's adapter onto the disk,
 * adds a UEFI boot entry for it and puts that entry first in the boot order.
 *
 * Two ways: add ZenithOS to an existing FAT32 volume (files are kept, the
 * partition is marked as an EFI System Partition), or erase the disk and
 * create a fresh GPT with one FAT32 EFI System Partition.
 */
#include <wm.h>
#include <block.h>
#include <vfs.h>
#include <mm.h>
#include <sched.h>
#include <bootinfo.h>
#include <sys.h>
#include <gpu.h>
#include <pkg.h>
#include <uefi.h>

extern struct bootinfo *boot_info;

/* ------------------------------------------------------------------------
 * the installation itself (also used by the shell's `install`)
 * ---------------------------------------------------------------------- */

#define INSTALL_STEPS 8

static void step(struct install_progress *p, int k, const char *msg)
{
    p->step = k;
    p->pct = k * 100 / (INSTALL_STEPS + 1);
    strlcpy(p->msg, msg, sizeof(p->msg));
    klog("install: %s", msg);
}

static int fail(struct install_progress *p, const char *msg)
{
    strlcpy(p->msg, msg, sizeof(p->msg));
    klog("install: failed: %s", msg);
    p->failed = true;
    p->done = true;
    return E_IO;
}

static bool on_disk(struct blockdev *d, struct blockdev *disk) { return d == disk || d->parent == disk; }

static struct fs_mount *mount_on(struct blockdev *disk)
{
    for (struct fs_mount *m = vfs_mounts(); m; m = m->next) {
        struct blockdev *d = blk_find(m->dev);
        if (d && on_disk(d, disk)) return m;
    }
    return NULL;
}

static int mount_somewhere(struct blockdev *d, char *path, size_t n)
{
    for (int k = 1; k < 10; k++) {
        if (k == 1) strlcpy(path, "/disk", n);
        else snprintf(path, n, "/disk%d", k);
        struct vfs_stat st;
        if (!vfs_stat(path, &st) && vfs_mount_of(path)) continue;
        if (!fat_mount(d, path)) return 0;
    }
    return E_EXIST;
}

static int copy_blob(const char *path, uint64_t phys, uint64_t size)
{
    char dir[VFS_PATH_MAX];
    vfs_dirname(path, dir, sizeof(dir));
    sys_mkdir_p(dir);
    return vfs_write_file(path, P2V(phys), size);
}

void install_defaults(struct install_opts *o)
{
    memset(o, 0, sizeof(*o));
    strlcpy(o->language, i18n_current(), sizeof(o->language));
    strlcpy(o->keyboard, keymap_current(), sizeof(o->keyboard));
    strlcpy(o->timezone, tz_name(), sizeof(o->timezone));
    strlcpy(o->hostname, conf_get(&sysconf, "install_hostname", "zenith"), sizeof(o->hostname));
    o->autologin = false;
    o->gpu_driver = true;
}

static bool valid_hostname(const char *h)
{
    size_t l = strlen(h);
    if (!l || l > 39) return false;
    for (size_t i = 0; i < l; i++) {
        char c = h[i];
        if (!(islower((unsigned char)c) || isdigit((unsigned char)c) || c == '-')) return false;
    }
    return h[0] != '-' && h[l - 1] != '-';
}

/* the new system's /etc: what the person chose, written next to the image's defaults */
static int write_settings(const struct install_opts *o, const char *root, struct install_progress *p)
{
    char dir[VFS_PATH_MAX], path[VFS_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/zenith/etc/zenith", root);
    if (sys_mkdir_p(dir)) return E_IO;
    snprintf(path, sizeof(path), "%s/zenith/persist", root);
    vfs_write_file(path, "ZenithOS system volume\n", 23);                 /* the marker that makes this a system volume */
    snprintf(path, sizeof(path), "%s/zenith/etc/hostname", root);
    char hn[48];
    int hl = snprintf(hn, sizeof(hn), "%s\n", o->hostname);
    vfs_write_file(path, hn, (size_t)hl);

    struct conf c;
    conf_init(&c);
    conf_load(&c, "/etc/zenith/system.conf");                              /* the image's defaults */
    conf_set(&c, "language", o->language);
    conf_set(&c, "keyboard", o->keyboard);
    conf_set(&c, "timezone", o->timezone);
    conf_set(&c, "graphics", o->gpu_driver ? "driver" : "gop");
    conf_set(&c, "autologin", o->autologin ? o->user : "");
    conf_set_int(&c, "rtc_local", rtc_is_local);
    size_t n;
    char *t = conf_format(&c, &n);
    conf_free(&c);
    if (!t) return E_NOMEM;
    snprintf(path, sizeof(path), "%s/zenith/etc/zenith/system.conf", root);
    int r = vfs_write_file(path, t, n);
    kfree(t);
    if (r) return r;

    char line[400];
    if (users_make_line(o->user, o->fullname, o->password, true, 1000, line, sizeof(line))) return E_INVAL;
    snprintf(path, sizeof(path), "%s/zenith/etc/zenith/users", root);
    r = vfs_write_file(path, line, strlen(line));
    if (r) return r;

    static const char *dirs[] = { "home", "zenith/opt", "zenith/var" };
    for (unsigned i = 0; i < ARRAY_SIZE(dirs); i++) {
        snprintf(path, sizeof(path), "%s/%s", root, dirs[i]);
        sys_mkdir_p(path);
    }
    UNUSED(p);
    return 0;
}

int zenith_install(const struct install_opts *o, struct install_progress *p)
{
    struct blockdev *disk = o->disk;
    p->nsteps = INSTALL_STEPS;
    p->done = p->failed = false;
    p->note[0] = 0;
    p->boot_entry = false;
    if (!boot_info->loader_file_size || !boot_info->kernel_file_size || !boot_info->initrd_size)
        return fail(p, "The boot files are not available (was ZenithOS started by its own loader?)");
    if (!users_valid_name(o->user)) return fail(p, "The user name is not valid");
    if (!valid_hostname(o->hostname)) return fail(p, "The computer name is not valid");
    if (disk->parent) disk = disk->parent;
    char root[64] = "";
    struct blockdev *target = NULL;

    if (o->erase) {
        step(p, 1, "Unmounting the disk");
        for (struct fs_mount *m; (m = mount_on(disk));) {
            char path[64];
            strlcpy(path, m->path, sizeof(path));
            if (vfs_umount(path)) return fail(p, "Could not unmount the disk");
        }
        step(p, 1, "Creating the partition table (GPT)");
        if (gpt_create_single(disk, "ZenithOS", true)) return fail(p, "Could not write the partition table");
        for (int i = 0; i < blk_count(); i++)
            if (blk_get(i)->parent == disk) target = blk_get(i);
        if (!target) return fail(p, "The new partition did not show up");
        step(p, 1, "Formatting the EFI System Partition (FAT32)");
        if (fat_mkfs(target, "ZENITHOS")) return fail(p, "Formatting failed (is the disk at least 64 MiB?)");
        if (mount_somewhere(target, root, sizeof(root))) return fail(p, "Could not mount the new file system");
    } else {
        step(p, 1, "Looking for a FAT32 volume");
        struct fs_mount *m = mount_on(disk);
        if (m) {
            target = blk_find(m->dev);
            strlcpy(root, m->path, sizeof(root));
        } else {
            for (int i = 0; i < blk_count() && !target; i++) {
                struct blockdev *d = blk_get(i);
                if (on_disk(d, disk) && fat_probe(d, NULL, 0)) target = d;
            }
            if (!target) return fail(p, "There is no FAT32 volume on this disk; choose \"Erase\" instead");
            if (mount_somewhere(target, root, sizeof(root))) return fail(p, "Could not mount the volume");
        }
        if (part_make_bootable(target)) return fail(p, "Could not update the partition table");
    }

    char path[VFS_PATH_MAX];
    step(p, 2, "Copying the boot loader");
    snprintf(path, sizeof(path), "%s/EFI/BOOT/BOOTX64.EFI", root);
    if (copy_blob(path, boot_info->loader_file_phys, boot_info->loader_file_size)) return fail(p, "Could not write the boot loader");
    step(p, 3, "Copying the kernel");
    snprintf(path, sizeof(path), "%s/zenith/kernel.elf", root);
    if (copy_blob(path, boot_info->kernel_file_phys, boot_info->kernel_file_size)) return fail(p, "Could not write the kernel");
    step(p, 4, "Copying the system image");
    snprintf(path, sizeof(path), "%s/zenith/initrd.tar", root);
    if (copy_blob(path, boot_info->initrd_phys, boot_info->initrd_size)) return fail(p, "Could not write the system image");
    char cfg[64];
    int n = snprintf(cfg, sizeof(cfg), "resolution=%ux%u\n", boot_info->fb.width, boot_info->fb.height);
    snprintf(path, sizeof(path), "%s/zenith/boot.cfg", root);
    vfs_write_file(path, cfg, (size_t)n);

    step(p, 5, "Setting up the account and the settings");
    if (write_settings(o, root, p)) return fail(p, "Could not write the settings");

    step(p, 6, o->gpu_driver ? "Downloading the graphics driver" : "Graphics: UEFI GOP, no driver");
    if (o->gpu_driver) {
        struct gpu_offer offer;
        gpu_find_driver(&offer);
        if (offer.state == GPU_OFFER_AVAILABLE) {
            struct pkg_job *job = kzalloc(sizeof(*job));
            char zdir[96];
            snprintf(zdir, sizeof(zdir), "%s/zenith", root);
            int r = job ? gpu_install_for_target(&offer, zdir, job) : E_NOMEM;
            if (r) snprintf(p->note, sizeof(p->note), "The graphics driver %s could not be installed (%s). It is tried again at the first start.",
                            offer.package, job ? job->error : "out of memory");
            else snprintf(p->note, sizeof(p->note), "Graphics driver %s %s installed for %s.", offer.package, offer.version, offer.adapter);
            kfree(job);
        } else if (offer.state == GPU_OFFER_UNSUPPORTED) {
            snprintf(p->note, sizeof(p->note), "There is no driver for %s in the package sources; the UEFI GOP display is used.", offer.adapter);
        } else if (offer.state == GPU_OFFER_NO_NETWORK) {
            snprintf(p->note, sizeof(p->note), "No network: the graphics driver is downloaded at the first start that has a connection.");
        } else if (offer.state == GPU_OFFER_NO_LIST) {
            snprintf(p->note, sizeof(p->note), "The driver list could not be read; the driver is fetched at the first start instead.");
        }
    }

    step(p, 7, "Making the firmware start ZenithOS");
    if (target->parent) {
        int num = -1;
        int r = uefi_add_boot_entry("ZenithOS", target, "\\EFI\\BOOT\\BOOTX64.EFI", &num);
        if (!r) {
            p->boot_entry = true;
            klog("install: boot entry Boot%04X is first in the boot order", num);
        } else {
            const char *add = " The firmware could not be given a boot entry; it will find the loader on the disk by itself.";
            strlcat(p->note, add, sizeof(p->note));
        }
    }

    step(p, 8, "Writing everything to the disk");
    if (vfs_sync()) return fail(p, "Writing to the disk failed");
    char sz[24];
    blk_format_size(disk->sectors * SECTOR_SIZE, sz, sizeof(sz));
    snprintf(p->msg, sizeof(p->msg), "ZenithOS is installed on %s (%s, %s)", disk->name, disk->model, sz);
    klog("install: %s", p->msg);
    p->pct = 100;
    p->done = true;
    return 0;
}

/* ------------------------------------------------------------------------
 * the app
 * ---------------------------------------------------------------------- */

enum { PG_WELCOME, PG_LANG, PG_ZONE, PG_ACCOUNT, PG_DISK, PG_GRAPHICS, PG_CONFIRM, PG_INSTALL, PG_COUNT };

struct inst {
    int page;
    struct install_opts opts;
    int disk;                           /* index into the blk registry */
    char pass2[64];
    bool user_edited;                   /* the user name was typed, so stop deriving it from the full name */
    char last_full[64];
    char tz_filter[48];
    int tz_scroll, tz_init;
    struct lang_info langs[16];
    int nlangs;
    struct kbd_info kbds[16];
    int nkbds;
    /* graphics */
    struct gpu_offer offer;
    int offer_state;                    /* 0 not asked, 1 asking, 2 answered */
    /* progress */
    struct install_progress prog;
    bool running;
    uint64_t done_at;                   /* when the installation finished (for the restart countdown) */
    bool stay;                          /* the countdown was cancelled */
};

static int worker(void *arg)
{
    struct inst *in = arg;
    zenith_install(&in->opts, &in->prog);
    in->running = false;
    return 0;
}

static int offer_worker(void *arg)
{
    struct inst *in = arg;
    gpu_find_driver(&in->offer);
    in->offer_state = 2;
    return 0;
}

static void describe(struct blockdev *disk, char *out, size_t n, bool *has_fat)
{
    *has_fat = false;
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < blk_count(); i++) {
        struct blockdev *d = blk_get(i);
        if (!on_disk(d, disk)) continue;
        char label[16] = "";
        if (!fat_probe(d, label, sizeof(label))) continue;
        *has_fat = true;
        const char *mp = "";
        for (struct fs_mount *m = vfs_mounts(); m; m = m->next)
            if (!strcmp(m->dev, d->name)) mp = m->path;
        o += (size_t)snprintf(out + o, n - o, "%sFAT32 %s%s%s%s%s", o ? ", " : "", tr("volume"), label[0] ? " " : "",
                              label[0] ? label : d->name, mp[0] ? " @ " : "", mp);
        if (o >= n) break;
    }
    if (!out[0]) {
        bool parts = false;
        for (int i = 0; i < blk_count(); i++) if (blk_get(i)->parent == disk) parts = true;
        strlcpy(out, parts ? tr("Partitions without a FAT32 file system") : tr("Empty disk"), n);
    }
}

static void check(surface_t *s, int x, int y, int state)
{
    /* 0 pending, 1 active, 2 done */
    if (state == 2) {
        gfx_circle(s, (float)x + 11, (float)y + 11, 11, theme.success);
        gfx_line_w(s, (float)x + 6, (float)y + 11.5f, (float)x + 10, (float)y + 15.5f, 2.2f, 0xFFFFFFFFu);
        gfx_line_w(s, (float)x + 10, (float)y + 15.5f, (float)x + 16.5f, (float)y + 7.5f, 2.2f, 0xFFFFFFFFu);
    } else if (state == 1) {
        gfx_ring(s, (float)x + 11, (float)y + 11, 10, 2.5f, theme.accent);
        float a = (float)(uptime_ms() % 1000) / 1000.0f * 6.2831853f;
        gfx_circle(s, (float)x + 11 + 6 * (float)k_cos(a), (float)y + 11 + 6 * (float)k_sin(a), 3, theme.accent);
    } else {
        gfx_ring(s, (float)x + 11, (float)y + 11, 10, 2, theme.surface2);
    }
}

static void radio(surface_t *s, rect_t r, bool on, bool avail)
{
    gfx_ring(s, (float)r.x + 22, (float)r.y + r.h / 2.0f, 8, 2, avail ? theme.text_dim : theme.surface2);
    if (on) gfx_circle(s, (float)r.x + 22, (float)r.y + r.h / 2.0f, 4.5f, theme.accent);
}

static void field(ui_t *u, surface_t *s, int x, int y, int w, const char *label, char *buf, size_t cap, bool password)
{
    gfx_text(s, font_ui, x, y, label, theme.text_dim);
    if (password) ui_password(u, R(x, y + 20, w, 34), buf, cap, "");
    else ui_textbox(u, R(x, y + 20, w, 34), buf, cap, "");
}

/* the first word of the full name, lower case, as a user name */
static void derive_user(const char *full, char *out, size_t n)
{
    size_t o = 0;
    for (const char *c = full; *c && o + 1 < n; c++) {
        unsigned char ch = (unsigned char)*c;
        if (ch == ' ' && o) break;
        if (isupper(ch)) ch = (unsigned char)tolower(ch);
        if (islower(ch) || isdigit(ch) || ch == '_' || ch == '-') out[o++] = (char)ch;
    }
    out[o] = 0;
    if (o && !(islower((unsigned char)out[0]) || out[0] == '_')) out[0] = 0;
}

static bool account_ok(struct inst *in, const char **why)
{
    struct install_opts *o = &in->opts;
    if (!users_valid_name(o->user)) { *why = tr("The user name may use lower-case letters, digits, - and _"); return false; }
    if (!valid_hostname(o->hostname)) { *why = tr("The computer name may use lower-case letters, digits and -"); return false; }
    if (!o->password[0]) { *why = tr("Choose a password"); return false; }
    if (strcmp(o->password, in->pass2)) { *why = tr("The two passwords are not the same"); return false; }
    *why = NULL;
    return true;
}

static void start_install(struct inst *in)
{
    in->opts.disk = blk_get(in->disk);
    in->page = PG_INSTALL;
    in->running = true;
    memset(&in->prog, 0, sizeof(in->prog));
    thread_create("installer", worker, in);
}

static void inst_paint(app_t *a, surface_t *s)
{
    struct inst *in = a->data;
    ui_t *u = &a->ui;
    int W = s->w, H = s->h;
    struct install_opts *o = &in->opts;
    gfx_clear(s, theme.bg);

    /* left banner */
    int bw = 250;
    gfx_vgradient(s, 0, 0, bw, H, color_lerp(theme.accent, HEX(0x101328), 70), HEX(0x151827));
    gfx_circle(s, 40, (float)H - 20, 110, ALPHA(0xFFFFFF, 8));
    logo_draw(s, 28, 32, 64, false);
    gfx_text(s, font_title, 28, 112, tr("Install"), 0xFFFFFFFFu);
    gfx_text(s, font_title, 28, 142, "ZenithOS", 0xFFFFFFFFu);
    static const char *stages[] = { N_("Welcome"), N_("Language"), N_("Time zone"), N_("Account"), N_("Disk"), N_("Graphics"), N_("Install") };
    int stage = in->page >= PG_CONFIRM ? 6 : in->page;
    for (int i = 0; i < 7; i++) {
        int y = 200 + i * 36;
        bool cur = i == stage, past = i < stage;
        gfx_circle(s, 40, (float)y + 9, 6, cur ? 0xFFFFFFFFu : past ? ALPHA(0xFFFFFF, 160) : ALPHA(0xFFFFFF, 50));
        gfx_text(s, cur ? font_ui_bold : font_ui, 58, y, tr(stages[i]), cur ? 0xFFFFFFFFu : ALPHA(0xFFFFFF, past ? 170 : 110));
    }

    int x = bw + 36, y = 30, cw = W - x - 36;
    int by = H - 58;
    bool next_ok = true;
    const char *why = NULL;

    switch (in->page) {
    case PG_WELCOME: {
        gfx_text(s, font_bold_lg, x, y, tr("Put ZenithOS on this computer"), theme.text);
        y += 44;
        gfx_text_wrap(s, font_ui_lg, R(x, y, cw, 80),
                      tr("The installer copies the running system to a disk, sets it up for you and makes the firmware start it. "
                         "Afterwards the computer starts ZenithOS straight from that disk, without the USB stick."),
                      theme.text_dim, 4);
        y += 100;
        static const struct { int icon; const char *t, *d; } pts[] = {
            { ICON_USER, N_("Your account"), N_("Language, keyboard, time zone, name and password") },
            { ICON_DISK, N_("Keep your files, or start fresh"), N_("Add to a FAT32 volume or erase the disk") },
            { ICON_MONITOR, N_("Graphics"), N_("The UEFI display, or the driver for your graphics card") },
        };
        for (int i = 0; i < 3; i++) {
            int yy = y + i * 62;
            gfx_round_rect(s, x, yy, cw, 54, 10, theme.panel);
            icon_draw(s, pts[i].icon, x + 12, yy + 11, 32);
            gfx_text(s, font_ui_bold, x + 58, yy + 9, tr(pts[i].t), theme.text);
            gfx_text(s, font_ui, x + 58, yy + 30, tr(pts[i].d), theme.text_dim);
        }
        if (!blk_count()) {
            gfx_text(s, font_ui_md, x, by + 10, tr("No disks found. ZenithOS supports SATA (AHCI) disks."), theme.warning);
            next_ok = false;
        }
        break;
    }
    case PG_LANG: {
        gfx_text(s, font_bold_lg, x, y, tr("Language and keyboard"), theme.text);
        y += 50;
        int colw = (cw - 16) / 2;
        gfx_text(s, font_ui_bold, x, y, tr("Language"), theme.text);
        if (!in->nlangs) in->nlangs = i18n_languages(in->langs, 16);
        for (int i = 0; i < in->nlangs && i < 9; i++) {
            rect_t r = R(x, y + 26 + i * 36, colw, 32);
            if (ui_list_item(u, r, !strcmp(in->langs[i].id, o->language))) {
                strlcpy(o->language, in->langs[i].id, sizeof(o->language));
                i18n_set(o->language);
                wm_invalidate_all();
            }
            gfx_text(s, font_ui_md, r.x + 12, r.y + 7, in->langs[i].name, theme.text);
        }
        int kx = x + colw + 16;
        gfx_text(s, font_ui_bold, kx, y, tr("Keyboard layout"), theme.text);
        if (!in->nkbds) in->nkbds = keymap_layouts(in->kbds, 16);
        for (int i = 0; i < in->nkbds && i < 9; i++) {
            rect_t r = R(kx, y + 26 + i * 36, colw, 32);
            if (ui_list_item(u, r, !strcmp(in->kbds[i].id, o->keyboard))) {
                strlcpy(o->keyboard, in->kbds[i].id, sizeof(o->keyboard));
                keymap_set(o->keyboard);
            }
            gfx_text(s, font_ui_md, r.x + 12, r.y + 7, in->kbds[i].name, theme.text);
        }
        gfx_text(s, font_ui, x, by - 70, tr("Try the keyboard here:"), theme.text_dim);
        static char try_buf[64];
        ui_textbox(u, R(x, by - 46, cw, 34), try_buf, sizeof(try_buf), "");
        break;
    }
    case PG_ZONE: {
        gfx_text(s, font_bold_lg, x, y, tr("Where are you?"), theme.text);
        y += 46;
        ui_textbox(u, R(x, y, cw, 36), in->tz_filter, sizeof(in->tz_filter), tr("Search a city or country"));
        const struct tz_zone *zones;
        int nz = tz_zones(&zones);
        if (!in->tz_init) {
            in->tz_init = 1;
            for (int i = 0; i < nz; i++)
                if (!strcmp(zones[i].name, o->timezone)) in->tz_scroll = MAX(0, i - 3);
        }
        int rows = 8, shown = 0, matches = 0;
        for (int i = 0; i < nz; i++) {
            if (in->tz_filter[0] && !strstr_ci(zones[i].name, in->tz_filter) && !strstr_ci(zones[i].comment, in->tz_filter) &&
                !strstr_ci(zones[i].cc, in->tz_filter))
                continue;
            if (matches++ < in->tz_scroll || shown >= rows) continue;
            rect_t r = R(x, y + 46 + shown * 36, cw - 14, 32);
            if (ui_list_item(u, r, !strcmp(zones[i].name, o->timezone))) {
                strlcpy(o->timezone, zones[i].name, sizeof(o->timezone));
                tz_set(o->timezone);                           /* the clock in the corner shows it */
            }
            char label[96];
            snprintf(label, sizeof(label), "%s", zones[i].name);
            for (char *c = label; *c; c++) if (*c == '_') *c = ' ';
            gfx_text(s, font_ui_md, r.x + 12, r.y + 7, label, theme.text);
            if (zones[i].comment[0]) gfx_text_right(s, font_ui, r.x + r.w - 10, r.y + 8, zones[i].comment, theme.text_faint);
            shown++;
        }
        rect_t list = R(x, y + 46, cw, rows * 36);
        if (ui_hover(u, list) && u->wheel) {
            in->tz_scroll = CLAMP(in->tz_scroll + (u->wheel > 0 ? 2 : -2), 0, MAX(0, matches - rows));
            u->wheel = 0;
        }
        in->tz_scroll = CLAMP(in->tz_scroll, 0, MAX(0, matches - rows));
        ui_scrollbar(u, R(x + cw - 8, y + 46, 8, rows * 36), &in->tz_scroll, matches, rows);
        bool dst;
        const char *abbr;
        int off = tz_offset(rtc_epoch(), &dst, &abbr);
        char b[96];
        snprintf(b, sizeof(b), "%s: %s  (%s, UTC%+d:%02d)", tr("Selected"), o->timezone, abbr, off / 3600, (off < 0 ? -off : off) / 60 % 60);
        gfx_text_ellipsis(s, font_ui, x, by - 30, cw, b, theme.text_dim);
        break;
    }
    case PG_ACCOUNT: {
        gfx_text(s, font_bold_lg, x, y, tr("Who are you?"), theme.text);
        y += 46;
        int half = (cw - 16) / 2;
        field(u, s, x, y, half, tr("Your name"), o->fullname, sizeof(o->fullname), false);
        /* the user name follows the name until it is edited by hand */
        if (strcmp(in->last_full, o->fullname)) {
            char derived[32];
            derive_user(o->fullname, derived, sizeof(derived));
            char old[32];
            derive_user(in->last_full, old, sizeof(old));
            if (!in->user_edited || !strcmp(o->user, old)) { strlcpy(o->user, derived, sizeof(o->user)); in->user_edited = false; }
            strlcpy(in->last_full, o->fullname, sizeof(in->last_full));
        }
        char before[32];
        strlcpy(before, o->user, sizeof(before));
        field(u, s, x + half + 16, y, half, tr("User name"), o->user, sizeof(o->user), false);
        if (strcmp(before, o->user)) in->user_edited = true;
        y += 64;
        field(u, s, x, y, cw, tr("Computer name"), o->hostname, sizeof(o->hostname), false);
        y += 64;
        field(u, s, x, y, half, tr("Password"), o->password, sizeof(o->password), true);
        field(u, s, x + half + 16, y, half, tr("Repeat the password"), in->pass2, sizeof(in->pass2), true);
        y += 62;
        int len = (int)strlen(o->password);
        ui_progress(u, R(x, y, half, 6), (float)MIN(len, 12) / 12.0f, len < 6 ? theme.warning : theme.success);
        gfx_text(s, font_ui, x, y + 10, len == 0 ? "" : len < 6 ? tr("Short: use a longer password if you can") : tr("Good"),
                 theme.text_faint);
        ui_checkbox(u, R(x, y + 36, cw, 30), tr("Sign in automatically at start"), &o->autologin);
        next_ok = account_ok(in, &why);
        if (why && (o->password[0] || in->pass2[0] || o->fullname[0])) gfx_text(s, font_ui, x, by - 22, why, theme.warning);
        break;
    }
    case PG_DISK: {
        gfx_text(s, font_bold_lg, x, y, tr("Where should ZenithOS go?"), theme.text);
        y += 46;
        int shown = 0;
        bool sel_fat = false;
        for (int i = 0; i < blk_count(); i++) {
            struct blockdev *d = blk_get(i);
            if (d->parent) continue;
            if (in->disk < 0) in->disk = i;
            rect_t r = R(x, y + shown * 70, cw, 62);
            bool sel = in->disk == i;
            if (ui_list_item(u, r, sel)) in->disk = i;
            gfx_round_outline(s, r.x, r.y, r.w, r.h, 10, sel ? theme.accent : ALPHA(0xFFFFFF, 16));
            icon_draw(s, ICON_DISK, r.x + 14, r.y + 10, 40);
            char t[96], sz[24], desc[160];
            bool has_fat;
            blk_format_size(d->sectors * SECTOR_SIZE, sz, sizeof(sz));
            snprintf(t, sizeof(t), "%s  ·  %s  ·  %s", d->name, d->model, sz);
            describe(d, desc, sizeof(desc), &has_fat);
            if (sel) sel_fat = has_fat;
            gfx_text_ellipsis(s, font_ui_bold, r.x + 66, r.y + 10, r.w - 80, t, theme.text);
            gfx_text_ellipsis(s, font_ui, r.x + 66, r.y + 34, r.w - 80, desc, theme.text_dim);
            shown++;
        }
        y += shown * 70 + 12;
        if (!sel_fat) o->erase = true;
        for (int k = 0; k < 2; k++) {
            bool erase = k == 1;
            rect_t r = R(x, y + k * 46, cw, 40);
            bool avail = erase || sel_fat;
            if (ui_list_item(u, r, o->erase == erase) && avail) o->erase = erase;
            radio(s, r, o->erase == erase, avail);
            gfx_text(s, font_ui_md, r.x + 42, r.y + 11,
                     erase ? tr("Erase the disk and install") : tr("Install next to the files on the FAT32 volume"),
                     avail ? (erase ? theme.danger : theme.text) : theme.text_faint);
        }
        next_ok = in->disk >= 0;
        break;
    }
    case PG_GRAPHICS: {
        gfx_text(s, font_bold_lg, x, y, tr("Graphics"), theme.text);
        y += 46;
        if (!in->offer_state) {
            in->offer_state = 1;
            thread_create("gpu-offer", offer_worker, in);
        }
        char info[200];
        if (in->offer_state == 1) snprintf(info, sizeof(info), "%s", tr("Looking for a driver for your graphics card ..."));
        else switch (in->offer.state) {
        case GPU_OFFER_AVAILABLE:
            snprintf(info, sizeof(info), "%s: %s %s", in->offer.adapter, in->offer.package, in->offer.version);
            break;
        case GPU_OFFER_UNSUPPORTED: snprintf(info, sizeof(info), "%s: %s", in->offer.adapter, tr("no driver available yet")); break;
        case GPU_OFFER_NO_NETWORK: snprintf(info, sizeof(info), "%s: %s", in->offer.adapter, tr("no network, the driver cannot be looked up now")); break;
        case GPU_OFFER_NO_LIST: snprintf(info, sizeof(info), "%s: %s", in->offer.adapter, tr("the driver list could not be read")); break;
        default: snprintf(info, sizeof(info), "%s", tr("No graphics adapter found")); break;
        }
        gfx_round_rect(s, x, y, cw, 54, 10, theme.panel);
        icon_draw(s, ICON_MONITOR, x + 12, y + 11, 32);
        gfx_text(s, font_ui_bold, x + 58, y + 9, tr("This computer's graphics adapter"), theme.text);
        gfx_text_ellipsis(s, font_ui, x + 58, y + 30, cw - 70, info, in->offer_state == 1 ? theme.text_dim : theme.text_dim);
        y += 72;
        static const struct { bool drv; const char *t, *d; } ch[] = {
            { true, N_("Use the graphics driver if there is one"), N_("Full resolution and the monitor's full refresh rate. Downloaded once now and kept on the disk. Falls back to UEFI GOP if there is no driver.") },
            { false, N_("Use the UEFI display (GOP)"), N_("Works on every computer; the resolution is what the firmware chose.") },
        };
        for (int k = 0; k < 2; k++) {
            rect_t r = R(x, y + k * 92, cw, 84);
            if (ui_list_item(u, r, o->gpu_driver == ch[k].drv)) o->gpu_driver = ch[k].drv;
            gfx_round_outline(s, r.x, r.y, r.w, r.h, 10, o->gpu_driver == ch[k].drv ? theme.accent : ALPHA(0xFFFFFF, 16));
            gfx_ring(s, (float)r.x + 22, (float)r.y + 24, 8, 2, theme.text_dim);
            if (o->gpu_driver == ch[k].drv) gfx_circle(s, (float)r.x + 22, (float)r.y + 24, 4.5f, theme.accent);
            gfx_text(s, font_ui_md, r.x + 42, r.y + 13, tr(ch[k].t), theme.text);
            gfx_text_wrap(s, font_ui, R(r.x + 42, r.y + 38, r.w - 56, 40), tr(ch[k].d), theme.text_dim, 2);
        }
        if (in->offer_state == 1) u->want_repaint = true;
        break;
    }
    case PG_CONFIRM: {
        struct blockdev *d = blk_get(in->disk);
        gfx_text(s, font_bold_lg, x, y, tr("Ready to install"), theme.text);
        y += 46;
        char sz[24], t[200];
        blk_format_size(d->sectors * SECTOR_SIZE, sz, sizeof(sz));
        struct lang_info *lg = NULL;
        for (int i = 0; i < in->nlangs; i++) if (!strcmp(in->langs[i].id, o->language)) lg = &in->langs[i];
        static const char *keys[] = { N_("Language"), N_("Keyboard"), N_("Time zone"), N_("Account"), N_("Computer name"), N_("Disk"), N_("Graphics") };
        char vals[7][120];
        snprintf(vals[0], 120, "%s", lg ? lg->name : o->language);
        snprintf(vals[1], 120, "%s", o->keyboard);
        snprintf(vals[2], 120, "%s", o->timezone);
        snprintf(vals[3], 120, "%s (%s)%s", o->fullname[0] ? o->fullname : o->user, o->user, o->autologin ? tr(", signs in automatically") : "");
        snprintf(vals[4], 120, "%s", o->hostname);
        snprintf(vals[5], 120, "%s  ·  %s  ·  %s", d->name, d->model, sz);
        snprintf(vals[6], 120, "%s", o->gpu_driver ? tr("Graphics driver if available, else UEFI GOP") : tr("UEFI display (GOP)"));
        for (int i = 0; i < 7; i++) {
            gfx_text(s, font_ui, x, y + i * 30, tr(keys[i]), theme.text_dim);
            gfx_text_ellipsis(s, font_ui_md, x + 150, y + i * 30 - 1, cw - 150, vals[i], theme.text);
        }
        y += 7 * 30 + 10;
        if (o->erase) {
            gfx_round_rect(s, x, y, cw, 68, 12, color_lerp(theme.danger, theme.bg, 200));
            gfx_round_outline(s, x, y, cw, 68, 12, ALPHA(0xFF5C7A, 90));
            snprintf(t, sizeof(t), tr("Everything on %s will be deleted. It gets a new partition table with one FAT32 EFI System Partition."), d->name);
            gfx_text_wrap(s, font_ui, R(x + 16, y + 12, cw - 32, 48), t, theme.text, 2);
        } else {
            gfx_text_wrap(s, font_ui, R(x, y, cw, 48), tr("Your files on the FAT32 volume are kept; ZenithOS is added next to them."), theme.text_dim, 2);
        }
        break;
    }
    case PG_INSTALL: {
        struct install_progress *p = &in->prog;
        const char *title = !p->done ? tr("Installing...") : p->failed ? tr("Installation failed") : tr("All done!");
        gfx_text(s, font_bold_lg, x, y, title, theme.text);
        y += 46;
        static const char *names[] = { "", N_("Preparing the disk"), N_("Boot loader"), N_("Kernel"), N_("System image"),
                                       N_("Account and settings"), N_("Graphics driver"), N_("Firmware boot entry"), N_("Writing to disk") };
        for (int i = 1; i <= INSTALL_STEPS; i++) {
            int yy = y + (i - 1) * 30;
            int st = p->done && !p->failed ? 2 : i < p->step ? 2 : i == p->step ? (p->failed ? 0 : 1) : 0;
            check(s, x, yy, st);
            gfx_text(s, st == 1 ? font_ui_bold : font_ui_md, x + 34, yy + 2, tr(names[i]), st ? theme.text : theme.text_faint);
        }
        y += INSTALL_STEPS * 30 + 8;
        gfx_round_rect(s, x, y, cw, 8, 4, theme.surface2);
        gfx_round_rect(s, x, y, MAX(8, cw * p->pct / 100), 8, 4, p->failed ? theme.danger : theme.accent);
        gfx_text_ellipsis(s, font_ui, x, y + 16, cw, tr(p->msg), p->failed ? theme.danger : theme.text_dim);
        if (p->note[0]) gfx_text_wrap(s, font_ui, R(x, y + 38, cw, 40), p->note, theme.text_dim, 2);
        if (p->done && !p->failed) {
            if (!in->done_at) in->done_at = uptime_ms();
            int left = in->stay ? -1 : 10 - (int)((uptime_ms() - in->done_at) / 1000);
            char t[120];
            if (left >= 0) snprintf(t, sizeof(t), tr("Restarting into ZenithOS in %d s. The installation medium can be removed."), left);
            else snprintf(t, sizeof(t), "%s", tr("Remove the installation medium, then restart."));
            gfx_text(s, font_ui, x, by - 26, t, theme.text_faint);
            if (ui_button(u, R(W - 36 - 150, by, 150, 40), tr("Restart now"), BTN_PRIMARY) || left == 0) app_launch("@reboot");
            if (left >= 0 && ui_button(u, R(W - 36 - 150 - 120, by, 110, 40), tr("Stay"), BTN_NORMAL)) in->stay = true;
            if (left >= 0) u->want_repaint = true;
        } else if (p->done) {
            if (ui_button(u, R(x, by, 110, 40), tr("Back"), BTN_NORMAL)) { in->page = PG_CONFIRM; memset(p, 0, sizeof(*p)); }
        }
        if (!p->done) u->want_repaint = true;
        return;
    }
    }

    /* navigation */
    if (in->page > PG_WELCOME && ui_button(u, R(x, by, 110, 40), tr("Back"), BTN_NORMAL)) in->page--;
    if (in->page == PG_CONFIRM) {
        if (ui_button(u, R(W - 36 - 190, by, 190, 40), o->erase ? tr("Erase and install") : tr("Install"), o->erase ? BTN_DANGER : BTN_PRIMARY))
            start_install(in);
    } else if (next_ok) {
        if (ui_button(u, R(W - 36 - 130, by, 130, 40), tr("Next"), BTN_PRIMARY)) in->page++;
    } else {
        ui_button(u, R(W - 36 - 130, by, 130, 40), tr("Next"), BTN_SUBTLE);
    }
}

static void inst_close(app_t *a)
{
    struct inst *in = a->data;
    if (in->running) return;        /* never leave a half-written disk behind */
    a->quit = true;
}

int installer_main(void *arg)
{
    UNUSED(arg);
    struct inst *in = kzalloc(sizeof(*in));
    in->disk = -1;
    install_defaults(&in->opts);
    app_t a = { 0 };
    a.data = in;
    a.win = wm_create(tr("Install ZenithOS"), 900, 600, WF_CENTER);
    if (!a.win) { kfree(in); return 1; }
    wm_set_icon(a.win, ICON_INSTALL);
    a.on_paint = inst_paint;
    a.on_close = inst_close;
    int r = app_run(&a);
    while (in->running) sched_sleep(50);
    kfree(in);
    return r;
}
