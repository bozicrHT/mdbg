#define _GNU_SOURCE // for 'sigabbrev_np'
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#include <elf.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

#include <sys/wait.h>
#include <sys/ptrace.h>

#define MAX_LENGTH  1024
#define NOT_IMPLEMENTED(arg) \
    fprintf(stdout, "NOT IMPLEMENTED:%d: %s\n", __LINE__, arg);

const char *prompt = "(mdbg) ";

static void wait_for_debugee(pid_t pid);

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

            pid_t pid;
            if ((pid = fork()) < 0)
                die("Error: fork failed in '%s'", __func__);

            if (pid == 0) { // Child process, execute debugee
                if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0)
                    die("Error: tracing failed in '%s'", __func__);

                if (execl(argv[1], argv[1], NULL) < 0)
                    die("Error: execl failed in '%s'", __func__);
            }

            int status;
            if (waitpid(pid, &status, 0) < 0)
                die("Error: waitpid failed in '%s'", __func__);

            if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP)
                die("Error: Unexpected initial process state");

            // Debugee is stopped
            printf("[Inferior (%d) stopped by 'TRACEME' call, continuing]\n", pid);
            if (ptrace(PTRACE_CONT, pid, NULL, NULL) < 0)
                die("Error: PTRACE_CONT failed in '%s'", __func__);

            wait_for_debugee(pid);

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

static void
wait_for_debugee(pid_t pid)
{
    int status, options = 0;
    if (waitpid(pid, &status, options) < 0)
        die("Error: waitpid failed in '%s'", __func__);

    if (WIFEXITED(status)) {
        printf("[Inferior (%d) exited normally (code %d)]\n",
                pid, WEXITSTATUS(status));
    } else if (WIFSTOPPED(status)) {
        int sig = WSTOPSIG(status);
        printf("[Inferior (%d) stopped by signal %d (%s)]\n",
                pid, sig, sigabbrev_np(sig));
    }
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
