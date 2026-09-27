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
#include <sys/user.h>
#include <sys/ptrace.h>

#define MAX_LENGTH  1024
#define ARRAY_SIZE(x)   (sizeof(x)/sizeof(x[0]))
#define NOT_IMPLEMENTED(arg) \
    fprintf(stdout, "NOT IMPLEMENTED:%d: %s\n", __LINE__, arg);

const char *prompt = "(mdbg) ";

typedef enum {
    INVALID = -1,
    R15, R14, R13, R12, RBP, RBX,
    R11, R10, R9,  R8,  RAX, RCX,
    RDX, RSI, RDI, ORIG_RAX, RIP,
    CS, /// unused
    EFLAGS, RSP,
    NUM_OF_REGS,
} Register;

static const struct {
    Register reg;
    /* 3.6.2 DWARF Register Number Mapping:
     * https://www.uclibc.org/docs/psABI-x86_64.pdf */
    int dwarf_num;
    const char *name;
} g_register_descriptors[NUM_OF_REGS] = {
    { R15, 15, "r15" },
    { R14, 14, "r14" },
    { R13, 13, "r13" },
    { R12, 12, "r12" },
    { RBP,  6, "rbp" },
    { RBX,  3, "rbx" },
    { R11, 11, "r11" },
    { R10, 10, "r10" },
    { R9,   9, "r9" },
    { R8,   8, "r8" },
    { RAX,  0, "rax" },
    { RCX,  2, "rcx" },
    { RDX,  1, "rdx" },
    { RSI,  4, "rsi" },
    { RDI,  5, "rdi" },
    { ORIG_RAX, -1, "orig_rax" },
    { RIP, -1, "rip" },
    { CS, 51, "cs" }, /// Unused
    { EFLAGS, 49, "eflags" },
    { RSP, 7, "rsp" },
};

/* Copy gdb's printing order of registers */
static const Register print_reg_order[] = {
    RAX, RBX, RCX, RDX, RSI, RDI, RBP,
    RSP, R8, R9, R10, R11, R12, R13,
    R14, R15, RIP, EFLAGS,
};

static void print_registers(pid_t pid);

typedef enum {
    NO_PROCESS,
    RUNNING,
    STOPPED,
    EXITED,
} Process_State;
Process_State g_pstate = NO_PROCESS;
pid_t g_debuggee_pid = -1;

static pid_t launch(const char *file_path);
static void run_program(const char *file_path);
static bool cleanup(pid_t pid);
static void continue_execution(void);
static void wait_for_debuggee(pid_t pid);
static bool kill_debuggee(void);

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
        if (!strcmp(line, "quit") || !strcmp(line, "q")) {
            if (g_pstate == RUNNING || g_pstate == STOPPED)
                if (!kill_debuggee())
                    continue;
            break;
        }

        if (!strcmp(line, "run") || !strcmp(line, "r")) {

            run_program(argv[1]);

        } else if (!strcmp(line, "continue") || !strcmp(line, "cont") ||
                   !strcmp(line, "c")) {

            continue_execution();

        } else if (!strcmp(line, "breakpoint") || !strcmp(line, "break") ||
                   !strcmp(line, "b")) {
            NOT_IMPLEMENTED("breakpoint command");
        } else if (!strcmp(line, "regs")) {
            print_registers(g_debuggee_pid);
        } else if (!strcmp(line, "kill") || !strcmp(line, "k")) {

            (void)kill_debuggee();

        } else {
            fprintf(stdout, "Unknown command '%s'\n", line);
        }
    }

    return 0;
}

static void
wait_for_debuggee(pid_t pid)
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

    if (ptrace(PTRACE_CONT, g_debuggee_pid, NULL, NULL) < 0)
        die("Error: PTRACE_CONT failed in '%s'", __func__);


    g_pstate = RUNNING;
    wait_for_debuggee(g_debuggee_pid);
}

static bool
cleanup(pid_t pid)
{
    if (pid <= 0)
        return false;

    if (g_pstate == RUNNING || g_pstate == STOPPED) {
        printf("A debugging session is active.\n");
        return kill_debuggee();
    }

    return false;
}

static pid_t
launch(const char *file_path)
{
    pid_t pid;
    if ((pid = fork()) < 0)
        die("Error: fork failed in '%s'", __func__);

    if (pid == 0) { // Child process, execute debuggee
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
        g_debuggee_pid = launch(file_path);

        if (ptrace(PTRACE_CONT, g_debuggee_pid, NULL, NULL) < 0)
            die("Error: PTRACE_CONT failed in '%s'", __func__);

        g_pstate = RUNNING;

        wait_for_debuggee(g_debuggee_pid);
    }
    //Existing stopped process -> restart it
    // For now, simply kill it and create a new inferior
    else if (g_pstate == STOPPED) { // STOPPED

        // FIXME: Hacky, refactor it
        if (cleanup(g_debuggee_pid)) {
            g_debuggee_pid = launch(file_path);

            if (ptrace(PTRACE_CONT, g_debuggee_pid, NULL, NULL) < 0)
                die("Error: PTRACE_CONT failed in '%s'", __func__);

            g_pstate = RUNNING;

            wait_for_debuggee(g_debuggee_pid);
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
kill_debuggee(void)
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

    printf("[Inferior (%d) will be killed]\n", g_debuggee_pid);

    // SIGKILL terminates it regardless of whether it's running or stopped.
    if (kill(g_debuggee_pid, SIGKILL) < 0)
        die("Error: SIGSTOP falied");

    // waitpid after SIGKILL is good: you're reaping the child.
    // That's important because killing a child doesn't automatically
    // remove its process entry; the parent needs to waitpid() it.
    if (waitpid(g_debuggee_pid, NULL, 0) < 0)
        die("Error: waitpid failed during cleanup");


    g_debuggee_pid = -1;
    g_pstate = NO_PROCESS;

    return true;
}

static
void print_registers(pid_t pid)
{
    if (g_pstate != RUNNING && g_pstate != STOPPED) {
        printf("Program is not being run.\n");
        return;
    }

    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) < 0)
        die("Error: reading registers failed");

    const uint64_t *values = (uint64_t *)(&regs);

    size_t n = ARRAY_SIZE(print_reg_order);
    for (size_t i = 0; i < n; i++) {
        Register reg = print_reg_order[i];
        uint64_t val = values[reg];
        printf("%-15s %#-20lx %lu\n", g_register_descriptors[reg].name, val, val);
    }
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
