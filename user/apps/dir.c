/* dir: a user-space directory lister built on SYS_READDIR. */
#include <zenith.h>

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : ".";
    struct z_dirent d;
    int n = 0;
    for (int i = 0; readdir(path, i, &d) == 0; i++, n++) {
        if (d.type == 2) printf("\x1b[94m%-24s\x1b[0m  <dir>\n", d.name);
        else printf("%-24s  %lu bytes\n", d.name, d.size);
    }
    printf("%d entries\n", n);
    return 0;
}
