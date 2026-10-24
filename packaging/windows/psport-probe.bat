@echo off
rem Runs psport-windows-probe.exe from this folder and opens the report it writes (README-WINDOWS.txt).
setlocal
cd /d "%~dp0"
if not exist "psport-windows-probe.exe" (
    echo psport-windows-probe.exe is not next to this script: extract the whole zip into a folder first,
    echo then run psport-probe.bat from that folder.
    pause
    exit /b 1
)
echo Running the psport Windows probe; it takes a few seconds and needs no game.
echo.
psport-windows-probe.exe
set "probe_exit=%errorlevel%"
echo.
set "report=%~dp0psport-windows-report.txt"
if not exist "%report%" set "report=%TEMP%\psport-windows-report.txt"
if exist "%report%" (
    echo Report: %report%
    echo Send that file back as it is.
    start "" notepad "%report%"
) else (
    echo No report was written: probe exit code %probe_exit%. Send a screenshot of this window instead.
)
pause
exit /b %probe_exit%
