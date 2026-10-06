#define _GNU_SOURCE // for 'sigabbrev_np'
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

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
const char *debuggee_path = NULL;

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

/// Helpers
static bool get_registers(pid_t pid, struct user_regs_struct *regs);
static bool set_registers(pid_t pid, struct user_regs_struct *regs);
static void print_register_value(Register reg, uint64_t value);
static bool parse_register_command(const char *line, char *reg_name);
static bool parse_set_command(const char *line, char *reg_name, uint64_t *value);

static void print_registers(pid_t pid);
static void print_register_by_name(pid_t pid, const char *reg_name);
static void set_register_value(pid_t pid, const char *reg_name, uint64_t value);
static uint64_t get_register_value(pid_t pid, Register reg);
static Register find_register(const char *reg_name);

typedef struct {
    //pid_t pid;

    bool enabled;
    uint8_t original_byte;
    intptr_t address;
} Breakpoint;

#define MAX_BREAKPOINTS 64
Breakpoint    g_breakpoints[MAX_BREAKPOINTS] = {0};
static size_t g_breakpoint_count = 0;

static Breakpoint *find_breakpoint(intptr_t addr);
static bool install_breakpoint(Breakpoint *bp);
static bool add_breakpoint(intptr_t address);
static bool disable_breakpoint(Breakpoint *bp);
static bool enable_breakpoint(Breakpoint *bp);
static void step_over_breakpoint(void);
static bool print_if_hit_breakpoint(pid_t pid);

typedef enum {
    NO_PROCESS,
    RUNNING,
    STOPPED,
    EXITED,
} Process_State;
Process_State g_pstate = NO_PROCESS;
pid_t g_debuggee_pid = -1;

static pid_t launch(const char *file_path);
static void run_program(void);
static void continue_execution(void);
static void wait_for_debuggee(pid_t pid);
static bool kill_debuggee(void);
static void handle_command(const char *line);

static bool process_is_alive();
static bool process_is_stopped();

static void die(const char *fmt, ...);
static bool check_if_elf(const char *file_path);
static bool is_command(const char *line, const char *command);
static void help(void);

int main(int argc, char **argv)
{
    if (argc != 2)
        die("Usage: %s <a.out>", argv[0]);

    if (!check_if_elf(argv[1]))
        die("Error: provided file '%s' is not ELF", argv[1]);

    debuggee_path = argv[1];

    char line[MAX_LENGTH];
    while (true) {
        fputs(prompt, stdout);

        char *ret = fgets(line, sizeof(line), stdin);
        if (!ret) {
            // FIXME: pressing CTRL-D while program is running.
            break;
        }

        line[strcspn(line, "\n")] = '\0';

        if (!strcmp(line, "quit") || !strcmp(line, "q")) {
            if (process_is_alive() && !kill_debuggee())
                continue;

            break;
        }

        if (*line != '\0')
            handle_command(line);
    }

    return 0;
}

static
void handle_command(const char *line)
{
    if (is_command(line, "run") || is_command(line, "r")) {
        run_program();
        return;
    }

    if (is_command(line, "continue") ||
        is_command(line, "cont") ||
        is_command(line, "c")) {
        continue_execution();
        return;
    }

    if (is_command(line, "breakpoint") ||
        is_command(line, "break") ||
        is_command(line, "b")) {

        char cmd[16];
        uint64_t addr;

        int n = sscanf(line, "%15s %lx", cmd, &addr);
        if (n != 2) {
            printf("usage: breakpoint <address>\n");
            return;
        }

        if (find_breakpoint((intptr_t)addr)) {
            printf("There is already breakpoint at address %#lx\n", addr);
            return;
        }

        if (g_breakpoint_count >= MAX_BREAKPOINTS) {
            printf("Maximum number of breakpoints (%d) reached\n",
                    MAX_BREAKPOINTS);
            return;
        }

        if (add_breakpoint(addr))
            printf("Setting breakpoint on address: %#lx\n", addr);

        return;
    }

    if (is_command(line, "regs")) {
        print_registers(g_debuggee_pid);
        return;
    }

    if (is_command(line, "reg")) {
        char reg_name[16];

        if (parse_register_command(line, reg_name))
            print_register_by_name(g_debuggee_pid, reg_name);

        return;
    }

    if (is_command(line, "set")) {

        char reg_name[16];
        uint64_t value;
        if (parse_set_command(line, reg_name, &value))
            set_register_value(g_debuggee_pid, reg_name, value);

        return;
    }

    if (is_command(line, "kill") ||
        is_command(line, "k")) {
        (void)kill_debuggee();
        return;
    }

    if (is_command(line, "help")) {
        help();
        return;
    }

    printf("Unknown command '%s'\n", line);
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
        return;
    }

    if (WIFSTOPPED(status)) {
        int sig = WSTOPSIG(status);

        if (sig == SIGTRAP) {
            if (print_if_hit_breakpoint(pid))
                return;
        }


        printf("[Inferior (%d) stopped by signal %d (%s)]\n",
                pid, sig, sigabbrev_np(sig));

        g_pstate = STOPPED;
        return;
    }

    die("Unexpected waitpid status '%d'", status);
}

static void
continue_execution(void)
{
    if (!process_is_stopped()) {
        fprintf(stdout, "Inferior is not stopped.\n");
        return;
    }

    step_over_breakpoint();

    if (ptrace(PTRACE_CONT, g_debuggee_pid, NULL, NULL) < 0)
        die("Error: PTRACE_CONT failed in '%s'", __func__);


    g_pstate = RUNNING;
    wait_for_debuggee(g_debuggee_pid);
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
run_program(void)
{
    if (g_pstate == RUNNING) {
        puts("The program is already running.");
        return;
    }

    //Existing stopped process -> restart it
    // For now, simply kill it and create a new inferior
    if (process_is_stopped()) {
        if (!kill_debuggee())
                return;
    }

    // No process -> create one
    // Previous process died -> create a new one

    g_debuggee_pid = launch(debuggee_path);

    // Install breakpoints that were requested before run.
    for (size_t i = 0; i < g_breakpoint_count; i++) {
        Breakpoint *bp = &g_breakpoints[i];

        if (!bp->enabled)
            continue;

        // FIXME: handle error
        (void)install_breakpoint(bp);
    }

    if (ptrace(PTRACE_CONT, g_debuggee_pid, NULL, NULL) < 0)
        die("Error: PTRACE_CONT failed in '%s'", __func__);

    g_pstate = RUNNING;
    wait_for_debuggee(g_debuggee_pid);
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
    if (!process_is_alive()) {
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
        die("Error: SIGKILL falied");

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
bool get_registers(pid_t pid, struct user_regs_struct *regs)
{
    if (ptrace(PTRACE_GETREGS, pid, NULL, regs) < 0) {
        fprintf(stderr, "Error: PTRACE_GETREGS failed: %s\n",
                strerror(errno));
        return false;
    }

    return true;
}

static
bool set_registers(pid_t pid, struct user_regs_struct *regs)
{
    if (ptrace(PTRACE_SETREGS, pid, NULL, regs) < 0) {
        fprintf(stderr, "Error: PTRACE_SETREGS failed: %s\n",
                strerror(errno));
        return false;
    }

    return true;

}

static __attribute__((always_inline))
void print_register_value(Register reg, uint64_t value)
{
    const char *name = g_register_descriptors[reg].name;

    printf("%-15s %#-20lx %lu\n", name, value, value);
}

static
void print_registers(pid_t pid)
{
    if (!process_is_stopped()) {
        printf("Program must be stopped.\n");
        return;
    }

    struct user_regs_struct regs;
    if (!get_registers(pid, &regs))
        return;

    const uint64_t *values = (uint64_t *)(&regs);

    size_t n = ARRAY_SIZE(print_reg_order);
    for (size_t i = 0; i < n; i++) {
        Register reg = print_reg_order[i];
        uint64_t val = values[reg];

        print_register_value(reg, val);
    }
}

static uint64_t
get_register_value(pid_t pid, Register reg)
{
    struct user_regs_struct regs;
    if (!get_registers(pid, &regs))
        die("Something went really wrong...");

    const uint64_t *values = (uint64_t *)(&regs);
    return values[reg];
}

static
Register find_register(const char *reg_name)
{
    for (int i = 0; i < NUM_OF_REGS; i++)
        if (!strcmp(g_register_descriptors[i].name, reg_name))
            return g_register_descriptors[i].reg;

    return INVALID;
}

static
void print_register_by_name(pid_t pid, const char *reg_name)
{
    Register reg = find_register(reg_name);
    if (reg == INVALID) {
        printf("Invalid register '%s'\n", reg_name);
        return;
    }

    uint64_t val = get_register_value(pid, reg);
    print_register_value(reg, val);
}

static
void set_register_value(pid_t pid, const char *reg_name, uint64_t value)
{
    if (!process_is_stopped()) {
        printf("Program must be stopped.\n");
        return;
    }

    Register reg = find_register(reg_name);

    if (reg == INVALID) {
        printf("Invalid register '%s'\n", reg_name);
        return;
    }

    struct user_regs_struct regs;
    if (!get_registers(pid, &regs))
        return;

    ((uint64_t *)&regs)[reg] = value;

    if (!set_registers(pid, &regs))
        return;

    printf("Register '%s' set to %ld\n", reg_name, value);
}

static
bool install_breakpoint(Breakpoint *bp)
{
    errno = 0;
    // ------------------------------------ //
    // Get the 64 bits at the given address //
    // ------------------------------------ //
    long data = ptrace(PTRACE_PEEKDATA, g_debuggee_pid,
            bp->address, NULL);
    if (data == -1 && errno != 0) {
        printf("Reading instruction failed: %s\n", strerror(errno));
        return false;
    }

    // Save the original byte first
    bp->original_byte = (uint8_t)(data & 0xFF);

    // Replace first byte with INT3 = 0xCC
    long patched = (data & ~0xFFL) | 0xCC;

    errno = 0;
    if (ptrace(PTRACE_POKEDATA, g_debuggee_pid,
                (void *)bp->address,
                (void *)patched) < 0) {
        printf("Error: POKEDATA failed %s\n", strerror(errno));
        return false;
    }

    return true;
}

static
bool enable_breakpoint(Breakpoint *bp)
{
    if (!install_breakpoint(bp))
        return false;

    bp->enabled = true;
    return true;
}

static
bool add_breakpoint(intptr_t address)
{
    Breakpoint *bp = &g_breakpoints[g_breakpoint_count];

    bp->address = address;
    bp->enabled = true;

    g_breakpoint_count++;

    if (!process_is_alive())
        return true;

    // XXX: Maybe it should be:
    //       return enable_breakpoint(bp);
    return install_breakpoint(bp);
}

static bool disable_breakpoint(Breakpoint *bp)
{
    // ----------- //
    // Remove int3 //
    // ----------- //
    errno = 0;
    long data = ptrace(PTRACE_PEEKDATA,
                       g_debuggee_pid,
                       (void *)bp->address,
                       NULL);

    if (data == -1 && errno != 0) {
        printf("Error: PTRACE_PEEKDATA failed %s\n", strerror(errno));
        return false;
    }

    long restored = (data & ~0xFFL) | bp->original_byte;
    if (ptrace(PTRACE_POKEDATA, g_debuggee_pid,
                (void *)bp->address,
                (void *)restored) < 0) {
        printf("Error: PTRACE_POKEDATA failed %s", strerror(errno));
        return false;
    }

    bp->enabled = false;

    return true;
}

static
void step_over_breakpoint(void)
{
    struct user_regs_struct regs;
    if (!get_registers(g_debuggee_pid, &regs))
        return;

    intptr_t possible_breakpoint_loc = regs.rip - 1;

    Breakpoint *bp = find_breakpoint(possible_breakpoint_loc);
    if (!bp)
        return;

    if (!disable_breakpoint(bp))
        return;

    // ------------------------------------- //
    // Move RIP back to the INT3 instruction //
    // ------------------------------------- //
    regs.rip--;
    if (!set_registers(g_debuggee_pid, &regs))
        return;

    // -------------------------------- //
    // Single-step original instruction //
    // -------------------------------- //
    if (ptrace(PTRACE_SINGLESTEP, g_debuggee_pid, NULL, NULL) < 0)
        die("Error: PTRACE_SINGLESTEP failed");

    // Wait silently
    if (waitpid(g_debuggee_pid, 0, 0) < 0)
        die("Error: waitpid failed during single-step");

    g_pstate = STOPPED; // Single step leaves process stopped

    if (!enable_breakpoint(bp))
        printf("Failed to re-install breakpoint at %#lx\n",
                bp->address);
}

static
Breakpoint *find_breakpoint(intptr_t addr)
{
    for (size_t i = 0; i < g_breakpoint_count; i++)
        if (g_breakpoints[i].enabled &&
                g_breakpoints[i].address == addr)
            return &g_breakpoints[i];

    return NULL;
}

static
bool print_if_hit_breakpoint(pid_t pid)
{
    struct user_regs_struct regs;
    if (!get_registers(pid, &regs))
        return false;

    intptr_t breakpoint_address = (intptr_t)regs.rip - 1;
    Breakpoint *bp = find_breakpoint(breakpoint_address);
    if (bp && bp->enabled) {
        printf("\n\tBreakpoint hit at 0x%lx\n\n",
                (unsigned long)bp->address);

        g_pstate = STOPPED;
        return true;
    }

    return false;
}

static
bool parse_register_command(const char *line, char *reg_name)
{
    char cmd[16];
    int n = sscanf(line, "%15s %15s", cmd, reg_name);

    if (!strcmp(reg_name, "") || n != 2) {
        printf("usage: reg <register>\n");
        return false;
    }

    reg_name[strlen(reg_name)] = '\0';

    return true;
}

static
bool parse_set_command(const char *line, char *reg_name, uint64_t *value)
{
    char cmd[16];
    int n = sscanf(line, "%15s %15s %ld", cmd, reg_name, value);
    if (n != 3) {
        printf("usage: set <register> <value>\n");
        return false;
    }

    reg_name[strlen(reg_name)] = '\0';

    return true;
}

static
bool process_is_alive()
{
    return g_pstate == RUNNING ||
           g_pstate == STOPPED;
}

static
bool process_is_stopped()
{
    return g_pstate == STOPPED;
}

static
bool is_command(const char *line, const char *command)
{
    while (isspace(*line))
        line++;

    size_t command_length = strlen(command);

    if (strncmp(line, command, command_length) != 0)
        return false;

    char next = line[command_length];

    return next == '\0' || isspace(next);
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

static
void help(void)
{
    printf("List of supported commands:\n\n"
            "help                 -- Print this help.\n"
            "run, r               -- Start debugged program.\n"
            "continue, c          -- Continue program being debugged.\n"
            "breakpoint, break, b -- Set breakpoint at specific location.\n"
            "regs                 -- List all of the registers and their contents.\n"
            "reg <register>       -- List the content of the given register.\n"
            "set <register> <val> -- Set value of the specified register.\n"
            "kill, k              -- Kill execution of program being debugged.\n\n");
}
