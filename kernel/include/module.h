#ifndef ZENITH_MODULE_H
#define ZENITH_MODULE_H

/*
 * Loadable kernel modules (.zko): relocatable ELF objects built with the
 * kernel's flags, linked at load time against the kernel's exported
 * functions. A module provides one `zmodule_info`; its init function
 * registers whatever it drives (a graphics adapter, ...).
 */
#include <kernel.h>

#define ZMODULE_MAGIC 0x5A4D4F44u            /* "ZMOD" */

struct zmodule {
    uint32_t magic;
    const char *name;
    const char *version;
    const char *description;
    int  (*init)(void);
    void (*exit)(void);
};

/* a module defines exactly one of these */
#define ZENITH_MODULE(n, ver, desc, init_fn, exit_fn) \
    const struct zmodule zmodule_info = { ZMODULE_MAGIC, n, ver, desc, init_fn, exit_fn }

struct module_status {
    char name[32];
    char version[16];
    char description[80];
    char origin[96];
    uint64_t base;
    size_t size;
};

int  module_load(const void *image, size_t size, const char *origin, char *err, size_t errn);
int  module_unload(const char *name);
int  module_list(struct module_status *out, int max);
bool module_loaded(const char *name);

#endif
