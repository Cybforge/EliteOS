/*
 * Graphics adapters: the registry drivers announce themselves to, the choice
 * of a display mode from the monitor's EDID, and the thread that finds,
 * downloads and loads the driver package for the adapter in this computer.
 *
 * Nothing here knows a particular adapter. Which package drives which
 * hardware is the package index's business (Match-PCI), which mode to use is
 * the monitor's (EDID).
 */
#include <kernel.h>
#include <mm.h>
#include <vfs.h>
#include <net.h>
#include <sys.h>
#include <sched.h>
#include <module.h>
#include <pkg.h>
#include <gpu.h>
#include <wm.h>
#include <dev.h>

extern struct bootinfo *boot_info;

static struct gpu *gpus;
static struct gpu *active_gpu;
static struct edid_info monitor;
static struct gpu_fb early_fb;                  /* set when a driver took over before the window manager started */
static bool early_valid;
static char state[120] = "";
static char package[48];
static bool repo_supported;
static mutex_t gpu_mtx = MUTEX_INIT("gpu");

/* A driver that hangs or crashes the machine must not do it on every boot. The marker is written (and flushed to the
 * disk) before a driver starts and removed once the desktop has been running with it for a while; if it is still
 * there at the next boot, drivers are switched off until the user turns them on again in the settings. */
#define GPU_TRYING   "/var/lib/zenith/gpu.trying"
#define GPU_DISABLED "/var/lib/zenith/gpu.disabled"

static void mark_trying(const char *what)
{
    sys_mkdir_p("/var/lib/zenith");
    vfs_write_file(GPU_TRYING, what, strlen(what));
    vfs_sync();
}

bool gpu_disabled(void) { return vfs_lookup(GPU_DISABLED) != NULL; }

void gpu_reenable(void)
{
    vfs_unlink(GPU_DISABLED, false);
    vfs_unlink(GPU_TRYING, false);
    vfs_sync();
}

const char *gpu_policy(void)
{
    const char *p = conf_get(&sysconf, "graphics", "auto");
    return !strcmp(p, "gop") || !strcmp(p, "driver") ? p : "auto";
}

void gpu_set_policy(const char *p)
{
    if (strcmp(p, "gop") && strcmp(p, "driver") && strcmp(p, "auto")) return;
    conf_set(&sysconf, "graphics", p);
    size_t n;
    char *t = conf_format(&sysconf, &n);
    if (t) { sys_write_etc("zenith/system.conf", t, n); kfree(t); }
}

static void set_state(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state, sizeof(state), fmt, ap);
    va_end(ap);
    klog("gpu: %s", state);
}

/* ---- picking and applying a mode ---- */

static bool wanted_mode(const struct edid_info *e, const struct gpu *g, struct gpu_timing *out)
{
    /* the user's choice from the settings ("1920x1080@144") wins if the monitor offers it */
    const char *want = conf_get(&sysconf, "display_mode", "");
    if (*want) {
        char *end;
        long w = strtol(want, &end, 10), h = 0, hz = 0;
        if (*end == 'x') h = strtol(end + 1, &end, 10);
        if (*end == '@') hz = strtol(end + 1, &end, 10);
        for (int i = 0; i < e->nmodes; i++) {
            const struct gpu_timing *t = &e->modes[i];
            if (t->hactive == w && t->vactive == h && (t->refresh_mhz + 500) / 1000 == (uint32_t)hz) {
                *out = *t;
                return true;
            }
        }
    }
    return gpu_pick_mode(e, g, out);
}

static void present_cb(int x, int y, int w, int h)
{
    if (active_gpu && active_gpu->ops->present) active_gpu->ops->present(active_gpu, x, y, w, h);
}

static int apply(struct gpu *g, const struct gpu_timing *t)
{
    struct gpu_fb fb;
    memset(&fb, 0, sizeof(fb));
    int r = g->ops->set_mode(g, t, &fb);
    if (r) return r;
    g->cur = *t;
    g->active = true;
    active_gpu = g;
    if (wm_ready) {
        wm_set_framebuffer(fb.vaddr, fb.w, fb.h, fb.pitch, fb.swap, g->ops->present ? present_cb : NULL);
    } else {
        bootcon_enable(false);
        early_fb = fb;
        early_valid = true;
    }
    wm_set_refresh(t->refresh_mhz);
    klog("gpu: %s drives %dx%d at %u.%03u Hz (pixel clock %u kHz)", g->name, fb.w, fb.h, t->refresh_mhz / 1000,
         t->refresh_mhz % 1000, t->pclk_khz);
    return 0;
}

static int activate(struct gpu *g)
{
    uint8_t raw[512];
    int n = 0;
    bool ok = false;
    if (g->ops->read_edid) n = g->ops->read_edid(g, raw, sizeof(raw));
    if (n >= 128) ok = edid_parse(raw, (size_t)n, &monitor);
    if (!ok && boot_info->edid_size >= 128) {                  /* what the firmware read when it started the screen */
        memcpy(raw, boot_info->edid, boot_info->edid_size);
        ok = edid_parse(raw, boot_info->edid_size, &monitor);
    }
    struct gpu_timing t;
    if (ok) {
        char list[200];
        int o = 0;
        for (int i = 0; i < monitor.nmodes && o < (int)sizeof(list) - 20; i++)
            o += snprintf(list + o, sizeof(list) - (size_t)o, "%dx%d@%u ", monitor.modes[i].hactive, monitor.modes[i].vactive,
                          (monitor.modes[i].refresh_mhz + 500) / 1000);
        klog("gpu: the monitor offers %s", list);
    }
    if (ok && wanted_mode(&monitor, g, &t)) {
        klog("gpu: monitor %s %s, %d modes, using %dx%d@%u", monitor.mfg, monitor.name, monitor.nmodes, t.hactive,
             t.vactive, (t.refresh_mhz + 500) / 1000);
    } else {
        /* no usable EDID: keep the size the firmware chose, at the rate every monitor takes */
        gpu_cvt_rb((int)boot_info->fb.width, (int)boot_info->fb.height, 60, &t);
        klog("gpu: no EDID, keeping %dx%d at 60 Hz", t.hactive, t.vactive);
    }
    return apply(g, &t);
}

bool gpu_early_display(struct gpu_fb *out)
{
    if (!early_valid) return false;
    *out = early_fb;
    return true;
}

int gpu_register(struct gpu *g)
{
    if (!g || !g->ops || !g->ops->set_mode) return E_INVAL;
    mutex_lock(&gpu_mtx);
    g->next = gpus;
    gpus = g;
    mutex_unlock(&gpu_mtx);
    klog("gpu: adapter %s registered by %s", g->name, g->driver);
    return activate(g);
}

void gpu_unregister(struct gpu *g)
{
    mutex_lock(&gpu_mtx);
    for (struct gpu **pp = &gpus; *pp; pp = &(*pp)->next)
        if (*pp == g) { *pp = g->next; break; }
    if (active_gpu == g) active_gpu = NULL;
    mutex_unlock(&gpu_mtx);
}

int gpu_modes(struct gpu_timing *out, int max)
{
    int n = 0;
    if (!active_gpu) return 0;
    for (int i = 0; i < monitor.nmodes && n < max; i++) {
        const struct gpu_timing *t = &monitor.modes[i];
        if (active_gpu->max_w && t->hactive > active_gpu->max_w) continue;
        if (active_gpu->max_h && t->vactive > active_gpu->max_h) continue;
        if (active_gpu->max_pclk_khz && t->pclk_khz > active_gpu->max_pclk_khz) continue;
        out[n++] = *t;
    }
    /* largest first, fastest first */
    for (int i = 1; i < n; i++) {
        struct gpu_timing k = out[i];
        int j = i - 1;
        long ka = (long)k.hactive * k.vactive;
        while (j >= 0 && ((long)out[j].hactive * out[j].vactive < ka ||
                          ((long)out[j].hactive * out[j].vactive == ka && out[j].refresh_mhz < k.refresh_mhz))) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = k;
    }
    return n;
}

int gpu_apply_mode(const struct gpu_timing *t)
{
    if (!active_gpu) return E_NOENT;
    return apply(active_gpu, t);
}

/* ---- finding and loading drivers ---- */

static bool has_gpu_for(const struct pci_dev *d)
{
    for (struct gpu *g = gpus; g; g = g->next)
        if (g->pci == d) return true;
    return false;
}

static void adapter_name(const struct pci_dev *d, char *out, size_t n)
{
    const char *v = pci_vendor_name(d->vendor), *m = pci_device_name(d->vendor, d->device);
    snprintf(out, n, "%s %s", v ? v : "", m ? m : "");
    if (out[0] == ' ' || !out[1]) snprintf(out, n, "%04x:%04x", d->vendor, d->device);
}

static int load_package_module(const struct pkg *p)
{
    char path[VFS_PATH_MAX], err[120] = "";
    snprintf(path, sizeof(path), "/opt/%s/%s", p->name, p->module);
    size_t n;
    void *img = vfs_read_file(path, &n);
    if (!img) { set_state("%s: the driver file %s is missing", p->name, p->module); return E_NOENT; }
    int r = module_load(img, n, p->name, err, sizeof(err));
    kfree(img);
    if (r) set_state("%s: loading the driver failed: %s", p->name, err);
    return r;
}

/* the display adapters on the PCI bus that no driver has taken yet */
static int free_adapters(struct pci_dev **out, int max)
{
    int n = 0;
    for (int i = 0; i < pci_count() && n < max; i++) {
        struct pci_dev *d = pci_get(i);
        if (d->cls == 0x03 && !has_gpu_for(d)) out[n++] = d;
    }
    return n;
}

void gpu_boot_local(void)
{
    if (!strcmp(gpu_policy(), "gop")) { set_state("UEFI GOP, as chosen"); return; }
    if (vfs_lookup(GPU_TRYING) && !gpu_disabled()) {
        vfs_write_file(GPU_DISABLED, "the last start of the graphics driver did not finish\n", 53);
        vfs_unlink(GPU_TRYING, false);
        vfs_sync();
    }
    if (gpu_disabled()) {
        set_state("the graphics driver did not start properly last time: using UEFI GOP until it is turned on again in Settings");
        return;
    }
    struct pci_dev *ad[8];
    int na = free_adapters(ad, 8);
    const struct pkg *inst[64];
    int ni = pkg_installed_list(inst, 64);
    for (int a = 0; a < na; a++) {
        uint32_t id = (uint32_t)ad[a]->vendor << 16 | ad[a]->device;
        for (int i = 0; i < ni; i++) {
            if (strcmp(inst[i]->type, "driver") || !inst[i]->module[0]) continue;
            for (int k = 0; k < inst[i]->npci; k++) {
                if (inst[i]->pci[k] != id) continue;
                snprintf(package, sizeof(package), "%s", inst[i]->name);
                mark_trying(inst[i]->name);
                if (!load_package_module(inst[i]) && active_gpu) set_state("driver %s loaded from the installed system", inst[i]->name);
            }
        }
    }
}

static int gpud(void *arg)
{
    UNUSED(arg);
    if (!strcmp(gpu_policy(), "gop")) return 0;
    if (active_gpu) {                                         /* started from the installed system: the desktop is up with it */
        sched_sleep(20000);
        vfs_unlink(GPU_TRYING, false);
    }
    if (gpu_disabled()) return 0;
    struct pci_dev *ad[8];
    int na = free_adapters(ad, 8);
    if (!na) {
        if (!active_gpu && !state[0]) set_state("no display adapter found");
        return 0;
    }
    /* the network comes up on its own schedule */
    struct net_info ni;
    for (int waited = 0; waited < 40; waited++) {
        if (net_get_info(&ni) && ni.ip) break;
        set_state("waiting for the network to look for a graphics driver");
        sched_sleep(1000);
    }
    if (!net_get_info(&ni) || !ni.ip) {
        set_state("no network: staying on the firmware's UEFI GOP display");
        return 0;
    }
    struct pkg_job *job = kzalloc(sizeof(*job));
    if (!job) return 0;
    char name[64];
    adapter_name(ad[0], name, sizeof(name));
    set_state("reading the driver list for %s", name);
    if (pkg_update(job) <= 0) {
        set_state("the driver list could not be read: staying on UEFI GOP");
        kfree(job);
        return 0;
    }
    for (int a = 0; a < na; a++) {
        adapter_name(ad[a], name, sizeof(name));
        int idx[4];
        int n = pkg_for_pci(ad[a]->vendor, ad[a]->device, idx, 4);
        if (!n) {
            set_state("no driver for %s in the repository: staying on UEFI GOP", name);
            continue;
        }
        repo_supported = true;
        const struct pkg *p = pkg_at(idx[0]);
        snprintf(package, sizeof(package), "%s", p->name);
        if (!p->verified) { set_state("the driver package %s is not signed by a trusted key", p->name); continue; }
        if (!pkg_is_installed(p->name)) {
            set_state("downloading the driver for %s (%s %s)", name, p->name, p->version);
            memset(job, 0, sizeof(*job));
            if (pkg_install(p->name, job)) {
                set_state("downloading the driver failed: %s", job->error);
                continue;
            }
            p = pkg_lookup(p->name);
        }
        set_state("starting the driver for %s", name);
        mark_trying(p->name);
        if (!load_package_module(p) && active_gpu) {
            set_state("%s active", p->name);
            sched_sleep(20000);
            vfs_unlink(GPU_TRYING, false);
        } else {
            vfs_unlink(GPU_TRYING, false);                    /* it failed cleanly: nothing to be afraid of at the next boot */
        }
    }
    kfree(job);
    return 0;
}

void gpu_find_driver(struct gpu_offer *o)
{
    memset(o, 0, sizeof(*o));
    struct pci_dev *ad[8];
    int na = 0;
    for (int i = 0; i < pci_count() && na < 8; i++)
        if (pci_get(i)->cls == 0x03) ad[na++] = pci_get(i);
    if (!na) { o->state = GPU_OFFER_NO_ADAPTER; return; }
    adapter_name(ad[0], o->adapter, sizeof(o->adapter));
    struct net_info ni;
    for (int waited = 0; waited < 25 && !(net_get_info(&ni) && ni.ip); waited++) sched_sleep(1000);
    if (!net_get_info(&ni) || !ni.ip) { o->state = GPU_OFFER_NO_NETWORK; return; }
    struct pkg_job *job = kzalloc(sizeof(*job));
    if (!job) { o->state = GPU_OFFER_NO_LIST; return; }
    int known = pkg_update(job);
    kfree(job);
    if (known <= 0) { o->state = GPU_OFFER_NO_LIST; return; }
    o->state = GPU_OFFER_UNSUPPORTED;
    for (int a = 0; a < na; a++) {
        int idx[4];
        if (!pkg_for_pci(ad[a]->vendor, ad[a]->device, idx, 4)) continue;
        const struct pkg *p = pkg_at(idx[0]);
        if (!p->verified) continue;
        adapter_name(ad[a], o->adapter, sizeof(o->adapter));
        strlcpy(o->package, p->name, sizeof(o->package));
        strlcpy(o->version, p->version, sizeof(o->version));
        o->state = GPU_OFFER_AVAILABLE;
        return;
    }
}

/* download the offered driver onto another volume's /opt (the installer); it is loaded from there at every boot */
int gpu_install_for_target(const struct gpu_offer *o, const char *target_zenith_dir, struct pkg_job *job)
{
    if (o->state != GPU_OFFER_AVAILABLE) return E_NOENT;
    pkg_set_root(target_zenith_dir);
    int r = pkg_install(o->package, job);
    pkg_set_root("");
    return r;
}

void gpu_start(void)
{
    thread_create("gpud", gpud, NULL);
}

void gpu_get_status(struct gpu_status *s)
{
    memset(s, 0, sizeof(*s));
    struct pci_dev *d = NULL;
    for (int i = 0; i < pci_count(); i++) {
        struct pci_dev *x = pci_get(i);
        if (x->cls == 0x03) { d = x; if (active_gpu && x == active_gpu->pci) break; }
    }
    if (d) adapter_name(d, s->adapter, sizeof(s->adapter));
    else strlcpy(s->adapter, "-", sizeof(s->adapter));
    snprintf(s->mode_policy, sizeof(s->mode_policy), "%s", gpu_policy());
    strlcpy(s->state, state, sizeof(s->state));
    strlcpy(s->package, package, sizeof(s->package));
    s->supported = repo_supported;
    if (active_gpu) {
        strlcpy(s->driver, active_gpu->driver, sizeof(s->driver));
        s->accelerated = true;
        s->w = active_gpu->cur.hactive;
        s->h = active_gpu->cur.vactive;
        s->refresh_mhz = active_gpu->cur.refresh_mhz;
    } else {
        strlcpy(s->driver, "UEFI GOP", sizeof(s->driver));
        s->w = wm_screen_w();
        s->h = wm_screen_h();
    }
    snprintf(s->monitor, sizeof(s->monitor), "%s%s%s", monitor.mfg, monitor.name[0] ? " " : "", monitor.name);
    s->nmodes = monitor.nmodes;
}
