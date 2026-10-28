/* Kernel symbol lookup for stack traces (table generated at link time). */
#include <kernel.h>

struct ksym {
    uint64_t addr;
    const char *name;
};

extern const struct ksym ksym_table[];
extern const int ksym_count;

const char *ksym_lookup(uint64_t addr, uint64_t *off)
{
    int lo = 0, hi = ksym_count - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ksym_table[mid].addr <= addr) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return NULL;
    if (off) *off = addr - ksym_table[best].addr;
    return ksym_table[best].name;
}

/* the global functions modules may call: sorted by name, with their addresses */
struct kexport { const char *name; uint64_t addr; };
extern const struct kexport kexport_table[];
extern const int kexport_count;

uint64_t ksym_find(const char *name)
{
    int lo = 0, hi = kexport_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(name, kexport_table[mid].name);
        if (!c) return kexport_table[mid].addr;
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return 0;
}
