@echo off
rem Double-clickable entry point for simcore-view.
rem
rem Builds the viewer if it has not been built yet, then opens a window on the
rem ring demo.  Pass a board path (or any other argument) to override:
rem
rem   open.cmd --board data\latch.txt
rem   open.cmd --board data\and_gate.txt --bmp out.bmp
rem
rem Nothing here is required - the exe can be run directly, and started with no
rem arguments it asks for a board with a file picker.
setlocal
set "EXE=%~dp0..\..\build\simcore-view.exe"

if not exist "%EXE%" (
    echo Building simcore-view ...
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1"
    if errorlevel 1 (
        echo.
        echo Build failed - see the message above.
        pause
        exit /b 1
    )
)

if "%~1"=="" (
    "%EXE%" --board "%~dp0data\ring3.txt"
) else (
    "%EXE%" %*
)

set "CODE=%ERRORLEVEL%"
if not "%CODE%"=="0" (
    echo.
    pause
)
endlocal & exit /b %CODE%
