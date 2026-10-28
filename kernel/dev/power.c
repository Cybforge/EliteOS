/* Reboot and power-off. */
#include <kernel.h>
#include <dev.h>
#include <x86.h>
#include <cpu.h>
#include <vfs.h>
#include <sched.h>

void system_reboot(void)
{
    klog("power: rebooting");
    if (!panic_in_progress()) vfs_sync();
    cli();
    if (acpi.reset_valid && acpi.reset_space == 1)
        outb((uint16_t)acpi.reset_addr, acpi.reset_value);
    mdelay(50);
    /* keyboard controller pulse */
    for (int i = 0; i < 10000 && (inb(0x64) & 2); i++) {}
    outb(0x64, 0xFE);
    mdelay(50);
    /* PCI reset control */
    outb(0xCF9, 0x06);
    mdelay(50);
    /* triple fault */
    struct PACKED { uint16_t l; uint64_t b; } idtr = { 0, 0 };
    __asm__ volatile("lidt %0; int3" :: "m"(idtr));
    for (;;) hlt();
}

void system_poweroff(void)
{
    klog("power: shutting down");
    if (!panic_in_progress()) vfs_sync();
    cli();
    if (acpi.s5_valid && acpi.pm1a_cnt) {
        outw((uint16_t)acpi.pm1a_cnt, (uint16_t)(acpi.slp_typa | (1u << 13)));
        if (acpi.pm1b_cnt) outw((uint16_t)acpi.pm1b_cnt, (uint16_t)(acpi.slp_typb | (1u << 13)));
        mdelay(100);
    }
    /* emulator fallbacks: QEMU (new/old), VirtualBox */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    for (;;) hlt();
}
