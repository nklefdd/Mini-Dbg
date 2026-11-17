#ifndef MINIDBG_H
#define MINIDBG_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

#if !defined(_M_X64) && !defined(__x86_64__)
#error MiniDbg must be built for Windows x64.
#endif

#define MAX_BREAKPOINTS 16
#define MAX_THREADS 64
#define MAX_MEMORY 256
#define TRAP_FLAG 0x100u
#define REGISTER_COUNT 18
#define TARGET_PATH_CAPACITY 32768

typedef struct {
    DWORD64 address;
    DWORD64 module;
    BYTE original;
    bool used;
    bool armed;
    uint64_t hits;
} Breakpoint;

typedef struct {
    DWORD id;
    HANDLE handle;
    bool suspended;
    bool registers_seen;
    uint64_t previous_registers[REGISTER_COUNT];
} Thread;

typedef struct {
    HANDLE process;
    DWORD process_id;
    Thread threads[MAX_THREADS];
    Breakpoint breakpoints[MAX_BREAKPOINTS];
    int pending_breakpoint;
    DWORD stepping_thread;
    DWORD64 step_from;
    DWORD64 image_base;
    DWORD64 entry_point;
    bool visible_step;
    bool quit_requested;
    uint64_t stops;
    uint64_t breakpoint_hits;
    uint64_t user_steps;
} Debugger;

typedef enum {
    COMMAND_INVALID, COMMAND_BREAK, COMMAND_CONTINUE, COMMAND_STEP,
    COMMAND_REGS, COMMAND_MEM, COMMAND_QUIT
} CommandKind;

typedef struct {
    CommandKind kind;
    DWORD64 address;
    SIZE_T length;
} Command;

typedef enum { ACTION_CONTINUE, ACTION_STEP, ACTION_QUIT, ACTION_ERROR } Action;
typedef enum { UI_RESET, UI_ACCENT, UI_CHANGED, UI_MUTED, UI_ERROR, UI_SUCCESS } UiColor;
typedef enum { TARGET_READY, TARGET_CANCELLED, TARGET_ERROR } TargetResult;

TargetResult select_target(const wchar_t *argument, wchar_t *path);

void console_init(void);
void console_restore(void);
void ui_color(UiColor color);
void ui_heading(const char *title);
void print_banner(void);
void print_summary(const Debugger *debugger);

void print_error(const char *operation);
bool read_memory(HANDLE process, DWORD64 address, void *buffer, SIZE_T length);
bool write_byte(HANDLE process, DWORD64 address, BYTE value);
bool get_context(HANDLE thread, CONTEXT *context);
int find_breakpoint(const Debugger *debugger, DWORD64 address);
bool add_breakpoint(Debugger *debugger, DWORD64 address);
bool disarm_breakpoint(Debugger *debugger, int index, HANDLE thread);
bool rearm_breakpoint(Debugger *debugger);
void print_registers(Thread *thread);
void print_memory(HANDLE process, DWORD64 address, SIZE_T length);
Command parse_command(const char *line);
void print_commands(void);
Action command_loop(Debugger *debugger, Thread *thread, bool can_step);

#endif
