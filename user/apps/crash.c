/* crash: dereferences a NULL pointer on purpose. The kernel isolates the
 * fault and terminates only this process - the system keeps running. */
#include <zenith.h>

int main(void)
{
    printf("Writing to address 0 ...\n");
    volatile int *p = (int *)0;
    *p = 42;
    printf("this line is never reached\n");
    return 0;
}
