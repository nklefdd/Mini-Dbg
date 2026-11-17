#include "debugger.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void print_register(const char *name, uint64_t value, bool changed, bool address)
{
    ui_color(UI_MUTED);
    fputs("  ", stdout);
    ui_color(changed ? UI_CHANGED : address ? UI_ACCENT : UI_RESET);
    printf("%s=0x%016" PRIX64 " %c", name, value, changed ? '*' : ' ');
    ui_color(UI_RESET);
}

void print_registers(Thread *thread)
{
    CONTEXT c;
    if (!get_context(thread->handle, &c))
        return;
    const char *names[] = {"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP",
                           "R8 ", "R9 ", "R10", "R11", "R12", "R13", "R14", "R15", "RIP"};
    uint64_t values[REGISTER_COUNT] = {c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp,
                                      c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15,
                                      c.Rip, c.EFlags};
    ui_heading("REGISTERS");
    printf("  Thread %lu | %s\n\n", thread->id,
           thread->registers_seen ? "* changed since previous regs in this thread" : "first snapshot");
    for (size_t i = 0; i < 17; ++i) {
        bool changed = thread->registers_seen && values[i] != thread->previous_registers[i];
        print_register(names[i], values[i], changed, i == 7 || i == 16);
        if (i % 2 == 1 || i == 16)
            putchar('\n');
    }
    bool flags_changed = thread->registers_seen && values[17] != thread->previous_registers[17];
    ui_color(flags_changed ? UI_CHANGED : UI_RESET);
    printf("  EFLAGS=0x%08lX %c\n", c.EFlags, flags_changed ? '*' : ' ');
    ui_color(UI_MUTED);
    printf("  CF=%u  PF=%u  AF=%u  ZF=%u  SF=%u  TF=%u  IF=%u  DF=%u  OF=%u\n",
           (unsigned)((c.EFlags >> 0) & 1), (unsigned)((c.EFlags >> 2) & 1),
           (unsigned)((c.EFlags >> 4) & 1), (unsigned)((c.EFlags >> 6) & 1),
           (unsigned)((c.EFlags >> 7) & 1), (unsigned)((c.EFlags >> 8) & 1),
           (unsigned)((c.EFlags >> 9) & 1), (unsigned)((c.EFlags >> 10) & 1),
           (unsigned)((c.EFlags >> 11) & 1));
    ui_color(UI_RESET);
    memcpy(thread->previous_registers, values, sizeof(values)); // compare snapshots within the same thread
    thread->registers_seen = true;
    putchar('\n');
}

static SIZE_T read_visible_memory(HANDLE process, DWORD64 address, SIZE_T length,
                                 BYTE *bytes, bool *valid)
{
    SIZE_T total = 0;
    for (SIZE_T offset = 0; offset < length;) {
        DWORD64 current = address + offset;
        MEMORY_BASIC_INFORMATION region;
        if (!VirtualQueryEx(process, (LPCVOID)(uintptr_t)current, &region, sizeof(region)))
            break;
        DWORD64 base = (DWORD64)(uintptr_t)region.BaseAddress;
        SIZE_T available = region.RegionSize - (SIZE_T)(current - base);
        SIZE_T chunk = available < length - offset ? available : length - offset;
        if (!chunk)
            break;
        if (region.State == MEM_COMMIT && !(region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) { // skip guard pages to preserve their state
            SIZE_T count = 0;
            ReadProcessMemory(process, (LPCVOID)(uintptr_t)current, bytes + offset, chunk, &count);
            if (count > chunk)
                count = chunk;
            for (SIZE_T i = 0; i < count; ++i)
                valid[offset + i] = true;
            total += count;
        }
        offset += chunk;
    }
    return total;
}

void print_memory(HANDLE process, DWORD64 address, SIZE_T length)
{
    BYTE bytes[MAX_MEMORY] = {0};
    bool valid[MAX_MEMORY] = {false};
    if (!length || length > sizeof(bytes) || address > UINT64_MAX - (length - 1)) {
        puts("Invalid memory range.");
        return;
    }
    SIZE_T total = read_visible_memory(process, address, length, bytes, valid);
    if (!total) {
        SetLastError(ERROR_PARTIAL_COPY);
        print_error("ReadProcessMemory");
        puts("  The requested range is inaccessible or guarded; no protection was changed.");
        return;
    }
    ui_heading("MEMORY");
    CONSOLE_SCREEN_BUFFER_INFO screen;
    SIZE_T columns = 8;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &screen) &&
        screen.srWindow.Right - screen.srWindow.Left + 1 >= 96)
        columns = 16;
    ui_color(UI_MUTED);
    fputs("  ADDRESS             ", stdout);
    for (SIZE_T i = 0; i < columns; ++i)
        printf("%02X ", (unsigned)i);
    puts(" ASCII");
    ui_color(UI_RESET);
    for (SIZE_T offset = 0; offset < length; offset += columns) {
        ui_color(UI_ACCENT);
        printf("  0x%016" PRIX64 "  ", (uint64_t)(address + offset));
        ui_color(UI_RESET);
        for (SIZE_T i = 0; i < columns; ++i) {
            if (offset + i >= length)
                fputs("   ", stdout);
            else if (valid[offset + i])
                printf("%02X ", (unsigned)bytes[offset + i]);
            else {
                ui_color(UI_CHANGED);
                fputs("?? ", stdout);
                ui_color(UI_RESET);
            }
        }
        ui_color(UI_MUTED);
        fputs(" |", stdout);
        for (SIZE_T i = 0; i < columns && offset + i < length; ++i) {
            BYTE value = bytes[offset + i];
            putchar(!valid[offset + i] ? '?' : value >= 32 && value <= 126 ? value : '.');
        }
        puts("|");
        ui_color(UI_RESET);
    }
    printf("  %zu/%zu bytes readable%s\n\n", (size_t)total, (size_t)length,
           total == length ? "" : " | ?? = inaccessible or guarded");
}