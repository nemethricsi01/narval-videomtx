@echo off
setlocal EnableDelayedExpansion
title Narval VideoMTX - Firmware Flasher

cd /d "%~dp0"

echo ============================================
echo   Narval VideoMTX Firmware Flasher
echo ============================================
echo.

if not exist "build\flash_args" (
    echo ERROR: build\flash_args not found.
    echo This script must sit next to a "build" folder produced by "idf.py build".
    echo Ask whoever built the firmware to copy the whole "build" folder here.
    echo.
    pause
    exit /b 1
)

rem Find esptool: prefer the modern "esptool" command, fall back to legacy "esptool.py"
set "ESPTOOL="
set "WRITE_CMD=write-flash"

where esptool >nul 2>nul
if not errorlevel 1 (
    set "ESPTOOL=esptool"
) else (
    where esptool.py >nul 2>nul
    if not errorlevel 1 (
        set "ESPTOOL=esptool.py"
        set "WRITE_CMD=write_flash"
    )
)

if "%ESPTOOL%"=="" (
    echo ERROR: esptool was not found on this computer.
    echo Install it first with:   pip install esptool
    echo or download esptool.exe from:
    echo   https://github.com/espressif/esptool/releases
    echo.
    pause
    exit /b 1
)

echo Available COM ports on this computer:
echo.
powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object"
echo.
echo (If the board isn't listed, unplug/replug it and re-run this script.
echo  You can also check Device Manager - Ports (COM ^& LPT).)
echo.

set /p PORT="Type the COM port to use (example: COM5) and press Enter: "
if "%PORT%"=="" (
    echo No COM port entered, aborting.
    pause
    exit /b 1
)

echo.
echo Flashing narval_videomtx to %PORT% using %ESPTOOL% ...
echo.

pushd build
"%ESPTOOL%" --chip esp32s3 -p %PORT% -b 460800 %WRITE_CMD% @flash_args
set "FLASH_RESULT=%ERRORLEVEL%"
popd

echo.
if not "%FLASH_RESULT%"=="0" (
    echo Something went wrong - scroll up to see the error from esptool.
    echo Common fixes: try a lower speed, hold the board's BOOT button while
    echo it connects, or double check the COM port number.
) else (
    echo Done! The board has been flashed successfully.
)

echo.
pause
