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

typedef enum {
    NO_PROCESS,
    RUNNING,
    STOPPED,
    EXITED,
} Process_State;
Process_State g_pstate = NO_PROCESS;
pid_t g_debugee_pid = -1;

static pid_t launch(const char *file_path);
static void run_program(const char *file_path);
static bool cleanup(pid_t pid);
static void continue_execution(void);
static void wait_for_debugee(pid_t pid);
static bool kill_debugee(void);

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

            run_program(argv[1]);

        } else if (!strcmp(line, "continue") || !strcmp(line, "cont") ||
                   !strcmp(line, "c")) {

            continue_execution();

        } else if (!strcmp(line, "breakpoint") || !strcmp(line, "break") ||
                   !strcmp(line, "b")) {
            NOT_IMPLEMENTED("breakpoint command");
        } else if (!strcmp(line, "kill") || !strcmp(line, "k")) {

            (void)kill_debugee();

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
        g_pstate = EXITED;
    } else if (WIFSTOPPED(status)) {
        int sig = WSTOPSIG(status);
        printf("[Inferior (%d) stopped by signal %d (%s)]\n",
                pid, sig, sigabbrev_np(sig));
        g_pstate = STOPPED;
    }
}

static void
continue_execution(void)
{
    if (g_pstate != STOPPED) {
        fprintf(stdout, "Inferior is not stopped.\n");
        return;
    }

    if (ptrace(PTRACE_CONT, g_debugee_pid, NULL, NULL) < 0)
        die("Error: PTRACE_CONT failed in '%s'", __func__);


    g_pstate = RUNNING;
    wait_for_debugee(g_debugee_pid);
}

static bool
cleanup(pid_t pid)
{
    if (pid <= 0)
        return false;

    if (g_pstate == RUNNING || g_pstate == STOPPED) {
        printf("A debugging session is active.\n");
        return kill_debugee();
    }

    return false;
}

static pid_t
launch(const char *file_path)
{
    pid_t pid;
    if ((pid = fork()) < 0)
        die("Error: fork failed in '%s'", __func__);

    if (pid == 0) { // Child process, execute debugee
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0)
            die("Error: tracing failed in '%s'", __func__);

        if (execl(file_path, file_path, NULL) < 0)
            die("Error: execl failed in '%s'", __func__);
    }

    int status;
    if (waitpid(pid, &status, 0) < 0)
        die("Error: waitpid failed in '%s'", __func__);

    if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP)
        die("Error: Unexpected initial process state");

    g_pstate = STOPPED;
    return pid;
}

static void
run_program(const char *file_path)
{
    // No process -> create one
    // Previous process died -> create a new one
    if (g_pstate == NO_PROCESS || g_pstate == EXITED) {
        g_debugee_pid = launch(file_path);

        if (ptrace(PTRACE_CONT, g_debugee_pid, NULL, NULL) < 0)
            die("Error: PTRACE_CONT failed in '%s'", __func__);

        g_pstate = RUNNING;

        wait_for_debugee(g_debugee_pid);
    }
    //Existing stopped process -> restart it
    // For now, simply kill it and create a new inferior
    else if (g_pstate == STOPPED) { // STOPPED

        // FIXME: Hacky, refactor it
        if (cleanup(g_debugee_pid)) {
            g_debugee_pid = launch(file_path);

            if (ptrace(PTRACE_CONT, g_debugee_pid, NULL, NULL) < 0)
                die("Error: PTRACE_CONT failed in '%s'", __func__);

            g_pstate = RUNNING;

            wait_for_debugee(g_debugee_pid);
        }

    } else if (g_pstate == RUNNING) {
        printf("The program is already running.\n");
        return;
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

static bool
kill_debugee(void)
{
    if (g_pstate != RUNNING && g_pstate != STOPPED) {
        fprintf(stdout, "The program is not being run.\n");
        return false;
    }

    char answer[8];
    fprintf(stdout, "Kill the program being debugged? (y or n) ");
    if (!fgets(answer, sizeof(answer), stdin))
        return false;

    if (answer[0] != 'Y' && answer[0] != 'y') {
        printf("Program not killed.\n");
        return false;
    }

    printf("[Inferior (%d) will be killed]\n", g_debugee_pid);

    // SIGKILL terminates it regardless of whether it's running or stopped.
    if (kill(g_debugee_pid, SIGKILL) < 0)
        die("Error: SIGSTOP falied");

    // waitpid after SIGKILL is good: you're reaping the child.
    // That's important because killing a child doesn't automatically
    // remove its process entry; the parent needs to waitpid() it.
    if (waitpid(g_debugee_pid, NULL, 0) < 0)
        die("Error: waitpid failed during cleanup");


    g_debugee_pid = -1;
    g_pstate = NO_PROCESS;

    return true;
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
