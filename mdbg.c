#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#include <elf.h>
#include <fcntl.h>
#include <unistd.h>

#define MAX_LENGTH  1024
#define NOT_IMPLEMENTED(arg) \
    fprintf(stdout, "NOT IMPLEMENTED:%d: %s\n", __LINE__, arg);

const char *prompt = "(mdbg) ";

static void die(const char *fmt, ...);
static bool check_if_elf(const char *file_path);

int main(int argc, char **argv)
{
    if (argc != 2)
        die("Usage: %s <a.out>", argv[0]);

    if (!check_if_elf(argv[1]))
        die("Error: provided file '%s' is not ELF", argv[1]);

    char line[MAX_LENGTH];
    while (true) {
        fputs(prompt, stdout);

        char *ret = fgets(line, sizeof(line), stdin);
        if (!ret)
            break;

        line[strcspn(line, "\n")] = '\0';
        if (!strcmp(line, "quit") || !strcmp(line, "q"))
            break;

        if (!strcmp(line, "run") || !strcmp(line, "r")) {
            NOT_IMPLEMENTED("run command");
        } else if (!strcmp(line, "continue") || !strcmp(line, "cont") ||
                   !strcmp(line, "c")) {
            NOT_IMPLEMENTED("continue command");
        } else if (!strcmp(line, "breakpoint") || !strcmp(line, "break") ||
                   !strcmp(line, "b")) {
            NOT_IMPLEMENTED("breakpoint command");
        } else {
            fprintf(stdout, "Unknown command '%s'\n", line);
        }
    }

    return 0;
}

static bool
check_if_elf(const char *file_path)
{
    int fd = open(file_path, O_RDONLY);
    if (fd < 0)
        die("Error: file '%s' does not exist", file_path);

    Elf64_Ehdr ehdr;

    if (read(fd, &ehdr, sizeof(ehdr)) != sizeof(ehdr))
        die("Error: reading size mismatch in '%s'", __func__);

    close(fd);
    if (ehdr.e_ident[EI_MAG0] == ELFMAG0 && ehdr.e_ident[EI_MAG1] == ELFMAG1 &&
            ehdr.e_ident[EI_MAG2] == ELFMAG2 && ehdr.e_ident[EI_MAG3] == ELFMAG3)
        return true;

    return false;
}

static void
die(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fputc('\n', stderr);
    exit(1);
}
