/* A tiny ZenithOS user program. Programs like this live in /bin
 * and run in ring 3 with their own address space. */
#include "zenith.h"

int main(int argc, char **argv)
{
    printf("Hello from user space!\n");
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = %s\n", i, argv[i]);

    /* ask the kernel how long it has been running */
    unsigned long ms = uptime();
    printf("The system has been up for %lu ms.\n", ms);
    return 0;
}
