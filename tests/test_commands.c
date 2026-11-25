#include "../src/debugger.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    assert(parse_command("continue\n").kind == COMMAND_CONTINUE);
    assert(parse_command(" \tstep\r\n").kind == COMMAND_STEP);
    assert(parse_command("regs").kind == COMMAND_REGS);
    assert(parse_command("quit\n").kind == COMMAND_QUIT);
    Command command = parse_command("break 0x401136");
    assert(command.kind == COMMAND_BREAK && command.address == 0x401136);
    command = parse_command("break 0000000140001000");
    assert(command.kind == COMMAND_BREAK && command.address == 0x140001000);
    command = parse_command("mem FF 256");
    assert(command.kind == COMMAND_MEM && command.address == 255 && command.length == 256);
    assert(parse_command("mem FFFFFFFFFFFFFFFF 1").kind == COMMAND_MEM);

    command = parse_command("\t break \t 0x10 \t");
    assert(command.kind == COMMAND_BREAK && command.address == 0x10);
    command = parse_command("mem  10   4");
    assert(command.kind == COMMAND_MEM && command.address == 0x10 && command.length == 4);

    command = parse_command("break 0XDEADbeef");
    assert(command.kind == COMMAND_BREAK && command.address == 0xDEADBEEF);

    command = parse_command("break 0");
    assert(command.kind == COMMAND_BREAK && command.address == 0);
    command = parse_command("break FFFFFFFFFFFFFFFF");
    assert(command.kind == COMMAND_BREAK && command.address == 0xFFFFFFFFFFFFFFFFULL);

    command = parse_command("mem 10 256");
    assert(command.kind == COMMAND_MEM && command.address == 0x10 && command.length == 256);
    command = parse_command("mem 10 0256");
    assert(command.kind == COMMAND_MEM && command.address == 0x10 && command.length == 256);

    const char *invalid[] = {
        "", " ", "help", "regs extra", "step now", "continue x", "quit x",
        "break", "break -1", "break +1", "break 0x", "break xyz", "break 1z",
        "break 10000000000000000", "break 12 34", "mem 1", "mem 1 0", "mem 1 -1",
        "mem 1 257", "mem 1 0x10", "mem 1 1 x", "mem FFFFFFFFFFFFFFFF 2",
        "mem 1 999999999999999999999999999", "a b c d e",
        "break 11111111111111111111111111111111111111111111111111111111111111111",
        "BREAK 10", "Continue", "STEP",
        "c", "s", "exit", "?",
        "break 1_0", "break 1,0", "mem 10, 4", "mem 10 0x4",
        "break 1 2 3",
        "break entry", "break rip", "break base+10",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(parse_command(invalid[i]).kind == COMMAND_INVALID);

    char boundary_ok[128] = "break ";
    for (int i = 0; i < 61; ++i) boundary_ok[6 + i] = '0';
    boundary_ok[6 + 61] = '1';
    boundary_ok[6 + 62] = '0';
    boundary_ok[6 + 63] = '\0';
    command = parse_command(boundary_ok);
    assert(command.kind == COMMAND_BREAK && command.address == 0x10);

    char boundary_over[128] = "break ";
    for (int i = 0; i < 62; ++i) boundary_over[6 + i] = '0';
    boundary_over[6 + 62] = '1';
    boundary_over[6 + 63] = '0';
    boundary_over[6 + 64] = '\0';
    assert(parse_command(boundary_over).kind == COMMAND_INVALID);

    puts("Command parser: all checks passed.");
    return 0;
}
