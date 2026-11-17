$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'bin/minidbg.exe')
exit $LASTEXITCODE
