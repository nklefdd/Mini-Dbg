#include "debugger.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static bool parse_number(const char *text, int base, uint64_t *value)
{
    if (!text[0] || text[0] == '-' || text[0] == '+')
        return false;
    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(text, &end, base);
    if (errno == ERANGE || end == text || *end)
        return false;
    *value = (uint64_t)parsed;
    return true;
}

static bool alias_value(const Debugger *debugger, const Thread *thread, // resolve aliases using the current process state
                        const char *name, size_t length, uint64_t *value)
{
    if (length == 5 && !strncmp(name, "entry", 5)) {
        *value = debugger->entry_point;
        return *value != 0;
    }
    if (length > 5 && !strncmp(name, "base+", 5)) {
        char number[64];
        uint64_t offset = 0;
        if (length - 5 >= sizeof(number) || !debugger->image_base)
            return false;
        memcpy(number, name + 5, length - 5);
        number[length - 5] = '\0';
        if (!parse_number(number, 16, &offset) || // reject offsets that would wrap around the address space
            offset > UINT64_MAX - debugger->image_base)
            return false;
        *value = debugger->image_base + offset;
        return true;
    }
    CONTEXT c;
    if (!thread || length < 2 || length > 3 || !get_context(thread->handle, &c))
        return false;
    char key[4] = {0};
    for (size_t i = 0; i < length; ++i)
        key[i] = (char)tolower((unsigned char)name[i]);
    static const struct { const char *name; size_t offset; } registers[] = {
        {"rax", offsetof(CONTEXT, Rax)}, {"rbx", offsetof(CONTEXT, Rbx)},
        {"rcx", offsetof(CONTEXT, Rcx)}, {"rdx", offsetof(CONTEXT, Rdx)},
        {"rsi", offsetof(CONTEXT, Rsi)}, {"rdi", offsetof(CONTEXT, Rdi)},
        {"rbp", offsetof(CONTEXT, Rbp)}, {"rsp", offsetof(CONTEXT, Rsp)},
        {"r8", offsetof(CONTEXT, R8)},   {"r9", offsetof(CONTEXT, R9)},
        {"r10", offsetof(CONTEXT, R10)}, {"r11", offsetof(CONTEXT, R11)},
        {"r12", offsetof(CONTEXT, R12)}, {"r13", offsetof(CONTEXT, R13)},
        {"r14", offsetof(CONTEXT, R14)}, {"r15", offsetof(CONTEXT, R15)},
        {"rip", offsetof(CONTEXT, Rip)}
    };
    for (size_t i = 0; i < sizeof(registers) / sizeof(registers[0]); ++i)
        if (!strcmp(key, registers[i].name)) {
            memcpy(value, (const char *)&c + registers[i].offset, sizeof(*value));
            return true;
        }
    return false;
}

static const char *resolve_alias(const Debugger *debugger, const Thread *thread, // reuse the numeric parser after resolving aliases
                                 const char *line, char *out, size_t size)
{
    const char *p = line + strspn(line, " \t");
    size_t command_length = strcspn(p, " \t\r\n");
    if (!(command_length == 5 && !strncmp(p, "break", 5)) &&
        !(command_length == 3 && !strncmp(p, "mem", 3)))
        return line;
    const char *arg = p + command_length;
    arg += strspn(arg, " \t");
    size_t arg_length = strcspn(arg, " \t\r\n");
    uint64_t value = 0;
    if (!alias_value(debugger, thread, arg, arg_length, &value))
        return line;
    snprintf(out, size, "%.*s %" PRIX64 "%s", (int)command_length, p, value, arg + arg_length);
    return out;
}

Command parse_command(const char *line)
{
    Command result = {COMMAND_INVALID, 0, 0};
    char tokens[4][64] = {{0}};
    int count = 0;
    while (*line) {
        while (isspace((unsigned char)*line))
            ++line;
        if (!*line)
            break;
        if (count == 4)
            return result;
        size_t length = 0;
        while (*line && !isspace((unsigned char)*line)) {
            if (length == sizeof(tokens[0]) - 1)
                return result;
            tokens[count][length++] = *line++;
        }
        ++count;
    }
    if (count == 1) {
        if (!strcmp(tokens[0], "continue")) result.kind = COMMAND_CONTINUE;
        else if (!strcmp(tokens[0], "step")) result.kind = COMMAND_STEP;
        else if (!strcmp(tokens[0], "regs")) result.kind = COMMAND_REGS;
        else if (!strcmp(tokens[0], "quit")) result.kind = COMMAND_QUIT;
    } else if (count == 2 && !strcmp(tokens[0], "break")) {
        uint64_t address = 0;
        if (parse_number(tokens[1], 16, &address)) {
            result.kind = COMMAND_BREAK;
            result.address = address;
        }
    } else if (count == 3 && !strcmp(tokens[0], "mem")) {
        uint64_t address = 0, length = 0;
        if (parse_number(tokens[1], 16, &address) &&
            parse_number(tokens[2], 10, &length) &&
            length > 0 && length <= MAX_MEMORY && address <= UINT64_MAX - (length - 1)) {
            result.kind = COMMAND_MEM;
            result.address = address;
            result.length = (SIZE_T)length;
        }
    }
    return result;
}

static void print_command_row(const char *command, const char *description, const char *hint)
{
    ui_color(UI_ACCENT);
    printf("  %-26s ", command);
    ui_color(UI_RESET);
    fputs(description, stdout);
    if (hint) {
        ui_color(UI_MUTED);
        printf(" %s", hint);
        ui_color(UI_RESET);
    }
    putchar('\n');
}

void print_commands(void)
{
    print_command_row("break <hex-address>", "Set a persistent breakpoint",
                      "(also: entry | base+<hex> | rip | rsp | rax ...)");
    print_command_row("continue", "Run until the next stop", NULL);
    print_command_row("step", "Execute ONE CPU instruction (not a C line)", NULL);
    print_command_row("regs", "CPU registers and changes (no C variables)", NULL);
    print_command_row("mem <hex-address> <1..256>", "Hex + ASCII memory view", NULL);
    print_command_row("quit", "Terminate the target and close", NULL);
}

Action command_loop(Debugger *debugger, Thread *thread, bool can_step)
{
    char line[256];
    for (;;) {
        ui_color(UI_ACCENT);
        fputs("(mdbg) ", stdout);
        ui_color(UI_RESET);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            if (ferror(stdin)) {
                fputs("Cannot read debugger commands from standard input.\n", stderr);
                return ACTION_ERROR;
            }
            return ACTION_QUIT;
        }
        if (!strchr(line, '\n') && !feof(stdin)) { // discard the remainder so it cannot become another command
            int character;
            while ((character = getchar()) != '\n' && character != EOF) {}
            puts("Command is too long.");
            continue;
        }
        if (line[strspn(line, " \t\r\n")] == '\0')
            continue;
        char resolved[320];
        Command command = parse_command(resolve_alias(debugger, thread, line, resolved, sizeof(resolved)));
        switch (command.kind) {
        case COMMAND_BREAK:
            if (!add_breakpoint(debugger, command.address))
                return ACTION_ERROR;
            break;
        case COMMAND_CONTINUE:
            return ACTION_CONTINUE;
        case COMMAND_STEP:
            if (can_step)
                return ACTION_STEP;
            puts("Cannot step an exception. Use continue to pass it to the program, or quit.");
            break;
        case COMMAND_REGS:
            print_registers(thread);
            break;
        case COMMAND_MEM:
            print_memory(debugger->process, command.address, command.length);
            break;
        case COMMAND_QUIT:
            return ACTION_QUIT;
        default:
            puts("Invalid command.");
            print_commands();
            break;
        }
    }
}