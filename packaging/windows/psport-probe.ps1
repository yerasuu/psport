# Runs psport-windows-probe.exe from this folder and opens the report it writes (README-WINDOWS.txt).
# Right-click > Run with PowerShell, or: powershell -ExecutionPolicy Bypass -File .\psport-probe.ps1
$ErrorActionPreference = 'Stop'
$folder = Split-Path -Parent $MyInvocation.MyCommand.Path
$probe = Join-Path $folder 'psport-windows-probe.exe'
if (-not (Test-Path -LiteralPath $probe)) {
    Write-Host 'psport-windows-probe.exe is not next to this script: extract the whole zip into a folder first.'
    Read-Host 'Press Enter to close'
    exit 1
}
Write-Host 'Running the psport Windows probe; it takes a few seconds and needs no game.'
Push-Location -LiteralPath $folder
try {
    & $probe
    $probeExit = $LASTEXITCODE
} finally {
    Pop-Location
}
$report = Join-Path $folder 'psport-windows-report.txt'
if (-not (Test-Path -LiteralPath $report)) { $report = Join-Path $env:TEMP 'psport-windows-report.txt' }
if (Test-Path -LiteralPath $report) {
    Write-Host "Report: $report"
    Write-Host 'Send that file back as it is.'
    Start-Process notepad.exe -ArgumentList "`"$report`""
} else {
    Write-Host "No report was written: probe exit code $probeExit. Send a screenshot of this window instead."
}
Read-Host 'Press Enter to close'
exit $probeExit
