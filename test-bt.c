#include <stdio.h>

void end()
{
    asm("int3");

    puts("end");
}

void g()
{
    end();
}

void f()
{
    g();
}

int main()
{
    f();

    return 0;
}

