/*
 * ZenithOS kernel entry.
 */
#include <kernel.h>
#include <bootinfo.h>
#include <cpu.h>
#include <dev.h>
#include <mm.h>
#include <gfx.h>
#include <sched.h>
#include <x86.h>
#include <input.h>
#include <vfs.h>
#include <wm.h>
#include <block.h>
#include <audio.h>
#include <sys.h>

struct bootinfo *boot_info;

NORETURN void kmain(struct bootinfo *bi)
{
    boot_info = bi;
    cpu_early_init();
    serial_init();
    kprintf("\n" ZENITH_NAME " " ZENITH_VERSION " \"" ZENITH_CODENAME "\" booting\n");
    if (bi->magic != BOOTINFO_MAGIC) panic("bad boot info magic");
    idt_init();
    fonts_init();
    bootcon_init(&bi->fb);
    klog("boot: firmware '%s', framebuffer %ux%u", bi->firmware_vendor, bi->fb.width, bi->fb.height);
    klog("cpu: %s (%s)", cpu_info.brand, cpu_info.vendor);

    bootcon_status("Memory", 10);
    pmm_init(bi);
    vmm_init(bi);
    heap_init();

    bootcon_status("Processors", 25);
    acpi_init(bi->rsdp_phys);
    lapic_init();
    ioapic_init();
    timer_calibrate();
    sched_init();
    timer_init();
    syscall_init_this();
    sti();
    smp_init();
    rtc_init();

    pmm_reclaim_acpi();

    bootcon_status("Devices", 50);
    pci_init();
    input_init();
    ps2_init();
    usb_init();
    ahci_init();
    audio_init();

    bootcon_status("File system", 70);
    vfs_init();
    if (bi->initrd_size) vfs_load_tar(P2V(bi->initrd_phys), bi->initrd_size);
    fat_automount();
    sys_init();
    net_init();

    bootcon_status("Starting desktop", 90);
    wm_init();
    klog("boot: done in %lu ms, %lu MiB free", uptime_ms(), (pmm_free_pages() * PAGE_SIZE) >> 20);

    for (;;) sched_sleep(1000);
}
