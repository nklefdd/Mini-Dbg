#include "debugger.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE output_handle;
static DWORD original_mode;
static bool mode_changed;
static bool colors_enabled;

void console_init(void)
{
    output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    const char *term = getenv("TERM");
    if (getenv("NO_COLOR") || (term && !strcmp(term, "dumb")))
        return;
    if (GetConsoleMode(output_handle, &original_mode) &&
        SetConsoleMode(output_handle, original_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        mode_changed = true;
        colors_enabled = true;
    }
}

void console_restore(void)
{
    ui_color(UI_RESET);
    fflush(stdout);
    if (mode_changed)
        SetConsoleMode(output_handle, original_mode);
    mode_changed = false;
    colors_enabled = false;
}

void ui_color(UiColor color)
{
    static const char *sequences[] = {
        "\x1b[0m", "\x1b[1;36m", "\x1b[1;33m", "\x1b[90m", "\x1b[1;31m", "\x1b[1;32m"
    };
    if (colors_enabled && color >= UI_RESET && color <= UI_SUCCESS)
        fputs(sequences[color], stdout);
}

void ui_heading(const char *title)
{
    ui_color(UI_MUTED);
    fputs("\n  -- ", stdout);
    ui_color(UI_ACCENT);
    fputs(title, stdout);
    ui_color(UI_MUTED);
    fputs(" ------------------------------------------------\n", stdout);
    ui_color(UI_RESET);
}

static void print_rule(void)
{
    fputs("  ", stdout);
    for (int i = 0; i < 103; ++i)
        putchar('-');
    putchar('\n');
}

void print_banner(void)
{
    static const char *letter_colors[] = {
        "\x1b[1;31m", "\x1b[1;33m", "\x1b[1;32m", "\x1b[1;36m",
        "\x1b[1;34m", "\x1b[1;35m", "\x1b[1;91m"
    };
    putchar('\n');
    fputs("  ", stdout);
    for (int i = 0; "MiniDbg"[i]; ++i) {
        if (colors_enabled)
            fputs(letter_colors[i], stdout);
        putchar("MiniDbg"[i]);
    }
    ui_color(UI_MUTED);
    puts("  for Windows x64");
    print_rule();
    ui_color(UI_RESET);
    print_commands();
    ui_color(UI_MUTED);
    print_rule();
    ui_color(UI_RESET);
    putchar('\n');
}

void print_summary(const Debugger *debugger)
{
    ui_heading("SESSION");
    printf("  Stops: %" PRIu64 "    Breakpoint hits: %" PRIu64 "    Steps: %" PRIu64 "\n",
           debugger->stops, debugger->breakpoint_hits, debugger->user_steps);
}
