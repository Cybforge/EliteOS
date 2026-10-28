/*
 * Kernel panic: stop the other CPUs, dump state to serial and draw a
 * readable crash screen.
 */
#include <kernel.h>
#include <cpu.h>
#include <x86.h>
#include <dev.h>
#include <gfx.h>
#include <sched.h>

static volatile int panicking;
surface_t *bootcon_surface(bool *swap);
const char *exception_name(int v);

bool panic_in_progress(void) { return panicking != 0; }

static color_t fixc(color_t c, bool swap)
{
    if (!swap) return c;
    return (c & 0xFF00FF00u) | ((c >> 16) & 0xFF) | ((c & 0xFF) << 16);
}

static void out(const char *s)
{
    serial_write(s, strlen(s));
}

static void draw_screen(const char *msg, struct regs *r, uint64_t *trace, int ntrace)
{
    bool swap;
    surface_t *s = bootcon_surface(&swap);
    if (!s || !font_ui) return;
    surface_reset_clip(s);
    int w = s->w, h = s->h;
    for (int y = 0; y < h; y++) {
        color_t c = color_lerp(HEX(0x2A0A18), HEX(0x12060C), y * 256 / h);
        memset32(s->px + (size_t)y * s->stride, fixc(c, swap), (size_t)w);
    }
    int x = w / 8, y = h / 6;
    gfx_text(s, font_huge, x, y - 20, ":(", fixc(HEX(0xFFFFFF), swap));
    y += 70;
    gfx_text(s, font_light, x, y, "ZenithOS ran into a problem and stopped.", fixc(HEX(0xFFFFFF), swap));
    y += 50;
    gfx_text_wrap(s, font_ui_bold, R(x, y, w - 2 * x, 60), msg, fixc(HEX(0xFF9EB5), swap), 2);
    y += 50;

    char line[160];
    if (r) {
        snprintf(line, sizeof(line), "RIP %016lx  RSP %016lx  RFLAGS %08lx  CS %02lx  CR2 %016lx",
                 r->rip, r->rsp, r->rflags, r->cs, read_cr2());
        gfx_text(s, font_mono, x, y, line, fixc(HEX(0xE0D0D8), swap)); y += 18;
        snprintf(line, sizeof(line), "RAX %016lx  RBX %016lx  RCX %016lx  RDX %016lx", r->rax, r->rbx, r->rcx, r->rdx);
        gfx_text(s, font_mono, x, y, line, fixc(HEX(0xB8A8B0), swap)); y += 18;
        snprintf(line, sizeof(line), "RSI %016lx  RDI %016lx  RBP %016lx  R8  %016lx", r->rsi, r->rdi, r->rbp, r->r8);
        gfx_text(s, font_mono, x, y, line, fixc(HEX(0xB8A8B0), swap)); y += 18;
        snprintf(line, sizeof(line), "R9  %016lx  R10 %016lx  R11 %016lx  R12 %016lx", r->r9, r->r10, r->r11, r->r12);
        gfx_text(s, font_mono, x, y, line, fixc(HEX(0xB8A8B0), swap)); y += 18;
        snprintf(line, sizeof(line), "R13 %016lx  R14 %016lx  R15 %016lx", r->r13, r->r14, r->r15);
        gfx_text(s, font_mono, x, y, line, fixc(HEX(0xB8A8B0), swap)); y += 28;
    }
    struct thread *t = this_cpu()->current;
    snprintf(line, sizeof(line), "CPU %d   thread: %s", this_cpu()->id, t ? t->name : "-");
    gfx_text(s, font_mono, x, y, line, fixc(HEX(0xE0D0D8), swap)); y += 24;
    if (ntrace) {
        gfx_text(s, font_ui_md, x, y, "Call trace", fixc(HEX(0xFFFFFF), swap)); y += 20;
        for (int i = 0; i < ntrace; i++) {
            const char *name = ksym_lookup(trace[i], NULL);
            snprintf(line, sizeof(line), "  %016lx  %s", trace[i], name ? name : "?");
            gfx_text(s, font_mono, x, y, line, fixc(HEX(0xB8A8B0), swap)); y += 17;
        }
    }
    gfx_text(s, font_ui, x, h - 50, "The details above were also written to the serial port. Restart the machine to continue.",
             fixc(HEX(0x9A7A88), swap));
}

static int collect_trace(uint64_t rbp, uint64_t *out_arr, int max)
{
    int n = 0;
    while (n < max && rbp >= 0xFFFF800000000000ull && !(rbp & 7)) {
        uint64_t *frame = (uint64_t *)rbp;
        uint64_t ret = frame[1];
        if (ret < 0xFFFFFFFF80000000ull) break;
        out_arr[n++] = ret;
        uint64_t next = frame[0];
        if (next <= rbp) break;
        rbp = next;
    }
    return n;
}

static NORETURN void do_panic(struct regs *r, const char *fmt, va_list ap)
{
    cli();
    if (__atomic_exchange_n(&panicking, 1, __ATOMIC_SEQ_CST)) {
        for (;;) hlt();
    }
    /* stop everyone else */
    if (ncpus > 1) lapic_broadcast_ipi(VEC_IPI_HALT);

    char msg[256];
    vsnprintf(msg, sizeof(msg), fmt, ap);

    out("\n\n*** KERNEL PANIC ***\n");
    out(msg);
    out("\n");
    char line[128];
    if (r) {
        snprintf(line, sizeof(line), "RIP=%016lx RSP=%016lx CR2=%016lx ERR=%lx\n",
                 r->rip, r->rsp, read_cr2(), r->error);
        out(line);
    }
    uint64_t trace[16];
    uint64_t rbp;
    if (r) rbp = r->rbp;
    else __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    int n = 0;
    if (r && r->rip >= 0xFFFFFFFF80000000ull) trace[n++] = r->rip;
    n += collect_trace(rbp, trace + n, 16 - n);
    for (int i = 0; i < n; i++) {
        const char *name = ksym_lookup(trace[i], NULL);
        snprintf(line, sizeof(line), "  %016lx %s\n", trace[i], name ? name : "?");
        out(line);
    }
    draw_screen(msg, r, trace, n);
    for (;;) { cli(); hlt(); }
}

void panic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    do_panic(NULL, fmt, ap);
}

void panic_regs(struct regs *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    do_panic(r, fmt, ap);
}
