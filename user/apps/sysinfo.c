/* sysinfo: ask the kernel about the machine. */
#include <zenith.h>

static void bar(unsigned long used, unsigned long total)
{
    int n = (int)(used * 30 / (total ? total : 1));
    printf("  [");
    for (int i = 0; i < 30; i++) printf(i < n ? "\x1b[96m#\x1b[0m" : "\x1b[90m.\x1b[0m");
    printf("]\n");
}

int main(void)
{
    struct z_sysinfo si;
    struct z_time t;
    sysinfo(&si);
    get_time(&t);
    printf("\x1b[1m%s %s\x1b[0m  (via the SYS_SYSINFO system call)\n\n", si.os, si.version);
    printf("  CPU       %s\n", si.cpu);
    printf("  Cores     %d\n", si.ncpus);
    printf("  Memory    %lu MiB used of %lu MiB\n", (si.mem_total - si.mem_free) >> 20, si.mem_total >> 20);
    bar(si.mem_total - si.mem_free, si.mem_total);
    printf("  Threads   %d\n", si.threads);
    printf("  Screen    %d x %d\n", si.screen_w, si.screen_h);
    printf("  Uptime    %lu s\n", si.uptime_ms / 1000);
    printf("  Time      %04d-%02d-%02d %02d:%02d:%02d\n", t.year, t.month, t.day, t.hour, t.minute, t.second);
    return 0;
}
