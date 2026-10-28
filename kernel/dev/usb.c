/*
 * USB: xHCI host controller driver with USB 2.0 hub support, HID
 * keyboards, mice and tablets, and bulk/interrupt endpoints for class
 * drivers outside this file (USB networking). Everything runs on one
 * polling thread, so no interrupt routing is needed; only endpoints that
 * other threads submit transfers to have a lock.
 */
#include <kernel.h>
#include <dev.h>
#include <mm.h>
#include <sched.h>
#include <input.h>
#include <spinlock.h>
#include <usb.h>
#include <x86.h>

/* ------------------------------------------------------------------------
 * register layout
 * ---------------------------------------------------------------------- */

#define OP_USBCMD  0x00
#define OP_USBSTS  0x04
#define OP_CRCR    0x18
#define OP_DCBAAP  0x30
#define OP_CONFIG  0x38
#define OP_PORTSC(p) (0x400 + 0x10 * ((p) - 1))

#define PORT_CCS   (1u << 0)
#define PORT_PED   (1u << 1)
#define PORT_PR    (1u << 4)
#define PORT_PP    (1u << 9)
#define PORT_CHANGE_BITS (0x7Fu << 17)

#define TRB_NORMAL     1
#define TRB_SETUP      2
#define TRB_DATA       3
#define TRB_STATUS     4
#define TRB_LINK       6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDRESS_DEV 11
#define TRB_CONFIG_EP  12
#define TRB_EVAL_CTX   13
#define TRB_EV_TRANSFER 32
#define TRB_EV_CMD     33
#define TRB_EV_PORT    34

#define RING_TRBS 256

struct trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
};

struct ring {
    struct trb *t;
    uint64_t phys;
    unsigned enq;
    uint32_t cycle;
};

struct xhci;

/* ------------------------------------------------------------------------
 * HID report layout (from the report descriptor)
 * ---------------------------------------------------------------------- */

struct hid_layout {
    int report_id;
    int btn_off, btn_count;
    int x_off, x_size, y_off, y_size, w_off, w_size;
    bool absolute;
    int32_t xmax, ymax;
    bool valid;
};

enum { F_NONE, F_KEYBOARD, F_POINTER };

struct usb_func {
    int kind;
    bool boot;              /* interface supports the boot protocol */
    int iface;
    int dci;                /* device context index of the interrupt IN endpoint */
    int ep_addr, mps, interval;
    int report_len;
    struct ring ring;
    uint8_t *buf;           /* 4 x 64-byte report buffers */
    uint64_t buf_phys;
    struct hid_layout lay;
    uint8_t prev[8];
    int prev_buttons;
    int repeat_key;
    uint64_t repeat_at;
};

/* an endpoint opened for a class driver */
struct usb_ep {
    struct usb_dev *d;
    int dci, mps;
    bool in;
    struct ring ring;
    spinlock_t lock;
    int outstanding;
    uintptr_t tags[RING_TRBS];
    usb_done_fn done;
    void *ctx;
};

struct usb_dev {
    struct xhci *hc;
    struct usb_dev *parent;
    int parent_port;
    struct usb_ep *eps[32];
    void (*detach)(void *ctx);
    void *detach_ctx;
    int slot;
    int root_port;
    int speed;
    uint32_t route;
    int depth;
    int tt_slot, tt_port;
    uint8_t *in_ctx, *out_ctx;
    uint64_t in_phys, out_phys;
    struct ring ep0;
    uint8_t *xfer;          /* control transfer buffer, one page */
    uint64_t xfer_phys;
    int mps0;
    bool is_hub;
    int hub_ports;
    uint32_t hub_seen;      /* ports with an enumerated device */
    struct usb_func funcs[3];
    int nfuncs;
    char name[64];
    bool dead;
};

struct xhci {
    volatile uint8_t *cap, *op, *rt, *db;
    int max_slots, max_ports, csz;
    bool ac64;
    uint64_t *dcbaa;
    struct ring cmd;
    struct trb *evt;
    uint64_t evt_phys;
    unsigned evt_deq;
    uint32_t evt_cycle;
    struct usb_dev *slots[256];
    uint32_t port_seen;
};

#define MAX_HC 4
static struct xhci *hcs[MAX_HC];
static int nhc;
static struct usb_dev *devices[32];
static int ndevices;

int usb_device_count(void) { return ndevices; }
const char *usb_device_name(int i) { return i >= 0 && i < ndevices ? devices[i]->name : NULL; }

/* ------------------------------------------------------------------------
 * helpers
 * ---------------------------------------------------------------------- */

static inline uint32_t r32(volatile uint8_t *b, uint32_t off) { return *(volatile uint32_t *)(b + off); }
static inline void w32(volatile uint8_t *b, uint32_t off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }
static inline void w64(volatile uint8_t *b, uint32_t off, uint64_t v)
{
    *(volatile uint32_t *)(b + off) = (uint32_t)v;
    *(volatile uint32_t *)(b + off + 4) = (uint32_t)(v >> 32);
}

static uint64_t dma_page(struct xhci *hc)
{
    return hc->ac64 ? pmm_alloc() : pmm_alloc_below(0x100000000ull);
}

static bool ring_init(struct xhci *hc, struct ring *r)
{
    r->phys = dma_page(hc);
    if (!r->phys) return false;
    r->t = P2V(r->phys);
    r->enq = 0;
    r->cycle = 1;
    /* link TRB back to the start, toggling the cycle bit */
    struct trb *l = &r->t[RING_TRBS - 1];
    l->param = r->phys;
    l->status = 0;
    l->control = (TRB_LINK << 10) | (1u << 1);
    return true;
}

static uint64_t ring_push(struct ring *r, uint64_t param, uint32_t status, uint32_t control)
{
    struct trb *t = &r->t[r->enq];
    t->param = param;
    t->status = status;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    t->control = (control & ~1u) | r->cycle;
    uint64_t phys = r->phys + r->enq * sizeof(struct trb);
    if (++r->enq == RING_TRBS - 1) {
        struct trb *l = &r->t[RING_TRBS - 1];
        l->control = (l->control & ~1u) | r->cycle;
        r->cycle ^= 1;
        r->enq = 0;
    }
    return phys;
}

static void doorbell(struct xhci *hc, int slot, uint32_t target)
{
    w32(hc->db, (uint32_t)slot * 4, target);
}

static bool event_pop(struct xhci *hc, struct trb *out)
{
    struct trb *e = &hc->evt[hc->evt_deq];
    if ((e->control & 1) != hc->evt_cycle) return false;
    *out = *e;
    if (++hc->evt_deq == RING_TRBS) {
        hc->evt_deq = 0;
        hc->evt_cycle ^= 1;
    }
    w64(hc->rt, 0x20 + 0x18, (hc->evt_phys + hc->evt_deq * sizeof(struct trb)) | (1u << 3));
    return true;
}

static void *ctx(struct xhci *hc, uint8_t *base, int index)
{
    return base + (size_t)index * (size_t)hc->csz;
}

static void handle_event(struct xhci *hc, struct trb *ev);

/* issue a command and wait for its completion event */
static int command(struct xhci *hc, uint64_t param, uint32_t control, int *slot_out)
{
    uint64_t trb = ring_push(&hc->cmd, param, 0, control);
    doorbell(hc, 0, 0);
    uint64_t end = uptime_ms() + 1000;
    while (uptime_ms() < end) {
        struct trb ev;
        while (event_pop(hc, &ev)) {
            int type = (ev.control >> 10) & 63;
            if (type == TRB_EV_CMD && ev.param == trb) {
                if (slot_out) *slot_out = (int)(ev.control >> 24);
                return (int)(ev.status >> 24);
            }
            handle_event(hc, &ev);
        }
        udelay(50);
    }
    return -1;
}

/* wait for a transfer event on `trb` */
static int wait_transfer(struct xhci *hc, uint64_t trb, uint32_t *residual)
{
    uint64_t end = uptime_ms() + 1000;
    while (uptime_ms() < end) {
        struct trb ev;
        while (event_pop(hc, &ev)) {
            int type = (ev.control >> 10) & 63;
            if (type == TRB_EV_TRANSFER && ev.param == trb) {
                if (residual) *residual = ev.status & 0xFFFFFF;
                return (int)(ev.status >> 24);
            }
            handle_event(hc, &ev);
        }
        udelay(50);
    }
    return -1;
}

/* control transfer on endpoint 0; data goes through dev->xfer */
static int control(struct usb_dev *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len)
{
    struct xhci *hc = d->hc;
    uint64_t setup = (uint64_t)type | ((uint64_t)req << 8) | ((uint64_t)value << 16) | ((uint64_t)index << 32) |
                     ((uint64_t)len << 48);
    bool in = type & 0x80;
    uint32_t trt = len ? (in ? 3u : 2u) : 0u;
    ring_push(&d->ep0, setup, 8, (TRB_SETUP << 10) | (1u << 6) | (trt << 16));
    if (len) ring_push(&d->ep0, d->xfer_phys, len, (TRB_DATA << 10) | (in ? (1u << 16) : 0));
    uint64_t st = ring_push(&d->ep0, 0, 0, (TRB_STATUS << 10) | (1u << 5) | ((len && in) ? 0 : (1u << 16)));
    doorbell(hc, d->slot, 1);
    int cc = wait_transfer(hc, st, NULL);
    if (cc == 6) {
        /* STALL halts EP0: reset it and move the dequeue pointer past the failed TD */
        command(hc, 0, (14u << 10) | (1u << 16) | ((uint32_t)d->slot << 24), NULL);
        uint64_t deq = d->ep0.phys + d->ep0.enq * sizeof(struct trb);
        command(hc, deq | d->ep0.cycle, (16u << 10) | (1u << 16) | ((uint32_t)d->slot << 24), NULL);
    }
    return cc == 1 || cc == 13 ? 0 : -1;
}

/* ------------------------------------------------------------------------
 * class driver interface (usb.h)
 * ---------------------------------------------------------------------- */

int usb_ctrl(struct usb_dev *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, void *data, uint16_t len)
{
    if (len > PAGE_SIZE || d->dead) return -1;
    if (!(type & 0x80) && len) memcpy(d->xfer, data, len);
    int r = control(d, type, req, value, index, len);
    if (!r && (type & 0x80) && len) memcpy(data, d->xfer, len);
    return r;
}

int usb_speed(struct usb_dev *d) { return d->speed; }
void usb_set_name(struct usb_dev *d, const char *name) { strlcpy(d->name, name, sizeof(d->name)); }

void usb_set_detach(struct usb_dev *d, void (*detach)(void *ctx), void *ctx)
{
    d->detach = detach;
    d->detach_ctx = ctx;
}

static int xhci_interval(int speed, int binterval);

bool usb_open_endpoints(struct usb_dev *d, const struct usb_ep_desc *e, int n, struct usb_ep **out, usb_done_fn done,
                        void *cookie)
{
    struct xhci *hc = d->hc;
    memset(d->in_ctx, 0, 4096);
    uint32_t *icc = ctx(hc, d->in_ctx, 0);
    icc[1] = 1;
    uint32_t *sc = ctx(hc, d->in_ctx, 1);
    memcpy(sc, ctx(hc, d->out_ctx, 0), (size_t)hc->csz);
    int max_dci = (int)(sc[0] >> 27);
    for (int i = 0; i < n; i++) {
        struct usb_ep *ep = kzalloc(sizeof(*ep));
        if (!ep || !ring_init(hc, &ep->ring)) return false;
        ep->d = d;
        ep->in = e[i].addr & 0x80;
        ep->dci = (e[i].addr & 15) * 2 + (ep->in ? 1 : 0);
        ep->mps = e[i].mps & 0x7FF;
        ep->lock = (spinlock_t)SPINLOCK_INIT("usb-ep");
        ep->done = done;
        ep->ctx = cookie;
        bool intr = (e[i].attr & 3) == 3;
        uint32_t type = intr ? (ep->in ? 7u : 3u) : (ep->in ? 6u : 2u);
        uint32_t *epc = ctx(hc, d->in_ctx, ep->dci + 1);
        epc[0] = intr ? (uint32_t)xhci_interval(d->speed, e[i].interval) << 16 : 0;
        epc[1] = (3u << 1) | (type << 3) | ((uint32_t)ep->mps << 16);
        epc[2] = (uint32_t)ep->ring.phys | 1;
        epc[3] = (uint32_t)(ep->ring.phys >> 32);
        epc[4] = intr ? ((uint32_t)ep->mps | ((uint32_t)ep->mps << 16)) : 3072;
        icc[1] |= 1u << ep->dci;
        max_dci = MAX(max_dci, ep->dci);
        out[i] = ep;
    }
    sc[0] = (sc[0] & ~(31u << 27)) | ((uint32_t)max_dci << 27);
    if (command(hc, d->in_phys, (TRB_CONFIG_EP << 10) | ((uint32_t)d->slot << 24), NULL) != 1) {
        klog("usb: configure endpoint failed for slot %d", d->slot);
        return false;
    }
    for (int i = 0; i < n; i++) d->eps[out[i]->dci] = out[i];
    return true;
}

/* queue one transfer (a normal TRB); completion arrives through ep->done */
bool usb_submit(struct usb_ep *ep, uint64_t phys, uint32_t len, uintptr_t tag)
{
    if (ep->d->dead) return false;
    spin_lock(&ep->lock);
    if (ep->outstanding >= RING_TRBS / 2) {
        spin_unlock(&ep->lock);
        return false;
    }
    unsigned idx = ep->ring.enq;
    ep->tags[idx] = tag;
    ep->outstanding++;
    /* interrupt on completion and on short packet */
    ring_push(&ep->ring, phys, len, (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));
    doorbell(ep->d->hc, ep->d->slot, (uint32_t)ep->dci);
    spin_unlock(&ep->lock);
    return true;
}

/* ------------------------------------------------------------------------
 * HID
 * ---------------------------------------------------------------------- */

static const uint8_t hid_to_key[256] = {
    [0x04] = KEY_A, [0x05] = KEY_B, [0x06] = KEY_C, [0x07] = KEY_D, [0x08] = KEY_E, [0x09] = KEY_F,
    [0x0A] = KEY_G, [0x0B] = KEY_H, [0x0C] = KEY_I, [0x0D] = KEY_J, [0x0E] = KEY_K, [0x0F] = KEY_L,
    [0x10] = KEY_M, [0x11] = KEY_N, [0x12] = KEY_O, [0x13] = KEY_P, [0x14] = KEY_Q, [0x15] = KEY_R,
    [0x16] = KEY_S, [0x17] = KEY_T, [0x18] = KEY_U, [0x19] = KEY_V, [0x1A] = KEY_W, [0x1B] = KEY_X,
    [0x1C] = KEY_Y, [0x1D] = KEY_Z,
    [0x1E] = KEY_1, [0x1F] = KEY_2, [0x20] = KEY_3, [0x21] = KEY_4, [0x22] = KEY_5, [0x23] = KEY_6,
    [0x24] = KEY_7, [0x25] = KEY_8, [0x26] = KEY_9, [0x27] = KEY_0,
    [0x28] = KEY_ENTER, [0x29] = KEY_ESC, [0x2A] = KEY_BACKSPACE, [0x2B] = KEY_TAB, [0x2C] = KEY_SPACE,
    [0x2D] = KEY_MINUS, [0x2E] = KEY_EQUAL, [0x2F] = KEY_LBRACE, [0x30] = KEY_RBRACE, [0x31] = KEY_BACKSLASH,
    [0x32] = KEY_BACKSLASH, [0x33] = KEY_SEMICOLON, [0x34] = KEY_APOSTROPHE, [0x35] = KEY_GRAVE,
    [0x36] = KEY_COMMA, [0x37] = KEY_DOT, [0x38] = KEY_SLASH, [0x39] = KEY_CAPSLOCK,
    [0x3A] = KEY_F1, [0x3B] = KEY_F2, [0x3C] = KEY_F3, [0x3D] = KEY_F4, [0x3E] = KEY_F5, [0x3F] = KEY_F6,
    [0x40] = KEY_F7, [0x41] = KEY_F8, [0x42] = KEY_F9, [0x43] = KEY_F10, [0x44] = KEY_F11, [0x45] = KEY_F12,
    [0x46] = KEY_SYSRQ, [0x47] = KEY_SCROLLLOCK, [0x49] = KEY_INSERT, [0x4A] = KEY_HOME, [0x4B] = KEY_PAGEUP,
    [0x4C] = KEY_DELETE, [0x4D] = KEY_END, [0x4E] = KEY_PAGEDOWN, [0x4F] = KEY_RIGHT, [0x50] = KEY_LEFT,
    [0x51] = KEY_DOWN, [0x52] = KEY_UP, [0x53] = KEY_NUMLOCK, [0x54] = KEY_KPSLASH, [0x55] = KEY_KPASTERISK,
    [0x56] = KEY_KPMINUS, [0x57] = KEY_KPPLUS, [0x58] = KEY_KPENTER, [0x59] = KEY_KP1, [0x5A] = KEY_KP2,
    [0x5B] = KEY_KP3, [0x5C] = KEY_KP4, [0x5D] = KEY_KP5, [0x5E] = KEY_KP6, [0x5F] = KEY_KP7, [0x60] = KEY_KP8,
    [0x61] = KEY_KP9, [0x62] = KEY_KP0, [0x63] = KEY_KPDOT, [0x64] = KEY_102ND, [0x65] = KEY_MENU,
};
static const uint8_t mod_keys[8] = { KEY_LCTRL, KEY_LSHIFT, KEY_LALT, KEY_LMETA, KEY_RCTRL, KEY_RSHIFT, KEY_RALT, KEY_RMETA };

static void key_event(int key, bool pressed)
{
    struct raw_input ev = { 0 };
    ev.type = RAW_KEY;
    ev.key = (uint16_t)key;
    ev.pressed = pressed;
    input_push(&ev);
}

static void keyboard_report(struct usb_func *f, const uint8_t *r, int len)
{
    if (len < 3) return;
    /* ignore "phantom" rollover reports */
    if (r[2] == 1) return;
    uint8_t changed = r[0] ^ f->prev[0];
    for (int b = 0; b < 8; b++)
        if (changed & (1 << b)) key_event(mod_keys[b], r[0] & (1 << b));
    for (int i = 2; i < 8 && i < len; i++) {           /* releases */
        uint8_t k = f->prev[i];
        if (!k) continue;
        bool still = false;
        for (int j = 2; j < 8 && j < len; j++) if (r[j] == k) still = true;
        if (!still && hid_to_key[k]) {
            key_event(hid_to_key[k], false);
            if (f->repeat_key == hid_to_key[k]) f->repeat_key = 0;
        }
    }
    for (int i = 2; i < 8 && i < len; i++) {           /* presses */
        uint8_t k = r[i];
        if (!k) continue;
        bool was = false;
        for (int j = 2; j < 8; j++) if (f->prev[j] == k) was = true;
        if (!was && hid_to_key[k]) {
            key_event(hid_to_key[k], true);
            f->repeat_key = hid_to_key[k];
            f->repeat_at = uptime_ms() + 500;
        }
    }
    memcpy(f->prev, r, (size_t)MIN(len, 8));
}

static int32_t bits(const uint8_t *r, int len, int off, int size, bool sign)
{
    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        int bit = off + i;
        if (bit / 8 >= len) break;
        if (r[bit / 8] & (1 << (bit % 8))) v |= 1u << i;
    }
    if (sign && size < 32 && (v & (1u << (size - 1)))) v |= ~0u << size;
    return (int32_t)v;
}

static void pointer_report(struct usb_func *f, const uint8_t *r, int len)
{
    struct hid_layout *l = &f->lay;
    if (l->report_id) {
        if (len < 1 || r[0] != l->report_id) return;
        r++;
        len--;
    }
    struct raw_input ev = { 0 };
    int btn = 0;
    for (int i = 0; i < l->btn_count && i < 3; i++)
        if (bits(r, len, l->btn_off + i, 1, false)) btn |= 1 << i;
    ev.buttons = (uint8_t)((btn & 1 ? BTN_LEFT : 0) | (btn & 2 ? BTN_RIGHT : 0) | (btn & 4 ? BTN_MIDDLE : 0));
    /* HID: positive wheel = away from the user; the input layer expects PS/2 sign */
    if (l->w_size) ev.wheel = (int8_t)-bits(r, len, l->w_off, l->w_size, true);
    if (l->absolute) {
        ev.type = RAW_MOUSE_ABS;
        int32_t x = bits(r, len, l->x_off, l->x_size, false), y = bits(r, len, l->y_off, l->y_size, false);
        ev.x = (int32_t)((int64_t)x * 65535 / MAX(l->xmax, 1));
        ev.y = (int32_t)((int64_t)y * 65535 / MAX(l->ymax, 1));
    } else {
        ev.type = RAW_MOUSE_REL;
        ev.x = bits(r, len, l->x_off, l->x_size, true);
        ev.y = -bits(r, len, l->y_off, l->y_size, true);
    }
    input_push(&ev);
}

/* minimal HID report descriptor parser: buttons, X, Y and wheel */
static void parse_report_desc(const uint8_t *d, int len, struct hid_layout *l)
{
    memset(l, 0, sizeof(*l));
    int usage_page = 0, report_size = 0, report_count = 0, report_id = 0;
    int32_t lmax = 0;
    int usages[16], nusages = 0, umin = 0, umax = -1;
    int off = 0;
    bool first_id_locked = false;
    for (int i = 0; i < len;) {
        uint8_t p = d[i];
        int size = p & 3;
        if (size == 3) size = 4;
        int type = (p >> 2) & 3, tag = p >> 4;
        uint32_t v = 0;
        for (int k = 0; k < size && i + 1 + k < len; k++) v |= (uint32_t)d[i + 1 + k] << (8 * k);
        i += 1 + size;
        if (type == 1) {                          /* global */
            if (tag == 0) usage_page = (int)v;
            else if (tag == 2) lmax = size == 1 ? (int8_t)v : size == 2 ? (int16_t)v : (int32_t)v;
            else if (tag == 7) report_size = (int)v;
            else if (tag == 9) report_count = (int)v;
            else if (tag == 8) {
                if (first_id_locked && l->valid) { report_id = -1; continue; }
                report_id = (int)v;
                off = 0;
            }
        } else if (type == 2) {                   /* local */
            if (tag == 0 && nusages < 16) usages[nusages++] = (int)(v & 0xFFFF);
            else if (tag == 1) umin = (int)v;
            else if (tag == 2) umax = (int)v;
        } else if (type == 0) {                   /* main */
            if (tag == 8 && report_id >= 0) {     /* input */
                bool constant = v & 1, relative = v & 4;
                for (int n = 0; n < report_count; n++) {
                    int usage = n < nusages ? usages[n] : (umax >= umin ? umin + n : (nusages ? usages[nusages - 1] : 0));
                    int bo = off + n * report_size;
                    if (constant) continue;
                    if (usage_page == 9) {
                        if (!l->btn_count) l->btn_off = bo;
                        l->btn_count++;
                        l->valid = true;
                        first_id_locked = true;
                        l->report_id = report_id;
                    } else if (usage_page == 1 && usage == 0x30) {
                        l->x_off = bo; l->x_size = report_size; l->absolute = !relative; l->xmax = lmax;
                        l->valid = true;
                        first_id_locked = true;
                        l->report_id = report_id;
                    } else if (usage_page == 1 && usage == 0x31) {
                        l->y_off = bo; l->y_size = report_size; l->ymax = lmax;
                    } else if (usage_page == 1 && usage == 0x38) {
                        l->w_off = bo; l->w_size = report_size;
                    }
                }
                off += report_size * report_count;
            }
            nusages = 0;
            umin = 0;
            umax = -1;
        }
    }
}

static void queue_report(struct usb_dev *d, struct usb_func *f, int slot_i)
{
    ring_push(&f->ring, f->buf_phys + (uint64_t)slot_i * 64, (uint32_t)MIN(f->mps, 64),
              (TRB_NORMAL << 10) | (1u << 5) | (1u << 2));
    doorbell(d->hc, d->slot, (uint32_t)f->dci);
}

/* ------------------------------------------------------------------------
 * events
 * ---------------------------------------------------------------------- */

/* a device went away: tell its class driver, then everything behind it */
static void device_gone(struct usb_dev *d)
{
    if (d->dead) return;
    d->dead = true;
    if (d->detach) d->detach(d->detach_ctx);
    klog("usb: %s disconnected", d->name[0] ? d->name : "device");
    for (int i = 0; i < ndevices; i++)
        if (devices[i]->parent == d) device_gone(devices[i]);
}

static void ep_complete(struct usb_ep *ep, struct trb *ev)
{
    if (ev->param < ep->ring.phys || ev->param >= ep->ring.phys + RING_TRBS * sizeof(struct trb)) return;
    unsigned idx = (unsigned)((ev->param - ep->ring.phys) / sizeof(struct trb));
    struct trb *t = &ep->ring.t[idx];
    uint32_t requested = t->status & 0x1FFFF;
    uint32_t residual = ev->status & 0xFFFFFF;
    int cc = (int)(ev->status >> 24);
    uintptr_t tag = ep->tags[idx];
    spin_lock(&ep->lock);
    ep->outstanding--;
    spin_unlock(&ep->lock);
    if (cc == 4 || cc == 6) {                       /* transaction error or stall: unplugged or broken */
        device_gone(ep->d);
        return;
    }
    if (ep->done) ep->done(ep->ctx, ep, tag, cc, residual <= requested ? requested - residual : 0);
}

static void handle_event(struct xhci *hc, struct trb *ev)
{
    int type = (ev->control >> 10) & 63;
    if (type == TRB_EV_PORT) {
        int port = (int)(ev->param >> 24) & 0xFF;
        uint32_t sc = r32(hc->op, OP_PORTSC(port));
        w32(hc->op, OP_PORTSC(port), (sc & ~PORT_PED & ~PORT_CHANGE_BITS) | (sc & PORT_CHANGE_BITS));
        if (!(sc & PORT_CCS)) {
            hc->port_seen &= ~(1u << port);
            for (int i = 0; i < ndevices; i++)
                if (devices[i]->hc == hc && devices[i]->root_port == port && !devices[i]->parent) device_gone(devices[i]);
        }
        return;
    }
    if (type != TRB_EV_TRANSFER) return;
    int slot = (int)(ev->control >> 24), dci = (int)(ev->control >> 16) & 31;
    struct usb_dev *d = hc->slots[slot];
    if (!d) return;
    if (d->eps[dci]) {
        ep_complete(d->eps[dci], ev);
        return;
    }
    int cc = (int)(ev->status >> 24);
    for (int i = 0; i < d->nfuncs; i++) {
        struct usb_func *f = &d->funcs[i];
        if (f->dci != dci) continue;
        /* the event points at the TRB; the TRB points at the buffer */
        if (ev->param < f->ring.phys || ev->param >= f->ring.phys + PAGE_SIZE) return;
        struct trb *t = P2V(ev->param);
        int idx = (int)((t->param - f->buf_phys) / 64);
        if (idx < 0 || idx > 3) return;
        if (cc == 1 || cc == 13) {
            int len = MIN(f->mps, 64) - (int)(ev->status & 0xFFFFFF);
            const uint8_t *r = f->buf + idx * 64;
            if (f->kind == F_KEYBOARD) keyboard_report(f, r, len);
            else pointer_report(f, r, len);
        } else if (cc == 4 || cc == 6) {
            d->dead = true;            /* transaction error / stall: device gone */
            return;
        }
        queue_report(d, f, idx);
        return;
    }
}

/* ------------------------------------------------------------------------
 * enumeration
 * ---------------------------------------------------------------------- */

static int ep0_mps_for(int speed)
{
    switch (speed) {
    case 2: return 8;       /* low */
    case 3: return 64;      /* high */
    case 4: return 512;     /* super */
    default: return 8;      /* full: fixed up after reading the descriptor */
    }
}

static int xhci_interval(int speed, int binterval)
{
    int iv;
    if (speed == 3 || speed == 4) iv = CLAMP(binterval, 1, 16) - 1;
    else {
        /* frames (1 ms) -> exponent of 125 us units */
        int ms = CLAMP(binterval, 1, 255), e = 3;
        while ((1 << (e + 1)) <= ms * 8 && e < 10) e++;
        iv = e;
    }
    return CLAMP(iv, 3, 10);
}

static void enumerate(struct xhci *hc, int root_port, int speed, struct usb_dev *parent, int parent_port);

static void setup_hub(struct usb_dev *d)
{
    if (control(d, 0xA0, 6, 0x2900, 0, 16)) return;
    d->hub_ports = MIN(d->xfer[2], 15);
    d->is_hub = true;
    /* tell the controller this slot is a hub */
    struct xhci *hc = d->hc;
    memset(d->in_ctx, 0, 4096);
    uint32_t *icc = ctx(hc, d->in_ctx, 0);
    icc[1] = 1;                                 /* A0: slot context */
    uint32_t *sc = ctx(hc, d->in_ctx, 1);
    uint32_t *osc = ctx(hc, d->out_ctx, 0);
    memcpy(sc, osc, (size_t)hc->csz);
    sc[0] |= 1u << 26;                          /* Hub */
    sc[1] = (sc[1] & 0x00FFFFFF) | ((uint32_t)d->hub_ports << 24);
    command(hc, d->in_phys, (TRB_EVAL_CTX << 10) | ((uint32_t)d->slot << 24), NULL);
    command(hc, d->in_phys, (TRB_CONFIG_EP << 10) | ((uint32_t)d->slot << 24), NULL);
    for (int p = 1; p <= d->hub_ports; p++) control(d, 0x23, 3, 8, (uint16_t)p, 0);   /* PORT_POWER */
    mdelay(100);
    snprintf(d->name, sizeof(d->name), "USB hub (%d ports)", d->hub_ports);
}

static void hub_scan(struct usb_dev *hub)
{
    for (int p = 1; p <= hub->hub_ports; p++) {
        if (control(hub, 0xA3, 0, 0, (uint16_t)p, 4)) continue;
        uint16_t st = (uint16_t)(hub->xfer[0] | hub->xfer[1] << 8);
        uint16_t ch = (uint16_t)(hub->xfer[2] | hub->xfer[3] << 8);
        if (ch & 1) control(hub, 0x23, 1, 16, (uint16_t)p, 0);       /* clear C_PORT_CONNECTION */
        bool connected = st & 1;
        if (!connected) {
            if (hub->hub_seen & (1u << p))
                for (int i = 0; i < ndevices; i++)
                    if (devices[i]->parent == hub && devices[i]->parent_port == p) device_gone(devices[i]);
            hub->hub_seen &= ~(1u << p);
            continue;
        }
        if (hub->hub_seen & (1u << p)) continue;
        control(hub, 0x23, 3, 4, (uint16_t)p, 0);                    /* PORT_RESET */
        for (int i = 0; i < 50; i++) {
            mdelay(10);
            if (control(hub, 0xA3, 0, 0, (uint16_t)p, 4)) break;
            if (hub->xfer[2] & 0x10) break;                           /* C_PORT_RESET */
        }
        control(hub, 0x23, 1, 20, (uint16_t)p, 0);                   /* clear C_PORT_RESET */
        control(hub, 0xA3, 0, 0, (uint16_t)p, 4);
        st = (uint16_t)(hub->xfer[0] | hub->xfer[1] << 8);
        if (!(st & 2)) continue;                                     /* not enabled */
        int speed = (st & (1 << 9)) ? 2 : (st & (1 << 10)) ? 3 : 1;
        hub->hub_seen |= 1u << p;
        mdelay(20);
        enumerate(hub->hc, hub->root_port, speed, hub, p);
    }
}

static void configure_hid(struct usb_dev *d, uint8_t *cfg, int total)
{
    struct xhci *hc = d->hc;
    int iface = -1, iclass = 0, isub = 0, iproto = 0, report_len = 0;
    int max_dci = 1;
    memset(d->in_ctx, 0, 4096);
    uint32_t *icc = ctx(hc, d->in_ctx, 0);
    for (int off = 0; off + 2 <= total && cfg[off];) {
        uint8_t len = cfg[off], type = cfg[off + 1];
        if (type == 4) {
            iface = cfg[off + 2];
            iclass = cfg[off + 5];
            isub = cfg[off + 6];
            iproto = cfg[off + 7];
            report_len = 0;
        } else if (type == 0x21 && len >= 9) {
            report_len = cfg[off + 7] | cfg[off + 8] << 8;
        } else if (type == 5 && iclass == 3 && d->nfuncs < 3) {
            uint8_t addr = cfg[off + 2], attr = cfg[off + 3];
            if ((addr & 0x80) && (attr & 3) == 3) {
                struct usb_func *f = &d->funcs[d->nfuncs++];
                f->iface = iface;
                f->ep_addr = addr;
                f->mps = (cfg[off + 4] | cfg[off + 5] << 8) & 0x7FF;
                f->interval = cfg[off + 6];
                f->dci = (addr & 15) * 2 + 1;
                f->kind = iproto == 1 ? F_KEYBOARD : F_POINTER;
                f->boot = isub == 1;
                f->report_len = report_len;
                max_dci = MAX(max_dci, f->dci);
            }
        }
        off += len;
    }
    if (!d->nfuncs) return;
    /* input context: slot + each interrupt endpoint */
    icc[1] = 1;
    uint32_t *sc = ctx(hc, d->in_ctx, 1);
    memcpy(sc, ctx(hc, d->out_ctx, 0), (size_t)hc->csz);
    sc[0] = (sc[0] & ~(31u << 27)) | ((uint32_t)max_dci << 27);
    for (int i = 0; i < d->nfuncs; i++) {
        struct usb_func *f = &d->funcs[i];
        if (!ring_init(hc, &f->ring)) return;
        f->buf_phys = dma_page(hc);
        f->buf = P2V(f->buf_phys);
        icc[1] |= 1u << f->dci;
        uint32_t *ep = ctx(hc, d->in_ctx, f->dci + 1);
        ep[0] = (uint32_t)xhci_interval(d->speed, f->interval) << 16;
        ep[1] = (3u << 1) | (7u << 3) | ((uint32_t)f->mps << 16);
        ep[2] = (uint32_t)f->ring.phys | 1;
        ep[3] = (uint32_t)(f->ring.phys >> 32);
        ep[4] = (uint32_t)f->mps | ((uint32_t)f->mps << 16);
    }
    if (command(hc, d->in_phys, (TRB_CONFIG_EP << 10) | ((uint32_t)d->slot << 24), NULL) != 1) {
        klog("usb: configure endpoint failed for slot %d", d->slot);
        d->nfuncs = 0;
        return;
    }
    for (int i = 0; i < d->nfuncs; i++) {
        struct usb_func *f = &d->funcs[i];
        if (f->kind == F_KEYBOARD) {
            if (f->boot) control(d, 0x21, 0x0B, 0, (uint16_t)f->iface, 0);   /* SET_PROTOCOL boot */
            control(d, 0x21, 0x0A, 0, (uint16_t)f->iface, 0);                /* SET_IDLE */
        } else {
            /* boot-capable mice start in boot protocol; ask for full reports */
            if (f->boot) control(d, 0x21, 0x0B, 1, (uint16_t)f->iface, 0);
            int rl = MIN(f->report_len ? f->report_len : 256, 1024);
            if (!control(d, 0x81, 6, 0x2200, (uint16_t)f->iface, (uint16_t)rl))
                parse_report_desc(d->xfer, rl, &f->lay);
            if (!f->lay.valid) {
                /* boot mouse layout: buttons, dx, dy */
                f->lay = (struct hid_layout){ 0, 0, 3, 8, 8, 16, 8, 24, 8, false, 0, 0, true };
            }
        }
        for (int k = 0; k < 4; k++) queue_report(d, f, k);
    }
}

static void enumerate(struct xhci *hc, int root_port, int speed, struct usb_dev *parent, int parent_port)
{
    int slot = 0;
    if (command(hc, 0, TRB_ENABLE_SLOT << 10, &slot) != 1 || slot <= 0 || slot > hc->max_slots) {
        klog("usb: enable slot failed on port %d", root_port);
        return;
    }
    struct usb_dev *d = kzalloc(sizeof(*d));
    d->hc = hc;
    d->slot = slot;
    d->root_port = root_port;
    d->speed = speed;
    if (parent) {
        d->parent = parent;
        d->parent_port = parent_port;
        d->depth = parent->depth + 1;
        d->route = parent->route | ((uint32_t)MIN(parent_port, 15) << (4 * parent->depth));
        if (parent->speed == 3 && speed < 3) { d->tt_slot = parent->slot; d->tt_port = parent_port; }
        else { d->tt_slot = parent->tt_slot; d->tt_port = parent->tt_port; }
    }
    d->out_phys = dma_page(hc);
    d->in_phys = dma_page(hc);
    d->xfer_phys = dma_page(hc);
    if (!d->out_phys || !d->in_phys || !d->xfer_phys || !ring_init(hc, &d->ep0)) { kfree(d); return; }
    d->out_ctx = P2V(d->out_phys);
    d->in_ctx = P2V(d->in_phys);
    d->xfer = P2V(d->xfer_phys);
    d->mps0 = ep0_mps_for(speed);
    hc->slots[slot] = d;
    hc->dcbaa[slot] = d->out_phys;

    uint32_t *icc = ctx(hc, d->in_ctx, 0);
    icc[1] = 3;                                          /* A0 | A1 */
    uint32_t *sc = ctx(hc, d->in_ctx, 1);
    sc[0] = d->route | ((uint32_t)speed << 20) | (1u << 27);
    sc[1] = (uint32_t)root_port << 16;
    if (d->tt_slot) sc[2] = (uint32_t)d->tt_slot | ((uint32_t)d->tt_port << 8);
    uint32_t *ep0 = ctx(hc, d->in_ctx, 2);
    ep0[1] = (3u << 1) | (4u << 3) | ((uint32_t)d->mps0 << 16);
    ep0[2] = (uint32_t)d->ep0.phys | 1;
    ep0[3] = (uint32_t)(d->ep0.phys >> 32);
    ep0[4] = 8;
    int cc = command(hc, d->in_phys, (TRB_ADDRESS_DEV << 10) | ((uint32_t)slot << 24), NULL);
    if (cc != 1) {
        klog("usb: address device failed (cc %d) on port %d", cc, root_port);
        hc->slots[slot] = NULL;
        return;
    }
    /* first 8 bytes tell us the real EP0 packet size */
    if (control(d, 0x80, 6, 0x0100, 0, 8)) { klog("usb: no device descriptor on port %d", root_port); return; }
    int mps = speed == 4 ? (1 << d->xfer[7]) : d->xfer[7];
    if (mps && mps != d->mps0) {
        d->mps0 = mps;
        memset(d->in_ctx, 0, 4096);
        icc[1] = 2;
        ep0 = ctx(hc, d->in_ctx, 2);
        ep0[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps << 16);
        command(hc, d->in_phys, (TRB_EVAL_CTX << 10) | ((uint32_t)slot << 24), NULL);
    }
    if (control(d, 0x80, 6, 0x0100, 0, 18)) return;
    uint8_t dev_desc[18];
    memcpy(dev_desc, d->xfer, 18);
    uint8_t dev_class = d->xfer[4];
    uint16_t vid = (uint16_t)(d->xfer[8] | d->xfer[9] << 8), pid = (uint16_t)(d->xfer[10] | d->xfer[11] << 8);
    static const char *speeds[] = { "?", "full", "low", "high", "super" };
    if (dev_class != 9 && usbnet_probe(d, dev_desc)) {
        klog("usb: slot %d port %d (%s speed%s): %s", slot, root_port, speeds[CLAMP(speed, 0, 4)],
             parent ? ", behind hub" : "", d->name);
        if (ndevices < 32) devices[ndevices++] = d;
        return;
    }
    if (control(d, 0x80, 6, 0x0200, 0, 9)) return;
    int total = MIN(d->xfer[2] | d->xfer[3] << 8, 1024);
    uint8_t cfg_value = d->xfer[5];
    if (control(d, 0x80, 6, 0x0200, 0, (uint16_t)total)) return;
    uint8_t *cfg = kmalloc((size_t)total);
    memcpy(cfg, d->xfer, (size_t)total);
    control(d, 0x00, 9, cfg_value, 0, 0);                /* SET_CONFIGURATION */

    if (dev_class == 9) {
        setup_hub(d);
    } else {
        configure_hid(d, cfg, total);
        const char *what = "USB device";
        for (int i = 0; i < d->nfuncs; i++)
            what = d->funcs[i].kind == F_KEYBOARD ? "USB keyboard" : (d->funcs[i].lay.absolute ? "USB tablet" : "USB mouse");
        snprintf(d->name, sizeof(d->name), "%s %04x:%04x", what, vid, pid);
    }
    kfree(cfg);
    klog("usb: slot %d port %d (%s speed%s): %s", slot, root_port, speeds[CLAMP(speed, 0, 4)],
         parent ? ", behind hub" : "", d->name);
    if (ndevices < 32) devices[ndevices++] = d;
    if (d->is_hub) hub_scan(d);
}

static void scan_root_ports(struct xhci *hc)
{
    for (int p = 1; p <= hc->max_ports; p++) {
        uint32_t sc = r32(hc->op, OP_PORTSC(p));
        if (!(sc & PORT_CCS)) continue;
        if (hc->port_seen & (1u << p)) continue;
        if (!(sc & PORT_PED)) {
            w32(hc->op, OP_PORTSC(p), (sc & ~PORT_PED & ~PORT_CHANGE_BITS) | PORT_PR);
            for (int i = 0; i < 250; i++) {
                mdelay(2);
                sc = r32(hc->op, OP_PORTSC(p));
                if (!(sc & PORT_PR) && (sc & PORT_PED)) break;
            }
            w32(hc->op, OP_PORTSC(p), (sc & ~PORT_PED & ~PORT_CHANGE_BITS) | (sc & PORT_CHANGE_BITS));
        }
        sc = r32(hc->op, OP_PORTSC(p));
        if (!(sc & PORT_PED)) continue;
        hc->port_seen |= 1u << p;
        mdelay(10);
        enumerate(hc, p, (int)((sc >> 10) & 15), NULL, 0);
    }
}

/* ------------------------------------------------------------------------
 * controller bring-up
 * ---------------------------------------------------------------------- */

static void bios_handoff(volatile uint8_t *cap, uint32_t hccparams1)
{
    uint32_t off = ((hccparams1 >> 16) & 0xFFFF) * 4;
    while (off) {
        volatile uint32_t *x = (volatile uint32_t *)(cap + off);
        uint32_t v = *x;
        if ((v & 0xFF) == 1) {                       /* USB legacy support */
            *x = v | (1u << 24);                     /* OS owned */
            for (int i = 0; i < 1000 && (*x & (1u << 16)); i++) mdelay(1);
            *x = *x & ~(1u << 16);
            x[1] &= 0xFFFF1F10;                      /* disable SMIs */
            return;
        }
        uint32_t next = (v >> 8) & 0xFF;
        off = next ? off + next * 4 : 0;
    }
}

static struct xhci *xhci_start(struct pci_dev *p)
{
    uint64_t bar = pci_bar_addr(p, 0);
    if (!bar) return NULL;
    pci_enable_busmaster(p);
    struct xhci *hc = kzalloc(sizeof(*hc));
    hc->cap = vmm_map_mmio(bar, 0x10000);
    uint8_t caplen = hc->cap[0];
    uint32_t hcs1 = r32(hc->cap, 4), hcs2 = r32(hc->cap, 8), hcc1 = r32(hc->cap, 0x10);
    hc->op = hc->cap + caplen;
    hc->rt = hc->cap + (r32(hc->cap, 0x18) & ~0x1Fu);
    hc->db = hc->cap + (r32(hc->cap, 0x14) & ~0x3u);
    hc->max_slots = MIN((int)(hcs1 & 0xFF), 255);
    hc->max_ports = MIN((int)((hcs1 >> 24) & 0xFF), 31);
    hc->csz = (hcc1 & 4) ? 64 : 32;
    hc->ac64 = hcc1 & 1;

    bios_handoff(hc->cap, hcc1);

    /* stop and reset */
    w32(hc->op, OP_USBCMD, r32(hc->op, OP_USBCMD) & ~1u);
    for (int i = 0; i < 200 && !(r32(hc->op, OP_USBSTS) & 1); i++) mdelay(1);
    w32(hc->op, OP_USBCMD, 2);
    for (int i = 0; i < 1000 && (r32(hc->op, OP_USBCMD) & 2); i++) mdelay(1);
    for (int i = 0; i < 1000 && (r32(hc->op, OP_USBSTS) & (1u << 11)); i++) mdelay(1);

    w32(hc->op, OP_CONFIG, (uint32_t)hc->max_slots);
    uint64_t dcbaa = dma_page(hc);
    hc->dcbaa = P2V(dcbaa);
    int scratch = (int)(((hcs2 >> 21) & 0x1F) << 5 | ((hcs2 >> 27) & 0x1F));
    if (scratch) {
        uint64_t arr = dma_page(hc);
        uint64_t *a = P2V(arr);
        for (int i = 0; i < scratch && i < 512; i++) a[i] = dma_page(hc);
        hc->dcbaa[0] = arr;
    }
    w64(hc->op, OP_DCBAAP, dcbaa);

    if (!ring_init(hc, &hc->cmd)) return NULL;
    w64(hc->op, OP_CRCR, hc->cmd.phys | 1);

    /* one event ring segment for interrupter 0 */
    hc->evt_phys = dma_page(hc);
    hc->evt = P2V(hc->evt_phys);
    hc->evt_cycle = 1;
    uint64_t erst = dma_page(hc);
    uint64_t *e = P2V(erst);
    e[0] = hc->evt_phys;
    e[1] = RING_TRBS;
    w32(hc->rt, 0x20 + 0x08, 1);                     /* ERSTSZ */
    w64(hc->rt, 0x20 + 0x18, hc->evt_phys);          /* ERDP */
    w64(hc->rt, 0x20 + 0x10, erst);                  /* ERSTBA */

    w32(hc->op, OP_USBCMD, 1);                       /* run */
    for (int i = 0; i < 100 && (r32(hc->op, OP_USBSTS) & 1); i++) mdelay(1);
    /* power the ports (needed on controllers with port power switches) */
    for (int port = 1; port <= hc->max_ports; port++) {
        uint32_t sc = r32(hc->op, OP_PORTSC(port));
        if (!(sc & PORT_PP)) w32(hc->op, OP_PORTSC(port), (sc & ~PORT_PED & ~PORT_CHANGE_BITS) | PORT_PP);
    }
    klog("usb: xHCI %02x:%02x.%x, %d slots, %d ports%s", p->bus, p->dev, p->fn, hc->max_slots, hc->max_ports,
         hc->ac64 ? ", 64-bit" : "");
    return hc;
}

static int usb_thread(void *arg)
{
    UNUSED(arg);
    mdelay(50);
    for (int i = 0; i < nhc; i++) scan_root_ports(hcs[i]);
    uint64_t last_scan = uptime_ms();
    for (;;) {
        for (int i = 0; i < nhc; i++) {
            struct trb ev;
            while (event_pop(hcs[i], &ev)) handle_event(hcs[i], &ev);
        }
        usbnet_service();
        uint64_t now = uptime_ms();
        /* software key repeat for USB keyboards */
        for (int i = 0; i < ndevices; i++) {
            struct usb_dev *d = devices[i];
            for (int k = 0; k < d->nfuncs; k++) {
                struct usb_func *f = &d->funcs[k];
                if (f->kind == F_KEYBOARD && f->repeat_key && now >= f->repeat_at) {
                    key_event(f->repeat_key, true);
                    f->repeat_at = now + 33;
                }
            }
        }
        /* hot plug: rescan root ports and hubs once a second */
        if (now - last_scan >= 1000) {
            last_scan = now;
            for (int i = 0; i < nhc; i++) scan_root_ports(hcs[i]);
            for (int i = 0; i < ndevices; i++) if (devices[i]->is_hub && !devices[i]->dead) hub_scan(devices[i]);
        }
        sched_sleep(2);
    }
    return 0;
}

void usb_init(void)
{
    for (int i = 0; i < pci_count() && nhc < MAX_HC; i++) {
        struct pci_dev *p = pci_get(i);
        if (p->cls == 0x0C && p->subcls == 0x03 && p->progif == 0x30) {
            struct xhci *hc = xhci_start(p);
            if (hc) hcs[nhc++] = hc;
        }
    }
    if (nhc) thread_create_ex("usb", usb_thread, NULL, 1, -1, NULL, 32 * 1024);
}
