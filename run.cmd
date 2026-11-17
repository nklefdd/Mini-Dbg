@echo off
setlocal
pushd "%~dp0" || exit /b 1
"bin\minidbg.exe"
set "debugExitCode=%errorlevel%"
popd
pause
exit /b %debugExitCode%
