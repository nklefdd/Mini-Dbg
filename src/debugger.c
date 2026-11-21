#include "debugger.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

void print_error(const char *operation)
{
    DWORD code = GetLastError();
    char message[512] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                   message, (DWORD)sizeof(message), NULL);
    message[strcspn(message, "\r\n")] = '\0';
    fprintf(stderr, "%s failed (Windows error %lu). %s\n", operation, code, message);
    SetLastError(code);
}

bool read_memory(HANDLE process, DWORD64 address, void *buffer, SIZE_T length)
{
    SIZE_T count = 0;
    if (!ReadProcessMemory(process, (LPCVOID)(uintptr_t)address, buffer, length, &count))
        return false;
    if (count != length) {
        SetLastError(ERROR_PARTIAL_COPY);
        return false;
    }
    return true;
}

bool write_byte(HANDLE process, DWORD64 address, BYTE value)
{
    void *location = (void *)(uintptr_t)address;
    DWORD old_protection = 0, ignored = 0;
    SIZE_T written = 0;
    MEMORY_BASIC_INFORMATION region;
    if (!VirtualQueryEx(process, location, &region, sizeof(region))) {
        print_error("VirtualQueryEx");
        return false;
    }
    if (region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        SetLastError(ERROR_NOACCESS);
        print_error("Write breakpoint byte");
        return false;
    }
    if (!VirtualProtectEx(process, location, 1, PAGE_EXECUTE_READWRITE, // temporarily allow writes to the code page
                          &old_protection)) {
        print_error("VirtualProtectEx");
        return false;
    }
    bool success = WriteProcessMemory(process, location, &value, 1, &written)
                   && written == 1;
    if (!success)
        print_error("WriteProcessMemory");
    if (!VirtualProtectEx(process, location, 1, old_protection, &ignored)) {
        print_error("Restore memory protection");
        success = false;
    }
    if (!FlushInstructionCache(process, location, 1)) { // flush cached instructions after modifying code
        print_error("FlushInstructionCache");
        success = false;
    }
    return success;
}

bool get_context(HANDLE thread, CONTEXT *context)
{
    memset(context, 0, sizeof(*context));
    context->ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, context)) {
        print_error("GetThreadContext");
        return false;
    }
    return true;
}

int find_breakpoint(const Debugger *debugger, DWORD64 address)
{
    for (int i = 0; i < MAX_BREAKPOINTS; ++i)
        if (debugger->breakpoints[i].used &&
            debugger->breakpoints[i].address == address)
            return i;
    return -1;
}

bool add_breakpoint(Debugger *debugger, DWORD64 address)
{
    if (find_breakpoint(debugger, address) >= 0) {
        puts("A breakpoint already exists at this address.");
        return true;
    }
    int slot = -1;
    for (int i = 0; i < MAX_BREAKPOINTS; ++i)
        if (!debugger->breakpoints[i].used) {
            slot = i;
            break;
        }
    if (slot < 0) {
        printf("Breakpoint limit: %d.\n", MAX_BREAKPOINTS);
        return true;
    }
    MEMORY_BASIC_INFORMATION region;
    if (!VirtualQueryEx(debugger->process, (LPCVOID)(uintptr_t)address,
                        &region, sizeof(region))) {
        print_error("VirtualQueryEx");
        return true;
    }
    DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                       PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if (region.State != MEM_COMMIT || !(region.Protect & executable) ||
        (region.Protect & PAGE_GUARD)) {
        puts("The breakpoint address must be in executable memory.");
        return true;
    }
    BYTE original = 0;
    if (!read_memory(debugger->process, address, &original, 1)) {
        print_error("ReadProcessMemory");
        return true;
    }
    if (original == INT3_OPCODE) {
        puts("This address already contains INT3.");
        return true;
    }
    if (!write_byte(debugger->process, address, INT3_OPCODE)) {
        BYTE check = 0;
        if (read_memory(debugger->process, address, &check, 1) && check == INT3_OPCODE)
            // roll back a half-applied write
            write_byte(debugger->process, address, original);
        puts("Breakpoint was not set.");
        return true;
    }
    debugger->breakpoints[slot] = (Breakpoint){
        address, (DWORD64)(uintptr_t)region.AllocationBase, original, true, true, 0
    };
    ui_color(UI_SUCCESS);
    printf("Breakpoint %d set at 0x%016" PRIX64 ".\n", slot + 1, (uint64_t)address);
    ui_color(UI_RESET);
    return true;
}

bool disarm_breakpoint(Debugger *debugger, int index, HANDLE thread)
{
    Breakpoint *breakpoint = &debugger->breakpoints[index];
    CONTEXT context;
    if (!get_context(thread, &context) ||
        !write_byte(debugger->process, breakpoint->address, breakpoint->original))
        return false;
    breakpoint->armed = false;
    context.Rip = breakpoint->address; // rewind RIP to execute the restored instruction
    if (!SetThreadContext(thread, &context)) {
        print_error("SetThreadContext");
        return false;
    }
    debugger->pending_breakpoint = index; // rearm this breakpoint after the next instruction
    return true;
}

bool rearm_breakpoint(Debugger *debugger)
{
    if (debugger->pending_breakpoint < 0)
        return true;
    Breakpoint *breakpoint = &debugger->breakpoints[debugger->pending_breakpoint];
    if (!write_byte(debugger->process, breakpoint->address, INT3_OPCODE))
        return false;
    breakpoint->armed = true;
    debugger->pending_breakpoint = -1;
    return true;
}