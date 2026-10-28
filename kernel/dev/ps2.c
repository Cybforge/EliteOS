/*
 * i8042 PS/2 controller: keyboard (scan code set 1 via translation) and
 * mouse (IntelliMouse wheel). If the VMware backdoor is present (QEMU,
 * VMware) the mouse switches to absolute mode, so the pointer follows the
 * host cursor without grabbing.
 */
#include <kernel.h>
#include <dev.h>
#include <cpu.h>
#include <x86.h>
#include <input.h>

#define DATA 0x60
#define STAT 0x64
#define CMD  0x64

static bool has_wheel;
static bool vmmouse;
static uint8_t packet[4];
static int pkt_idx;

static void wait_write(void)
{
    for (int i = 0; i < 100000 && (inb(STAT) & 2); i++) io_wait();
}

static bool wait_read(void)
{
    for (int i = 0; i < 100000; i++) {
        if (inb(STAT) & 1) return true;
        io_wait();
    }
    return false;
}

static void ctl_cmd(uint8_t c) { wait_write(); outb(CMD, c); }
static void dat_write(uint8_t d) { wait_write(); outb(DATA, d); }
static uint8_t dat_read(void) { return wait_read() ? inb(DATA) : 0; }

static uint8_t mouse_cmd(uint8_t c)
{
    ctl_cmd(0xD4);
    dat_write(c);
    return dat_read();    /* ACK = 0xFA */
}

static void flush(void)
{
    for (int i = 0; i < 64 && (inb(STAT) & 1); i++) inb(DATA);
}

/* ------------------------------------------------------------------------
 * VMware backdoor absolute pointer (vmmouse)
 * ---------------------------------------------------------------------- */

#define VMW_MAGIC 0x564D5868
#define VMW_PORT  0x5658
#define CMD_GETVERSION 10
#define CMD_ABSPOINTER_DATA 39
#define CMD_ABSPOINTER_STATUS 40
#define CMD_ABSPOINTER_COMMAND 41
#define VMMOUSE_ENABLE 0x45414552
#define VMMOUSE_REQUEST_ABSOLUTE 0x53424152
#define VMMOUSE_VERSION_ID 0x3442554A

struct bd { uint32_t a, b, c, d; };

static void backdoor(struct bd *r, uint32_t cmd, uint32_t arg)
{
    uint32_t a = VMW_MAGIC, b = arg, c = cmd, d = VMW_PORT;
    __asm__ volatile("inl %%dx, %%eax" : "+a"(a), "+b"(b), "+c"(c), "+d"(d));
    r->a = a; r->b = b; r->c = c; r->d = d;
}

static bool vmmouse_init(void)
{
    struct bd r;
    if (!cpu_info.hypervisor) return false;
    backdoor(&r, CMD_GETVERSION, ~0u);
    if (r.b != VMW_MAGIC || r.a == 0xFFFFFFFF) return false;
    backdoor(&r, CMD_ABSPOINTER_COMMAND, VMMOUSE_ENABLE);
    backdoor(&r, CMD_ABSPOINTER_STATUS, 0);
    if ((r.a & 0x0000FFFF) == 0) return false;
    backdoor(&r, CMD_ABSPOINTER_DATA, 1);
    if (r.a != VMMOUSE_VERSION_ID) return false;
    backdoor(&r, CMD_ABSPOINTER_COMMAND, VMMOUSE_REQUEST_ABSOLUTE);
    return true;
}

static void vmmouse_poll(void)
{
    struct bd r;
    for (int guard = 0; guard < 64; guard++) {
        backdoor(&r, CMD_ABSPOINTER_STATUS, 0);
        if ((r.a & 0xFFFF0000) == 0xFFFF0000) {   /* error: re-enable */
            vmmouse_init();
            return;
        }
        uint32_t queued = r.a & 0xFFFF;
        if (queued < 4) return;
        backdoor(&r, CMD_ABSPOINTER_DATA, 4);
        struct raw_input ev = { 0 };
        ev.type = RAW_MOUSE_ABS;
        uint32_t st = r.a;
        ev.buttons = (uint8_t)(((st & 0x20) ? BTN_LEFT : 0) | ((st & 0x10) ? BTN_RIGHT : 0) |
                               ((st & 0x08) ? BTN_MIDDLE : 0));
        ev.x = (int32_t)(r.b & 0xFFFF);
        ev.y = (int32_t)(r.c & 0xFFFF);
        ev.wheel = (int8_t)(int32_t)r.d;
        input_push(&ev);
    }
}

bool mouse_is_absolute(void) { return vmmouse; }

/* ------------------------------------------------------------------------
 * IRQ handlers
 * ---------------------------------------------------------------------- */

static bool e0;

static const uint8_t e0_map[128] = {
    [0x1C] = KEY_KPENTER, [0x1D] = KEY_RCTRL, [0x35] = KEY_KPSLASH, [0x37] = KEY_SYSRQ,
    [0x38] = KEY_RALT, [0x47] = KEY_HOME, [0x48] = KEY_UP, [0x49] = KEY_PAGEUP,
    [0x4B] = KEY_LEFT, [0x4D] = KEY_RIGHT, [0x4F] = KEY_END, [0x50] = KEY_DOWN,
    [0x51] = KEY_PAGEDOWN, [0x52] = KEY_INSERT, [0x53] = KEY_DELETE, [0x5B] = KEY_LMETA,
    [0x5C] = KEY_RMETA, [0x5D] = KEY_MENU,
};

static void kbd_irq(struct regs *r, void *ctx)
{
    UNUSED(r);
    UNUSED(ctx);
    while (inb(STAT) & 1) {
        uint8_t st = inb(STAT);
        uint8_t sc = inb(DATA);
        if (st & 0x20) continue;       /* mouse byte on the keyboard IRQ: drop */
        if (sc == 0xE0) { e0 = true; continue; }
        if (sc == 0xE1 || sc == 0xFA || sc == 0xFE) continue;
        bool pressed = !(sc & 0x80);
        uint8_t code = sc & 0x7F;
        int key;
        if (e0) {
            e0 = false;
            if (code == 0x2A || code == 0x36) continue;   /* fake shifts */
            key = e0_map[code];
        } else {
            key = code;
        }
        if (!key) continue;
        struct raw_input ev = { 0 };
        ev.type = RAW_KEY;
        ev.key = (uint16_t)key;
        ev.pressed = pressed;
        input_push(&ev);
    }
}

static void mouse_irq(struct regs *r, void *ctx)
{
    UNUSED(r);
    UNUSED(ctx);
    while (inb(STAT) & 1) {
        uint8_t st = inb(STAT);
        uint8_t b = inb(DATA);
        if (!(st & 0x20)) continue;
        if (pkt_idx == 0 && !(b & 0x08)) continue;   /* resync */
        packet[pkt_idx++] = b;
        int need = has_wheel ? 4 : 3;
        if (pkt_idx < need) continue;
        pkt_idx = 0;
        if (vmmouse) {
            vmmouse_poll();
            continue;
        }
        struct raw_input ev = { 0 };
        ev.type = RAW_MOUSE_REL;
        ev.buttons = packet[0] & 7;
        int dx = packet[1], dy = packet[2];
        if (packet[0] & 0x10) dx -= 256;
        if (packet[0] & 0x20) dy -= 256;
        if (packet[0] & 0xC0) { dx = 0; dy = 0; }     /* overflow */
        ev.x = dx;
        ev.y = -dy;
        if (has_wheel) ev.wheel = (int8_t)((packet[3] & 0x08) ? (packet[3] | 0xF0) : (packet[3] & 0x0F));
        input_push(&ev);
    }
}

void ps2_init(void)
{
    ctl_cmd(0xAD);       /* disable keyboard */
    ctl_cmd(0xA7);       /* disable mouse */
    flush();
    ctl_cmd(0x20);
    uint8_t cfg = dat_read();
    cfg |= 0x01 | 0x02 | 0x40;          /* both IRQs, translation */
    cfg &= (uint8_t)~0x30;              /* clocks on */
    ctl_cmd(0x60);
    dat_write(cfg);
    ctl_cmd(0xAE);
    ctl_cmd(0xA8);

    /* keyboard: enable scanning */
    dat_write(0xF4);
    dat_read();

    /* mouse: defaults, then try the IntelliMouse wheel sequence */
    mouse_cmd(0xF6);
    mouse_cmd(0xF3); mouse_cmd(200);
    mouse_cmd(0xF3); mouse_cmd(100);
    mouse_cmd(0xF3); mouse_cmd(80);
    mouse_cmd(0xF2);
    uint8_t id = dat_read();
    has_wheel = id == 3 || id == 4;
    mouse_cmd(0xF3); mouse_cmd(60);
    mouse_cmd(0xF4);
    flush();

    vmmouse = vmmouse_init();

    irq_register(VEC_KEYBOARD, kbd_irq, NULL);
    irq_register(VEC_MOUSE, mouse_irq, NULL);
    ioapic_route_isa(1, VEC_KEYBOARD, cpus[0].lapic_id);
    ioapic_route_isa(12, VEC_MOUSE, cpus[0].lapic_id);
    klog("ps2: keyboard ok, mouse id %u%s%s", id, has_wheel ? " (wheel)" : "",
         vmmouse ? ", absolute pointer via vmmouse" : "");
}
