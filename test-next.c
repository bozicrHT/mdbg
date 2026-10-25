#include <stdio.h>

int main(void)
{
    printf("On line %d\n", __LINE__);
    int a = 5;
    int b = 5;
    printf("%d\n", a + b);
    printf("On line %d\n", __LINE__);
    return 0;
}
