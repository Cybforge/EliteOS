/* COM1 serial port, used for the kernel log. */
#include <kernel.h>
#include <x86.h>
#include <dev.h>

#define COM1 0x3F8

static bool present;

void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* no interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB */
    outb(COM1 + 0, 0x01);   /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8N1 */
    outb(COM1 + 2, 0xC7);   /* FIFO */
    outb(COM1 + 4, 0x0B);
    /* loopback test */
    outb(COM1 + 4, 0x1E);
    outb(COM1 + 0, 0xAE);
    present = inb(COM1 + 0) == 0xAE;
    outb(COM1 + 4, 0x0F);
}

static void putc_raw(char c)
{
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) cpu_relax();
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s, size_t n)
{
    if (!present) return;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') putc_raw('\r');
        putc_raw(s[i]);
    }
}

int serial_read(void)
{
    if (!present || !(inb(COM1 + 5) & 1)) return -1;
    return inb(COM1);
}
