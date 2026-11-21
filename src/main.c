#include "debugger.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static Thread *find_thread(Debugger *debugger, DWORD id)
{
    for (int i = 0; i < MAX_THREADS; ++i)
        if (debugger->threads[i].handle && debugger->threads[i].id == id)
            return &debugger->threads[i];
    return NULL;
}

static bool add_thread(Debugger *debugger, DWORD id, HANDLE source)
{
    for (int i = 0; i < MAX_THREADS; ++i) {
        Thread *thread = &debugger->threads[i];
        if (thread->handle)
            continue;
        if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
                             &thread->handle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            print_error("DuplicateHandle");
            return false;
        }
        thread->id = id;
        if (debugger->stepping_thread && id != debugger->stepping_thread) {
            if (SuspendThread(thread->handle) == (DWORD)-1) {
                print_error("SuspendThread");
                CloseHandle(thread->handle);
                thread->handle = NULL;
                return false;
            }
            thread->suspended = true;
        }
        return true;
    }
    fprintf(stderr, "Thread limit reached: %d.\n", MAX_THREADS);
    return false;
}

static bool begin_step(Debugger *debugger, Thread *current, bool visible)
{
    CONTEXT context;
    if (!get_context(current->handle, &context))
        return false;
    // keep other threads paused while INT3 is disarmed
    for (int i = 0; i < MAX_THREADS; ++i) {
        Thread *thread = &debugger->threads[i];
        if (thread->handle && thread->id != current->id && !thread->suspended) {
            if (WaitForSingleObject(thread->handle, 0) == WAIT_OBJECT_0)
                continue;
            if (SuspendThread(thread->handle) == (DWORD)-1) {
                DWORD error = GetLastError();
                // the thread may exit before its debug event arrives
                if (error == ERROR_ACCESS_DENIED ||
                    WaitForSingleObject(thread->handle, 0) == WAIT_OBJECT_0)
                    continue;
                SetLastError(error);
                print_error("SuspendThread");
                return false;
            }
            thread->suspended = true;
        }
    }
    debugger->step_from = context.Rip;
    context.EFlags |= TRAP_FLAG;
    if (!SetThreadContext(current->handle, &context)) {
        print_error("SetThreadContext");
        return false;
    }
    debugger->stepping_thread = current->id;
    debugger->visible_step = visible;
    return true;
}

static bool finish_step(Debugger *debugger, bool thread_exited)
{
    Thread *current = find_thread(debugger, debugger->stepping_thread);
    if (!rearm_breakpoint(debugger))
        return false;
    if (current && !thread_exited) {
        CONTEXT context;
        if (!get_context(current->handle, &context))
            return false;
        context.EFlags &= ~TRAP_FLAG;
        if (!SetThreadContext(current->handle, &context)) {
            print_error("SetThreadContext");
            return false;
        }
    }
    for (int i = 0; i < MAX_THREADS; ++i) {
        Thread *thread = &debugger->threads[i];
        if (thread->handle && thread->suspended) {
            if (ResumeThread(thread->handle) == (DWORD)-1) {
                DWORD error = GetLastError();
                if (error != ERROR_ACCESS_DENIED &&
                    WaitForSingleObject(thread->handle, 0) != WAIT_OBJECT_0) {
                    SetLastError(error);
                    print_error("ResumeThread");
                    return false;
                }
            }
            thread->suspended = false;
        }
    }
    debugger->stepping_thread = 0;
    return true;
}

static bool rewind_thread(Thread *thread, DWORD64 address)
{
    CONTEXT context;
    if (!get_context(thread->handle, &context))
        return false;
    context.Rip = address;
    if (!SetThreadContext(thread->handle, &context)) {
        print_error("SetThreadContext");
        return false;
    }
    return true;
}

static bool is_debugger_noise(DWORD code)
{
    // OutputDebugString (ANSI and wide) and the MSVC thread-name exception
    return code == 0x40010006u || code == 0x4001000Au || code == 0x406D1388u;
}

static const char *access_name(ULONG_PTR access)
{
    switch (access) {
    case 0: return "read";
    case 1: return "write";
    case 8: return "execute";
    default: return "access";
    }
}

static bool report_step(Debugger *debugger, const Thread *thread)
{
    CONTEXT context;
    if (!get_context(thread->handle, &context))
        return false;
    printf("Step at 0x%016" PRIX64 " (one instruction from 0x%016" PRIX64 ", thread %lu).\n",
           (uint64_t)context.Rip, debugger->step_from, thread->id);
    BYTE next[8];
    if (read_memory(debugger->process, context.Rip, next, sizeof(next))) {
        fputs("  Next instruction bytes:", stdout);
        for (size_t i = 0; i < sizeof(next); ++i)
            printf(" %02X", next[i]);
        putchar('\n');
    }
    ++debugger->user_steps;
    return true;
}

static bool report_breakpoint(Debugger *debugger, const Thread *thread, DWORD64 address,
                              bool *initial_breakpoint)
{
    int index = find_breakpoint(debugger, address);
    if (index < 0 || !debugger->breakpoints[index].armed) {
        printf("%s at 0x%016" PRIX64 " (thread %lu).\n",
               *initial_breakpoint ? "Initial breakpoint" : "Program breakpoint",
               (uint64_t)address, thread->id);
        *initial_breakpoint = false;
        return true;
    }
    if (!disarm_breakpoint(debugger, index, thread->handle))
        return false;
    printf("Hit breakpoint %d at 0x%016" PRIX64 " (thread %lu).\n",
           index + 1, (uint64_t)address, thread->id);
    ++debugger->breakpoint_hits;
    ++debugger->breakpoints[index].hits;
    printf("  Hit count: %" PRIu64 " | original instruction restored\n",
           debugger->breakpoints[index].hits);
    return true;
}

static void report_exception(const EXCEPTION_DEBUG_INFO *info, const Thread *thread)
{
    const EXCEPTION_RECORD *record = &info->ExceptionRecord;
    DWORD code = record->ExceptionCode;
    ui_color(UI_ERROR);
    printf("Exception 0x%08lX at 0x%016" PRIX64 " (%s chance, thread %lu).\n",
           code, (uint64_t)(uintptr_t)record->ExceptionAddress,
           info->dwFirstChance ? "first" : "second", thread->id);
    ui_color(UI_RESET);
    if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
        record->NumberParameters >= 2)
        printf("  Invalid %s at 0x%016" PRIX64 ".\n",
               access_name(record->ExceptionInformation[0]),
               (uint64_t)record->ExceptionInformation[1]);
    puts(info->dwFirstChance ? "  continue passes this exception to the program's handlers."
                            : "  No handler accepted this exception; continue allows termination.");
}

static bool run_stop_commands(Debugger *debugger, Thread *thread, bool can_step)
{
    ++debugger->stops;
    Action action = command_loop(debugger, thread, can_step);
    if (action == ACTION_QUIT) {
        debugger->quit_requested = true;
        return false;
    }
    if (action == ACTION_ERROR)
        return false;
    if (action == ACTION_STEP || debugger->pending_breakpoint >= 0)
        return begin_step(debugger, thread, action == ACTION_STEP);
    return true;
}

static bool handle_exception(Debugger *debugger, const DEBUG_EVENT *event,
                             bool *initial_breakpoint, DWORD *status)
{
    Thread *thread = find_thread(debugger, event->dwThreadId);
    if (!thread) {
        fputs("Unknown debug thread.\n", stderr);
        return false;
    }
    const EXCEPTION_DEBUG_INFO *info = &event->u.Exception;
    DWORD code = info->ExceptionRecord.ExceptionCode;
    DWORD64 address = (DWORD64)(uintptr_t)info->ExceptionRecord.ExceptionAddress;
    bool own_step = debugger->stepping_thread == event->dwThreadId;
    bool visible_step = debugger->visible_step;

    // retry queued INT3 after the pending step rearms the breakpoint
    if (debugger->stepping_thread && !own_step && code == EXCEPTION_BREAKPOINT &&
        find_breakpoint(debugger, address) >= 0)
        return rewind_thread(thread, address);
    if (is_debugger_noise(code)) {
        *status = DBG_EXCEPTION_NOT_HANDLED;
        return true;
    }
    if (debugger->stepping_thread && !finish_step(debugger, false))
        return false;
    // hide the internal breakpoint recovery step
    if (code == EXCEPTION_SINGLE_STEP && own_step && !visible_step)
        return true;

    ui_heading("PAUSED");
    bool can_step = true;
    if (code == EXCEPTION_SINGLE_STEP && own_step) {
        if (!report_step(debugger, thread))
            return false;
    } else if (code == EXCEPTION_BREAKPOINT) {
        if (!report_breakpoint(debugger, thread, address, initial_breakpoint))
            return false;
    } else {
        report_exception(info, thread);
        *status = DBG_EXCEPTION_NOT_HANDLED;
        can_step = false;
    }
    return run_stop_commands(debugger, thread, can_step);
}

static bool print_image(Debugger *debugger, const CREATE_PROCESS_DEBUG_INFO *info)
{
    DWORD64 base = (DWORD64)(uintptr_t)info->lpBaseOfImage;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    if (!read_memory(debugger->process, base, &dos, sizeof(dos)) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        !read_memory(debugger->process, base + (DWORD)dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        fputs("Only native Windows x64 executables are supported.\n", stderr);
        return false;
    }
    debugger->image_base = base;
    debugger->entry_point = base + nt.OptionalHeader.AddressOfEntryPoint;
    printf("Image base: 0x%016" PRIX64 "\nEntry point: 0x%016" PRIX64 "\n",
           (uint64_t)base, (uint64_t)(base + nt.OptionalHeader.AddressOfEntryPoint));
    return true;
}

static void close_file_handle(HANDLE file)
{
    if (file)
        CloseHandle(file);
}

static bool handle_thread_exit(Debugger *debugger, DWORD thread_id, bool terminating)
{
    bool success = true;
    Thread *thread = find_thread(debugger, thread_id);
    if (!terminating && debugger->stepping_thread == thread_id)
        success = finish_step(debugger, true);
    if (thread) {
        CloseHandle(thread->handle);
        memset(thread, 0, sizeof(*thread));
    }
    return success;
}

static void forget_module_breakpoints(Debugger *debugger, DWORD64 module)
{
    for (int i = 0; i < MAX_BREAKPOINTS; ++i) {
        Breakpoint *breakpoint = &debugger->breakpoints[i];
        if (breakpoint->used && breakpoint->module == module) {
            breakpoint->used = false;
            if (debugger->pending_breakpoint == i)
                debugger->pending_breakpoint = -1;
        }
    }
}

static void print_exit_code(DWORD exit_code)
{
    ui_color(exit_code ? UI_CHANGED : UI_SUCCESS);
    printf("Process exited with code %lu (0x%08lX).\n", exit_code, exit_code);
    ui_color(UI_RESET);
}

static int debug_loop(Debugger *debugger)
{
    bool initial_breakpoint = true;
    bool terminating = false;
    int result = 0;
    for (;;) {
        DEBUG_EVENT event;
        if (!WaitForDebugEvent(&event, INFINITE)) {
            print_error("WaitForDebugEvent");
            TerminateProcess(debugger->process, 1);
            return 1;
        }
        DWORD status = DBG_CONTINUE;
        bool success = true;
        bool exited = false;
        switch (event.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            close_file_handle(event.u.CreateProcessInfo.hFile);
            if (!terminating)
                success = print_image(debugger, &event.u.CreateProcessInfo) &&
                          add_thread(debugger, event.dwThreadId, event.u.CreateProcessInfo.hThread);
            break;
        case CREATE_THREAD_DEBUG_EVENT:
            if (!terminating)
                success = add_thread(debugger, event.dwThreadId, event.u.CreateThread.hThread);
            break;
        case EXIT_THREAD_DEBUG_EVENT:
            success = handle_thread_exit(debugger, event.dwThreadId, terminating);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            close_file_handle(event.u.LoadDll.hFile);
            break;
        case UNLOAD_DLL_DEBUG_EVENT:
            forget_module_breakpoints(debugger, (DWORD64)(uintptr_t)event.u.UnloadDll.lpBaseOfDll);
            break;
        case EXCEPTION_DEBUG_EVENT:
            if (!terminating)
                success = handle_exception(debugger, &event, &initial_breakpoint, &status);
            else
                status = DBG_EXCEPTION_NOT_HANDLED;
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            print_summary(debugger);
            print_exit_code(event.u.ExitProcess.dwExitCode);
            exited = true;
            break;
        default:
            break;
        }
        if (!success && !terminating) {
            result = debugger->quit_requested ? 0 : 1;
            puts("Ending debug session (terminating target with code 1).");
            if (!TerminateProcess(debugger->process, 1)) {
                print_error("TerminateProcess");
                result = 1;
            }
            terminating = true;
        }
        if (!ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status)) {
            print_error("ContinueDebugEvent");
            TerminateProcess(debugger->process, 1);
            return 1;
        }
        if (exited)
            return result;
    }
}

static int run_session(const wchar_t *target)
{
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process = {0};
    startup.cb = sizeof(startup);
    size_t length = wcslen(target);
    wchar_t *command_line = calloc(length + 3, sizeof(*command_line));
    if (!command_line) {
        fputs("Cannot allocate process command line.\n", stderr);
        return 1;
    }
    swprintf(command_line, length + 3, L"\"%ls\"", target);
    BOOL created = CreateProcessW(target, command_line, NULL, NULL, FALSE,
                                  DEBUG_ONLY_THIS_PROCESS, NULL, NULL, &startup, &process);
    DWORD create_error = GetLastError();
    free(command_line);
    if (!created) {
        SetLastError(create_error);
        print_error("CreateProcessW");
        return 1;
    }
    CloseHandle(process.hThread);
    Debugger debugger = {0};
    debugger.process = process.hProcess;
    debugger.process_id = process.dwProcessId;
    debugger.pending_breakpoint = -1;
    int result = debug_loop(&debugger);
    for (int i = 0; i < MAX_THREADS; ++i)
        if (debugger.threads[i].handle)
            CloseHandle(debugger.threads[i].handle);
    CloseHandle(debugger.process);
    return result;
}

static BOOL WINAPI on_console_signal(DWORD type)
{
    // atexit handlers do not run when the default handler ends the process
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT)
        console_restore();
    return FALSE;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc > 2) {
        fputs("Usage: minidbg.exe [program.exe]\nQuote paths containing spaces.\n", stderr);
        return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    console_init();
    SetConsoleCtrlHandler(on_console_signal, TRUE);
    if (atexit(console_restore) != 0) {
        console_restore();
        return 1;
    }
    print_banner();
    wchar_t *target = calloc(TARGET_PATH_CAPACITY, sizeof(*target));
    if (!target) {
        fputs("Cannot allocate program path.\n", stderr);
        return 1;
    }
    // return to program selection after an interactive session
    bool interactive = argc == 1;
    int result = 0;
    for (;;) {
        TargetResult selection = select_target(interactive ? NULL : argv[1], target);
        if (selection != TARGET_READY) {
            result = selection == TARGET_CANCELLED ? 0 : 1;
            break;
        }
        result = run_session(target);
        if (!interactive)
            break;
        ui_color(UI_MUTED);
        puts("\nSession finished. Choose another program, or type quit to exit.\n");
        ui_color(UI_RESET);
    }
    free(target);
    return result;
}