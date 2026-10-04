#include <stdio.h>

int sum(int a, int b)
{
    int sum = 0;
    for (int i = a; i <= b; i++)
        sum += i;
    return sum;
}

int main(void)
{
    puts("Hello, World");

    printf("Sum is %d\n", sum(1, 100));

    return 0;
}
