/* spin: burns CPU forever. Try Ctrl+C, or kill it from the System Monitor. */
#include <zenith.h>

int main(void)
{
    printf("Spinning (pid %d). Press Ctrl+C to stop me.\n", getpid());
    unsigned long n = 0, last = uptime();
    for (;;) {
        n++;
        if ((n & 0xFFFFF) == 0 && uptime() - last >= 1000) {
            last = uptime();
            printf("  still spinning, %lu M iterations\n", n >> 20);
        }
    }
}
