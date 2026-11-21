@echo off
setlocal
pushd "%~dp0" || exit /b 1
if not exist "bin\minidbg.exe" (
    echo bin\minidbg.exe not found. Build first: powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
    popd
    pause
    exit /b 1
)
"bin\minidbg.exe"
set "debugExitCode=%errorlevel%"
popd
pause
exit /b %debugExitCode%
