#include <stdio.h>

int main(void)
{
    puts("Hello, World");

    int sum = 0;
    for (int i = 0; i < 100; i++)
        sum += (i+1);

    printf(">Hitting breakpoint from 'test.c'<\n");
    asm("int3");
    printf("Sum is %d\n", sum);

    return 0;
}
