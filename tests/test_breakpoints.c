#include "../src/debugger.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static Debugger make_debugger(void)
{
    Debugger debugger;
    memset(&debugger, 0, sizeof(debugger));
    debugger.process = GetCurrentProcess();
    debugger.pending_breakpoint = -1;
    return debugger;
}

static int used_slots(const Debugger *debugger)
{
    int count = 0;
    for (int i = 0; i < MAX_BREAKPOINTS; ++i)
        if (debugger->breakpoints[i].used)
            ++count;
    return count;
}

static BYTE read_byte(DWORD64 address)
{
    BYTE value = 0;
    assert(read_memory(GetCurrentProcess(), address, &value, 1));
    return value;
}

static DWORD WINAPI idle_thread(LPVOID argument)
{
    (void)argument;
    return 0;
}

static void test_empty_table(void)
{
    Debugger debugger = make_debugger();
    assert(find_breakpoint(&debugger, 0x1000) == -1);
    assert(used_slots(&debugger) == 0);
}

static void test_add_and_find(BYTE *code)
{
    Debugger debugger = make_debugger();
    DWORD64 address = (DWORD64)(uintptr_t)code + 4;
    assert(add_breakpoint(&debugger, address));
    int slot = find_breakpoint(&debugger, address);
    assert(slot == 0);
    const Breakpoint *breakpoint = &debugger.breakpoints[slot];
    assert(breakpoint->used && breakpoint->armed);
    assert(breakpoint->address == address);
    assert(breakpoint->original == 0x90);
    assert(breakpoint->hits == 0);
    assert(read_byte(address) == INT3_OPCODE);
    assert(find_breakpoint(&debugger, address + 1) == -1);
}

static void test_duplicate_is_ignored(BYTE *code)
{
    Debugger debugger = make_debugger();
    DWORD64 address = (DWORD64)(uintptr_t)code + 8;
    assert(add_breakpoint(&debugger, address));
    assert(add_breakpoint(&debugger, address));
    assert(used_slots(&debugger) == 1);
    assert(debugger.breakpoints[0].original == 0x90);
}

static void test_existing_int3_is_rejected(BYTE *code)
{
    Debugger debugger = make_debugger();
    DWORD64 address = (DWORD64)(uintptr_t)code + 12;
    assert(read_byte(address) == INT3_OPCODE);
    assert(add_breakpoint(&debugger, address));
    assert(used_slots(&debugger) == 0);
}

static void test_invalid_addresses_are_rejected(BYTE *data)
{
    Debugger debugger = make_debugger();
    assert(add_breakpoint(&debugger, 1));
    assert(add_breakpoint(&debugger, (DWORD64)(uintptr_t)data));
    assert(used_slots(&debugger) == 0);
    assert(read_byte((DWORD64)(uintptr_t)data) == 0x90);
}

static void test_table_limit(BYTE *code)
{
    Debugger debugger = make_debugger();
    DWORD64 base = (DWORD64)(uintptr_t)code + 64;
    for (int i = 0; i < MAX_BREAKPOINTS; ++i)
        assert(add_breakpoint(&debugger, base + (DWORD64)i));
    assert(used_slots(&debugger) == MAX_BREAKPOINTS);
    DWORD64 extra = base + MAX_BREAKPOINTS;
    assert(add_breakpoint(&debugger, extra));
    assert(used_slots(&debugger) == MAX_BREAKPOINTS);
    assert(find_breakpoint(&debugger, extra) == -1);
    assert(read_byte(extra) == 0x90);
    for (int i = 0; i < MAX_BREAKPOINTS; ++i)
        assert(find_breakpoint(&debugger, base + (DWORD64)i) == i);
}

static void test_disarm_and_rearm(BYTE *code)
{
    Debugger debugger = make_debugger();
    DWORD64 address = (DWORD64)(uintptr_t)code + 128;
    assert(add_breakpoint(&debugger, address));
    assert(rearm_breakpoint(&debugger));
    assert(read_byte(address) == INT3_OPCODE);

    HANDLE thread = CreateThread(NULL, 0, idle_thread, NULL, CREATE_SUSPENDED, NULL);
    assert(thread);
    assert(disarm_breakpoint(&debugger, 0, thread));
    assert(!debugger.breakpoints[0].armed);
    assert(debugger.pending_breakpoint == 0);
    assert(read_byte(address) == 0x90);
    CONTEXT context;
    assert(get_context(thread, &context));
    assert(context.Rip == address);

    assert(rearm_breakpoint(&debugger));
    assert(debugger.breakpoints[0].armed);
    assert(debugger.pending_breakpoint == -1);
    assert(read_byte(address) == INT3_OPCODE);
    assert(TerminateThread(thread, 0));
    assert(CloseHandle(thread));
}

int main(void)
{
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    SIZE_T size = system.dwPageSize;
    BYTE *pages = VirtualAlloc(NULL, size * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(pages);
    memset(pages, 0x90, size * 2);
    pages[12] = INT3_OPCODE;
    DWORD old_protection = 0;
    assert(VirtualProtect(pages, size, PAGE_EXECUTE_READ, &old_protection));

    test_empty_table();
    test_add_and_find(pages);
    test_duplicate_is_ignored(pages);
    test_existing_int3_is_rejected(pages);
    test_invalid_addresses_are_rejected(pages + size);
    test_table_limit(pages);
    test_disarm_and_rearm(pages);

    assert(VirtualFree(pages, 0, MEM_RELEASE));
    puts("Breakpoint table: all checks passed.");
    return 0;
}
