#include <windows.h>
#include <inttypes.h>
#include <stdio.h>

#ifdef _MSC_VER
#include <intrin.h>
#define BREAK_HERE() __debugbreak()
#define NOINLINE __declspec(noinline)
#else
#define BREAK_HERE() __asm__ volatile("int3")
#define NOINLINE __attribute__((noinline))
#endif

static volatile int total = 0;

static NOINLINE void checkpoint(int value)
{
    total += value;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("checkpoint = 0x%016" PRIXPTR "\n", (uintptr_t)checkpoint);
    printf("total      = 0x%016" PRIXPTR "\n", (uintptr_t)&total);
    puts("\nCopy these commands into MiniDbg:");
    printf("  break 0x%016" PRIXPTR "\n", (uintptr_t)checkpoint);
    printf("  mem 0x%016" PRIXPTR " 4\n", (uintptr_t)&total);
    puts("Demo pause. Set the breakpoint, then continue. Expected total: 0 -> 1 -> 3 -> 6.");
    if (IsDebuggerPresent())
        BREAK_HERE();
    for (int i = 1; i <= 3; ++i) {
        checkpoint(i);
        printf("[demo] checkpoint(%d) finished; total = %d\n", i, total);
    }
    printf("total = %d\n", total);
    return total == 6 ? 0 : 1;
}
