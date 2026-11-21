param([switch]$Test, [string]$Python = 'python')

$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    New-Item -ItemType Directory -Force -Path bin | Out-Null
    $common = @('src/debugger.c', 'src/commands.c', 'src/console.c', 'src/inspect.c')
    $sources = @('src/main.c', 'src/target.c') + $common
    if (Get-Command gcc -ErrorAction SilentlyContinue) {
        $flags = @('-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-O0', '-g')
        & gcc @flags -municode @sources -o bin/minidbg.exe
        if ($LASTEXITCODE -ne 0) { throw 'Debugger build failed.' }
        & gcc @flags examples/demo.c -o bin/demo.exe
        if ($LASTEXITCODE -ne 0) { throw 'Demo build failed.' }
        & gcc @flags tests/test_commands.c @common -o bin/test_commands.exe
        if ($LASTEXITCODE -ne 0) { throw 'Unit test build failed.' }
        if ($Test) {
            & gcc @flags tests/test_memory.c @common -o bin/test_memory.exe
            if ($LASTEXITCODE -ne 0) { throw 'Memory unit test build failed.' }
            & gcc @flags tests/test_breakpoints.c @common -o bin/test_breakpoints.exe
            if ($LASTEXITCODE -ne 0) { throw 'Breakpoint unit test build failed.' }
            & gcc @flags tests/target.c -o bin/test_target.exe
            if ($LASTEXITCODE -ne 0) { throw 'Test target build failed.' }
        }
    } elseif (Get-Command cl -ErrorAction SilentlyContinue) {
        $flags = @('/nologo', '/std:c11', '/W4', '/WX', '/Od', '/D_CRT_SECURE_NO_WARNINGS', '/Fobin/')
        & cl @flags @sources /Febin/minidbg.exe
        if ($LASTEXITCODE -ne 0) { throw 'Debugger build failed.' }
        & cl @flags examples/demo.c /Febin/demo.exe
        if ($LASTEXITCODE -ne 0) { throw 'Demo build failed.' }
        & cl @flags tests/test_commands.c @common /Febin/test_commands.exe
        if ($LASTEXITCODE -ne 0) { throw 'Unit test build failed.' }
        if ($Test) {
            & cl @flags tests/test_memory.c @common /Febin/test_memory.exe
            if ($LASTEXITCODE -ne 0) { throw 'Memory unit test build failed.' }
            & cl @flags tests/test_breakpoints.c @common /Febin/test_breakpoints.exe
            if ($LASTEXITCODE -ne 0) { throw 'Breakpoint unit test build failed.' }
            & cl @flags tests/target.c /Febin/test_target.exe
            if ($LASTEXITCODE -ne 0) { throw 'Test target build failed.' }
        }
    } else {
        throw 'Use MSYS2 UCRT64 GCC in PATH, or an x64 Native Tools shell for Visual Studio.'
    }
    if ($Test) {
        & ./bin/test_commands.exe
        if ($LASTEXITCODE -ne 0) { throw 'Unit tests failed.' }
        & ./bin/test_memory.exe
        if ($LASTEXITCODE -ne 0) { throw 'Memory unit tests failed.' }
        & ./bin/test_breakpoints.exe
        if ($LASTEXITCODE -ne 0) { throw 'Breakpoint unit tests failed.' }
        & $Python tests/integration.py
        if ($LASTEXITCODE -ne 0) { throw 'Integration tests failed.' }
        & $Python tests/launcher.py
        if ($LASTEXITCODE -ne 0) { throw 'Launcher tests failed.' }
    }
    Write-Host 'Built: bin/minidbg.exe and bin/demo.exe'
} finally {
    Pop-Location
}
