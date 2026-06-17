#include <stdio.h>

#ifndef SSU_DEFAULT_ROLE
#define SSU_DEFAULT_ROLE "unknown"
#endif

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    printf("ssu-%s runtime skeleton\n", SSU_DEFAULT_ROLE);
    return 0;
}
