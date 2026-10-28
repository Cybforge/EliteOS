#ifndef ZENITH_GPU_H
#define ZENITH_GPU_H

/*
 * Graphics adapters. The firmware leaves the screen in a UEFI GOP mode; a
 * graphics driver (a loadable module, see module.h) registers a `struct gpu`,
 * and this layer reads the monitor's EDID, picks the best mode it supports
 * (native resolution, highest refresh rate), has the driver program it and
 * hands the new framebuffer to the window manager.
 */
#include <kernel.h>
#include <dev.h>

/* one display mode with its full signal timing */
struct gpu_timing {
    uint32_t pclk_khz;
    uint16_t hactive, hfront, hsync, hback;
    uint16_t vactive, vfront, vsync, vback;
    bool hsync_pos, vsync_pos;
    uint32_t refresh_mhz;               /* milli-Hertz: 144000 = 144 Hz */
    bool from_edid_timing;              /* exact timing from the monitor, not computed */
};

static inline int gpu_htotal(const struct gpu_timing *t) { return t->hactive + t->hfront + t->hsync + t->hback; }
static inline int gpu_vtotal(const struct gpu_timing *t) { return t->vactive + t->vfront + t->vsync + t->vback; }

/* a scanout buffer the driver set up */
struct gpu_fb {
    void *vaddr;                        /* write-combined mapping */
    uint64_t phys;
    int w, h;
    int pitch;                          /* bytes per line */
    bool swap;                          /* byte order is R,G,B instead of B,G,R */
};

struct gpu;

struct gpu_ops {
    /* program the display engine for this timing and report the framebuffer to draw into */
    int  (*set_mode)(struct gpu *g, const struct gpu_timing *t, struct gpu_fb *out);
    /* read the monitor's EDID over the adapter's DDC/AUX channel; returns the number of bytes, 0 if none */
    int  (*read_edid)(struct gpu *g, uint8_t *buf, size_t max);
    /* make a rectangle of the framebuffer visible (adapters that do not scan out of it directly) */
    void (*present)(struct gpu *g, int x, int y, int w, int h);
    void (*shutdown)(struct gpu *g);
};

struct gpu {
    char name[48];
    uint16_t vendor, device;
    struct pci_dev *pci;
    const struct gpu_ops *ops;
    void *priv;
    int max_w, max_h;                   /* largest mode the adapter can scan out (0: no limit) */
    uint32_t max_pclk_khz;              /* pixel clock limit (0: no limit) */
    char driver[32];                    /* module that registered it */
    /* set by the framework */
    bool active;
    struct gpu_timing cur;
    struct gpu *next;
};

/* ---- monitors ---- */

#define EDID_MAX_MODES 96

struct edid_info {
    bool valid;
    char mfg[4];
    char name[20];
    uint16_t product;
    int width_mm, height_mm;
    int vmin, vmax;                     /* vertical refresh range in Hz from the range descriptor, 0 if absent */
    int hmin, hmax;                     /* horizontal, kHz */
    int max_pclk_mhz;
    int nmodes;
    int preferred;                      /* index of the monitor's preferred (native) mode, -1 if unknown */
    struct gpu_timing modes[EDID_MAX_MODES];
};

bool edid_parse(const uint8_t *e, size_t n, struct edid_info *out);
void gpu_cvt_rb(int w, int h, int refresh_hz, struct gpu_timing *t);       /* CVT reduced blanking v1 */
bool gpu_pick_mode(const struct edid_info *e, const struct gpu *g, struct gpu_timing *out);

/* ---- registry (used by driver modules) ---- */

int  gpu_register(struct gpu *g);               /* the adapter is ready; the framework switches to it */
void gpu_unregister(struct gpu *g);

/* ---- policy and status ---- */

struct gpu_status {
    char adapter[64];                   /* what the PCI bus says */
    char driver[32];                    /* module in use, or "UEFI GOP" */
    char package[48];
    char monitor[24];
    char state[120];                    /* what the driver logic is doing, in words */
    char mode_policy[12];               /* "auto", "gop" or "driver" */
    int w, h;
    uint32_t refresh_mhz;
    bool accelerated;                   /* a native driver is active */
    bool supported;                     /* the repository has a driver for the adapter */
    int nmodes;
};

bool gpu_early_display(struct gpu_fb *out);     /* the framebuffer a driver set up before the window manager started */
void gpu_boot_local(void);                      /* before the window manager: load drivers that are already installed */
void gpu_start(void);                           /* after the network is up: fetch a driver if there is one for this adapter */
void gpu_get_status(struct gpu_status *out);
int  gpu_modes(struct gpu_timing *out, int max);     /* what the monitor offers, best first */
int  gpu_apply_mode(const struct gpu_timing *t);     /* switch to one of them */
bool gpu_disabled(void);                        /* drivers are off because one failed to start */
void gpu_reenable(void);
const char *gpu_policy(void);
void gpu_set_policy(const char *p);             /* "auto", "gop", "driver" - saved in system.conf */

/* the window manager's side */
bool wm_set_framebuffer(void *fb, int w, int h, int pitch, bool swap, void (*present)(int, int, int, int));
void wm_set_refresh(uint32_t refresh_mhz);

#endif
