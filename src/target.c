#include "debugger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

static int read_target_line(wchar_t *line)
{
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    size_t length = 0;
    bool too_long = false, invalid = false;
    if (GetConsoleMode(input, &mode)) {
        for (;;) {
            wchar_t character;
            DWORD count = 0;
            if (!ReadConsoleW(input, &character, 1, &count, NULL)) {
                print_error("ReadConsoleW");
                return -2;
            }
            if (!count || character == 26)
                return 0;
            if (character == L'\n')
                break;
            if (character == L'\r')
                continue;
            if (!character)
                invalid = true;
            if (length + 1 < TARGET_PATH_CAPACITY)
                line[length++] = character;
            else
                too_long = true;
        }
        line[length] = L'\0';
    } else {
        size_t capacity = 4u * TARGET_PATH_CAPACITY; // keep redirected input and debugger commands in the same stream
        char *utf8 = malloc(capacity);
        if (!utf8) {
            fputs("Cannot allocate program input.\n", stderr);
            return -2;
        }
        int character;
        while ((character = fgetc(stdin)) != EOF && character != '\n') {
            if (character == '\r')
                continue;
            if (!character)
                invalid = true;
            if (length + 1 < capacity)
                utf8[length++] = (char)character;
            else
                too_long = true;
        }
        utf8[length] = '\0';
        if (ferror(stdin)) {
            free(utf8);
            fputs("Cannot read program input.\n", stderr);
            return -2;
        }
        if (character == EOF && !length && !too_long) {
            free(utf8);
            return 0;
        }
        if (!too_long && !invalid &&
            !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1,
                                 line, TARGET_PATH_CAPACITY)) {
            too_long = GetLastError() == ERROR_INSUFFICIENT_BUFFER;
            invalid = !too_long;
        }
        free(utf8);
    }
    if (too_long || invalid) {
        puts(too_long ? "Program name is too long." : "Invalid program name. Use UTF-8 for redirected input.");
        return -1;
    }
    return 1;
}

static bool normalize_input(wchar_t *name)
{
    wchar_t *start = name;
    while (iswspace(*start))
        ++start;
    size_t length = wcslen(start);
    while (length && iswspace(start[length - 1]))
        --length;
    if (length >= 2 && start[0] == L'"' && start[length - 1] == L'"') {
        ++start;
        length -= 2;
    }
    memmove(name, start, length * sizeof(*name));
    name[length] = L'\0';
    bool extended = wcsncmp(name, L"\\\\?\\", 4) == 0;
    for (size_t i = 0; i < length; ++i)
        if (name[i] < 32 || name[i] == L'"' || name[i] == L'*' ||
            (name[i] == L'?' && !(extended && i == 2)))
            return false;
    return true;
}

static bool existing_path(const wchar_t *name, wchar_t *path)
{
    DWORD count = GetFullPathNameW(name, TARGET_PATH_CAPACITY, path, NULL);
    if (!count || count >= TARGET_PATH_CAPACITY)
        return false;
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static bool search_directory(const wchar_t *directory, const wchar_t *name, wchar_t *path)
{
    DWORD count = SearchPathW(directory, name, L".exe", TARGET_PATH_CAPACITY, path, NULL);
    if (!count || count >= TARGET_PATH_CAPACITY)
        return false;
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static bool resolve_target(const wchar_t *name, wchar_t *path)
{
    if (existing_path(name, path))
        return true;
    bool explicit_path = wcspbrk(name, L"\\/:") != NULL;
    if (search_directory(L".", name, path))
        return true;
    if (explicit_path) // do not substitute another file for an explicit path
        return false;
    wchar_t directory[TARGET_PATH_CAPACITY];
    DWORD count = GetModuleFileNameW(NULL, directory, TARGET_PATH_CAPACITY);
    if (count && count < TARGET_PATH_CAPACITY) {
        wchar_t *separator = wcsrchr(directory, L'\\');
        if (separator) {
            separator[1] = L'\0';
            if (search_directory(directory, name, path))
                return true;
        }
    }
    count = GetEnvironmentVariableW(L"PATH", directory, TARGET_PATH_CAPACITY); // search PATH after the local directories
    return count && count < TARGET_PATH_CAPACITY && search_directory(directory, name, path);
}

static void print_target(const wchar_t *path)
{
    fputs("Target: ", stdout);
    fflush(stdout);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode, written;
    if (GetConsoleMode(output, &mode)) {
        WriteConsoleW(output, path, (DWORD)wcslen(path), &written, NULL);
    } else {
        int size = WideCharToMultiByte(CP_UTF8, 0, path, -1, NULL, 0, NULL, NULL);
        char *utf8 = size > 0 ? malloc((size_t)size) : NULL;
        if (utf8) {
            WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, size, NULL, NULL);
            fputs(utf8, stdout);
            free(utf8);
        }
    }
    putchar('\n');
}

TargetResult select_target(const wchar_t *argument, wchar_t *path)
{
    wchar_t name[TARGET_PATH_CAPACITY];
    bool interactive = argument == NULL;
    if (interactive) {
        puts("Enter a Windows x64 program name or full path. Type quit to cancel.");
        puts("Names are searched in the current folder, beside MiniDbg, then PATH.");
        puts("Example: demo.exe   or   C:\\Projects\\my_program.exe\n");
    }
    for (;;) {
        if (interactive) {
            ui_color(UI_ACCENT);
            fputs("Program> ", stdout);
            ui_color(UI_RESET);
            fflush(stdout);
            int result = read_target_line(name);
            if (result == 0)
                return TARGET_CANCELLED;
            if (result == -2)
                return TARGET_ERROR;
            if (result == -1)
                continue;
        } else {
            if (wcslen(argument) >= TARGET_PATH_CAPACITY) {
                puts("Program name is too long.");
                return TARGET_ERROR;
            }
            wcscpy(name, argument);
        }
        if (!normalize_input(name)) {
            puts("Invalid program name. Enter a file path, without arguments or wildcards.");
        } else if (interactive && !wcscmp(name, L"quit")) {
            return TARGET_CANCELLED;
        } else if (!name[0]) {
            if (!interactive)
                puts("Program name cannot be empty.");
        } else if (!resolve_target(name, path)) {
            puts("Program not found. Enter its full path, including the folder.");
        } else {
            DWORD binary_type;
            if (!GetBinaryTypeW(path, &binary_type)) {
                print_error("GetBinaryTypeW");
                puts("Select a compiled Windows x64 executable, not a document or source file.");
            } else if (binary_type != SCS_64BIT_BINARY) {
                puts("Only native Windows x64 executables are supported.");
            } else if (wcslen(path) > MAX_TARGET_PATH) {
                puts("Program name is too long.");
            } else {
                print_target(path);
                return TARGET_READY;
            }
        }
        if (!interactive)
            return TARGET_ERROR;
    }
}