$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'bin/minidbg.exe'
if (-not (Test-Path $exe)) {
    Write-Host 'bin/minidbg.exe not found. Build first: .\build.ps1'
    exit 1
}
& $exe
exit $LASTEXITCODE
