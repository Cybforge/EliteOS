/* hello: the classic first program, running in ring 3. */
#include <zenith.h>

int main(int argc, char **argv)
{
    printf("\x1b[1;95mHello from user space!\x1b[0m\n");
    printf("I am process %d, running in ring 3 with my own address space.\n", getpid());
    for (int i = 0; i < argc; i++) printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    printf("The system has been up for %lu ms.\n", uptime());
    return 0;
}
