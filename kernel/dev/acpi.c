/*
 * ACPI table parsing: MADT (CPUs, I/O APIC, overrides), FADT (power,
 * reset, century), DSDT \_S5 sleep type for soft power-off.
 */
#include <kernel.h>
#include <dev.h>
#include <mm.h>
#include <x86.h>

struct acpi_info acpi;

struct PACKED sdt_header {
    char sig[4];
    uint32_t length;
    uint8_t revision, checksum;
    char oem[6];
    char oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
};

struct PACKED rsdp {
    char sig[8];
    uint8_t checksum;
    char oem[6];
    uint8_t revision;
    uint32_t rsdt;
    uint32_t length;
    uint64_t xsdt;
    uint8_t xchecksum;
    uint8_t reserved[3];
};

static bool checksum_ok(const void *p, size_t n)
{
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s += ((const uint8_t *)p)[i];
    return s == 0;
}

static void parse_madt(struct sdt_header *h)
{
    uint8_t *p = (uint8_t *)h;
    acpi.lapic_phys = *(uint32_t *)(p + 36);
    uint8_t *e = p + 44, *end = p + h->length;
    while (e + 2 <= end && e[1]) {
        switch (e[0]) {
        case 0: /* local APIC */
            if ((*(uint32_t *)(e + 4) & 3) && acpi.ncpus < 64)
                acpi.lapic_ids[acpi.ncpus++] = e[3];
            break;
        case 1: /* I/O APIC - we use the first one */
            if (!acpi.ioapic_phys) {
                acpi.ioapic_phys = *(uint32_t *)(e + 4);
                acpi.ioapic_gsi_base = *(uint32_t *)(e + 8);
            }
            break;
        case 2: { /* interrupt source override */
            uint8_t src = e[3];
            if (src < 16) {
                acpi.iso[src].used = 1;
                acpi.iso[src].gsi = *(uint32_t *)(e + 4);
                acpi.iso[src].flags = *(uint16_t *)(e + 8);
            }
            break;
        }
        case 5:
            acpi.lapic_phys = *(uint64_t *)(e + 4);
            break;
        case 9: /* x2APIC */
            if ((*(uint32_t *)(e + 8) & 3) && acpi.ncpus < 64 && *(uint32_t *)(e + 4) < 255)
                acpi.lapic_ids[acpi.ncpus++] = *(uint32_t *)(e + 4);
            break;
        }
        e += e[1];
    }
}

static void parse_s5(struct sdt_header *dsdt)
{
    uint8_t *p = (uint8_t *)dsdt + sizeof(*dsdt);
    uint8_t *end = (uint8_t *)dsdt + dsdt->length;
    for (; p + 8 < end; p++) {
        if (memcmp(p, "_S5_", 4)) continue;
        if (!((p[-1] == 0x08) || (p[-2] == 0x08 && p[-1] == '\\'))) continue;
        if (p[4] != 0x12) continue;
        uint8_t *q = p + 5;
        q += ((*q & 0xC0) >> 6) + 2;          /* PkgLength + NumElements */
        if (*q == 0x0A) q++;                  /* BytePrefix */
        acpi.slp_typa = (uint16_t)(*q) << 10;
        q++;
        if (*q == 0x0A) q++;
        acpi.slp_typb = (uint16_t)(*q) << 10;
        acpi.s5_valid = true;
        return;
    }
}

static void parse_fadt(struct sdt_header *h)
{
    uint8_t *p = (uint8_t *)h;
    acpi.pm1a_cnt = *(uint32_t *)(p + 64);
    acpi.pm_tmr_port = (uint16_t)*(uint32_t *)(p + 76);
    if (h->length >= 116) acpi.pm_tmr_32 = (*(uint32_t *)(p + 112) >> 8) & 1;
    acpi.pm1b_cnt = *(uint32_t *)(p + 68);
    if (h->length > 108) acpi.century_reg = p[108];
    if (h->length >= 129) {
        uint32_t flags = *(uint32_t *)(p + 112);
        if (flags & (1u << 10)) {
            acpi.reset_space = p[116];
            acpi.reset_addr = *(uint64_t *)(p + 120);
            acpi.reset_value = p[128];
            acpi.reset_valid = true;
        }
    }
    uint64_t dsdt = *(uint32_t *)(p + 40);
    if (h->length >= 148 && *(uint64_t *)(p + 140)) dsdt = *(uint64_t *)(p + 140);

    /* enable ACPI mode if the firmware left it in legacy mode */
    uint32_t smi_cmd = *(uint32_t *)(p + 48);
    uint8_t enable = p[52];
    if (acpi.pm1a_cnt && !(inw((uint16_t)acpi.pm1a_cnt) & 1) && smi_cmd && enable) {
        outb((uint16_t)smi_cmd, enable);
        for (int i = 0; i < 300 && !(inw((uint16_t)acpi.pm1a_cnt) & 1); i++) io_wait();
    }

    if (dsdt) {
        struct sdt_header *d = P2V(dsdt);
        if (!memcmp(d->sig, "DSDT", 4)) parse_s5(d);
    }
}

void acpi_init(uint64_t rsdp_phys)
{
    if (!rsdp_phys) {
        klog("acpi: no RSDP, assuming a single CPU");
        acpi.ncpus = 1;
        acpi.lapic_phys = 0xFEE00000;
        acpi.ioapic_phys = 0xFEC00000;
        return;
    }
    struct rsdp *r = P2V(rsdp_phys);
    memcpy(acpi.oem, r->oem, 6);
    acpi.oem[6] = 0;
    bool x = r->revision >= 2 && r->xsdt;
    struct sdt_header *root = P2V(x ? r->xsdt : r->rsdt);
    if (!checksum_ok(root, root->length)) klog("acpi: root table checksum mismatch");
    int n = (int)(root->length - sizeof(*root)) / (x ? 8 : 4);
    uint8_t *ents = (uint8_t *)(root + 1);
    char sigs[128] = "";
    for (int i = 0; i < n; i++) {
        uint64_t pa = x ? *(uint64_t *)(ents + i * 8) : *(uint32_t *)(ents + i * 4);
        if (!pa) continue;
        struct sdt_header *h = P2V(pa);
        char s[6] = { h->sig[0], h->sig[1], h->sig[2], h->sig[3], ' ', 0 };
        strlcat(sigs, s, sizeof(sigs));
        if (!memcmp(h->sig, "APIC", 4)) parse_madt(h);
        else if (!memcmp(h->sig, "FACP", 4)) parse_fadt(h);
        else if (!memcmp(h->sig, "HPET", 4)) acpi.hpet_phys = *(uint64_t *)((uint8_t *)h + 44);
        else if (!memcmp(h->sig, "MCFG", 4)) acpi.mcfg_phys = pa;
    }
    if (!acpi.ncpus) { acpi.ncpus = 1; acpi.lapic_ids[0] = 0; }
    if (!acpi.lapic_phys) acpi.lapic_phys = 0xFEE00000;
    if (!acpi.ioapic_phys) acpi.ioapic_phys = 0xFEC00000;
    klog("acpi: OEM '%s', tables: %s", acpi.oem, sigs);
    klog("acpi: %d CPU(s), LAPIC %#lx, IOAPIC %#lx, S5 %s", acpi.ncpus, acpi.lapic_phys,
         acpi.ioapic_phys, acpi.s5_valid ? "ok" : "missing");
}
