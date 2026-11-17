#include "../src/debugger.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    BYTE *pages = VirtualAlloc(NULL, system.dwPageSize * 2,
                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(pages);
    pages[0] = 0x90;
    DWORD old_protection;
    assert(VirtualProtect(pages, system.dwPageSize, PAGE_EXECUTE_READ, &old_protection));
    DWORD64 address = (DWORD64)(uintptr_t)pages;
    assert(write_byte(GetCurrentProcess(), address, 0xCC));
    BYTE byte = 0;
    assert(read_memory(GetCurrentProcess(), address, &byte, 1) && byte == 0xCC);
    MEMORY_BASIC_INFORMATION region;
    assert(VirtualQuery(pages, &region, sizeof(region)));
    assert(region.Protect == PAGE_EXECUTE_READ);
    assert(write_byte(GetCurrentProcess(), address, 0x90));
    assert(read_memory(GetCurrentProcess(), address, &byte, 1) && byte == 0x90);
    assert(VirtualProtect(pages + system.dwPageSize, system.dwPageSize,
                          PAGE_READWRITE | PAGE_GUARD, &old_protection));
    assert(!write_byte(GetCurrentProcess(), address + system.dwPageSize, 0xCC));
    assert(VirtualQuery(pages + system.dwPageSize, &region, sizeof(region)));
    assert(region.Protect == (PAGE_READWRITE | PAGE_GUARD));
    assert(!read_memory(GetCurrentProcess(), 1, &byte, 1));
    assert(!write_byte(GetCurrentProcess(), 1, 0xCC));
    assert(VirtualFree(pages, 0, MEM_RELEASE));
    puts("Memory access: all checks passed.");
    return 0;
}
