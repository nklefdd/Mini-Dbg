#include "debugger.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define GENERAL_REGISTER_COUNT (REGISTER_COUNT - 1)
#define EFLAGS_INDEX GENERAL_REGISTER_COUNT

typedef struct {
    const char *name;
    bool is_address;
} RegisterInfo;

typedef struct {
    const char *name;
    unsigned bit;
} FlagInfo;

static const RegisterInfo REGISTERS[GENERAL_REGISTER_COUNT] = {
    {"RAX", false}, {"RBX", false}, {"RCX", false}, {"RDX", false},
    {"RSI", false}, {"RDI", false}, {"RBP", false}, {"RSP", true},
    {"R8 ", false}, {"R9 ", false}, {"R10", false}, {"R11", false},
    {"R12", false}, {"R13", false}, {"R14", false}, {"R15", false},
    {"RIP", true}
};

static const FlagInfo FLAGS[] = {
    {"CF", 0}, {"PF", 2}, {"AF", 4}, {"ZF", 6}, {"SF", 7},
    {"TF", 8}, {"IF", 9}, {"DF", 10}, {"OF", 11}
};

static void print_register(const char *name, uint64_t value, bool changed, bool address)
{
    ui_color(UI_MUTED);
    fputs("  ", stdout);
    ui_color(changed ? UI_CHANGED : address ? UI_ACCENT : UI_RESET);
    printf("%s=0x%016" PRIX64 " %c", name, value, changed ? '*' : ' ');
    ui_color(UI_RESET);
}

static void print_flag_bits(DWORD eflags)
{
    ui_color(UI_MUTED);
    for (size_t i = 0; i < sizeof(FLAGS) / sizeof(FLAGS[0]); ++i)
        printf("  %s=%u", FLAGS[i].name, (unsigned)((eflags >> FLAGS[i].bit) & 1));
    putchar('\n');
    ui_color(UI_RESET);
}

void print_registers(Thread *thread)
{
    CONTEXT context;
    if (!get_context(thread->handle, &context))
        return;
    uint64_t values[REGISTER_COUNT] = {
        context.Rax, context.Rbx, context.Rcx, context.Rdx,
        context.Rsi, context.Rdi, context.Rbp, context.Rsp,
        context.R8, context.R9, context.R10, context.R11,
        context.R12, context.R13, context.R14, context.R15,
        context.Rip, context.EFlags
    };
    ui_heading("REGISTERS");
    printf("  Thread %lu | %s\n\n", thread->id,
           thread->registers_seen ? "* changed since previous regs in this thread" : "first snapshot");
    for (size_t i = 0; i < GENERAL_REGISTER_COUNT; ++i) {
        bool changed = thread->registers_seen && values[i] != thread->previous_registers[i];
        print_register(REGISTERS[i].name, values[i], changed, REGISTERS[i].is_address);
        if (i % 2 == 1 || i == GENERAL_REGISTER_COUNT - 1)
            putchar('\n');
    }
    bool flags_changed = thread->registers_seen &&
                         values[EFLAGS_INDEX] != thread->previous_registers[EFLAGS_INDEX];
    ui_color(flags_changed ? UI_CHANGED : UI_RESET);
    printf("  EFLAGS=0x%08lX %c\n", context.EFlags, flags_changed ? '*' : ' ');
    print_flag_bits(context.EFlags);
    // compare snapshots within the same thread
    memcpy(thread->previous_registers, values, sizeof(values));
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