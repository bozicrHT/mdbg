#include <stdio.h>

int main(void)
{
    int x = 10;
    int y = 4;
    int z = x*y;

    if (z > 35)
        printf("Vece je od 35!\n");
    else
        printf("Nije vece od 35!\n");

    printf("On line %d\n", __LINE__);

    return 0;
}
