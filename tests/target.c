#include <windows.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#include <intrin.h>
#define BREAK_HERE() __debugbreak()
#define NOINLINE __declspec(noinline)
#else
#define BREAK_HERE() __asm__ volatile("int3")
#define NOINLINE __attribute__((noinline))
#endif

static volatile LONG visits;

static NOINLINE void checkpoint(void)
{
    InterlockedIncrement(&visits);
}

static DWORD WINAPI worker(void *argument)
{
    (void)argument;
    for (int i = 0; i < 3; ++i)
        checkpoint();
    return 0;
}

static LONG CALLBACK handle_exception(EXCEPTION_POINTERS *info)
{
    return info->ExceptionRecord->ExceptionCode == 0xE0000042u
           ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

static int memory_target(void)
{
    SYSTEM_INFO system;
    GetSystemInfo(&system);
    SIZE_T page_size = system.dwPageSize;
    BYTE *pages = VirtualAlloc(NULL, page_size * 3, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!pages)
        return 94;
    memcpy(pages + page_size - 8, "MiniDbg!", 8);
    DWORD previous;
    if (!VirtualProtect(pages + page_size, page_size, PAGE_NOACCESS, &previous) ||
        !VirtualProtect(pages + page_size * 2, page_size, PAGE_READWRITE | PAGE_GUARD, &previous))
        return 95;
    printf("mixed = 0x%016" PRIXPTR "\n", (uintptr_t)(pages + page_size - 8));
    printf("guard = 0x%016" PRIXPTR "\n", (uintptr_t)(pages + page_size * 2));
    BREAK_HERE();
    MEMORY_BASIC_INFORMATION region;
    if (!VirtualQuery(pages + page_size * 2, &region, sizeof(region)))
        return 96;
    bool guard_intact = (region.Protect & PAGE_GUARD) != 0;
    printf("guard intact = %d\n", guard_intact);
    VirtualFree(pages, 0, MEM_RELEASE);
    return guard_intact ? 0 : 97;
}

static int instruction_target(void)
{
    BYTE *code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!code)
        return 94;
    const BYTE instructions[] = {0x90, 0x90, 0xC3};
    memcpy(code, instructions, sizeof(instructions));
    DWORD previous;
    if (!VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &previous) ||
        !FlushInstructionCache(GetCurrentProcess(), code, sizeof(instructions)))
        return 95;
    void (*function)(void);
    _Static_assert(sizeof(function) == sizeof(code), "Windows x64 function pointer width");
    memcpy(&function, &code, sizeof(function));
    printf("probe = 0x%016" PRIXPTR "\n", (uintptr_t)code);
    BREAK_HERE();
    function();
    function();
    MEMORY_BASIC_INFORMATION region;
    if (!VirtualQuery(code, &region, sizeof(region)))
        return 96;
    printf("code protection = %lu\n", region.Protect);
    bool unchanged = region.Protect == PAGE_EXECUTE_READ;
    VirtualFree(code, 0, MEM_RELEASE);
    return unchanged ? 0 : 97;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    char mode[32] = {0};
    GetEnvironmentVariableA("MINIDBG_TEST_MODE", mode, sizeof(mode));
    if (mode[0] == 'm')
        return memory_target();
    if (mode[0] == 'p')
        return instruction_target();
    if (mode[0] == 'a') {
        printf("argc = %d; argv0 = %s\n", argc, argv[0]);
        return argc == 1 ? 0 : 98;
    }
    if (mode[0] == 'c') {
        volatile int *invalid = (volatile int *)(uintptr_t)1;
        *invalid = 42;
        return 99;
    }
    if (mode[0] == 'h') {
        void *handler = AddVectoredExceptionHandler(1, handle_exception);
        if (!handler)
            return 90;
        RaiseException(0xE0000042u, 0, 0, NULL);
        RemoveVectoredExceptionHandler(handler);
        puts("Exception handled by target.");
        return 7;
    }
    if (mode[0] == 'e')
        return 7;
    printf("checkpoint = 0x%016" PRIXPTR "\n", (uintptr_t)checkpoint);
    if (IsDebuggerPresent())
        BREAK_HERE();
    HANDLE threads[2];
    for (int i = 0; i < 2; ++i) {
        threads[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (!threads[i])
            return 91;
    }
    if (WaitForMultipleObjects(2, threads, TRUE, INFINITE) != WAIT_OBJECT_0)
        return 92;
    for (int i = 0; i < 2; ++i)
        CloseHandle(threads[i]);
    printf("visits = %ld\n", visits);
    return visits == 6 ? 0 : 93;
}
