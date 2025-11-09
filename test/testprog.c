// simple program to test snapshot/restore plumbing
#include <stdio.h>
#include <unistd.h>

int main(void) {
    int i = 0;
    while (1) {
        printf("testprog: tick %d (pid=%d)\n", i++, getpid());
        fflush(stdout);
        sleep(1);
    }
    return 0;
}
