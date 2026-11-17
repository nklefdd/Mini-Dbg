#include "../src/debugger.h"

#include <assert.h>
#include <stdio.h>

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
    const char *invalid[] = {
        "", " ", "help", "regs extra", "step now", "continue x", "quit x",
        "break", "break -1", "break +1", "break 0x", "break xyz", "break 1z",
        "break 10000000000000000", "break 12 34", "mem 1", "mem 1 0", "mem 1 -1",
        "mem 1 257", "mem 1 0x10", "mem 1 1 x", "mem FFFFFFFFFFFFFFFF 2",
        "mem 1 999999999999999999999999999", "a b c d e",
        "break 11111111111111111111111111111111111111111111111111111111111111111"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(parse_command(invalid[i]).kind == COMMAND_INVALID);
    puts("Command parser: all checks passed.");
    return 0;
}
