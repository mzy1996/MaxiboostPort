@echo off
rem The launcher (the window with the game, the cheats and the settings).
rem
rem The window needs PySide6. If it is not there, the launcher says so and the command
rem below installs it:
rem     python -m pip install PySide6
rem
rem The game itself needs a dump of MAXIBOOST ON with update 1.15 (the launcher asks for
rem the folder on its first page).

setlocal
cd /d "%~dp0"

rem The Python interpreter: the one of the system, then the py launcher.
set "PYTHON="
where python >nul 2>nul && set "PYTHON=python"
if not defined PYTHON where py >nul 2>nul && set "PYTHON=py"

if not defined PYTHON (
    echo Python was not found. Install it from python.org (the version 3.9 or newer)
    echo and run this file again.
    pause
    exit /b 1
)

rem The launcher tells whether PySide6 is there; if not, it is installed here.
"%PYTHON%" -c "import PySide6" >nul 2>nul
if errorlevel 1 (
    echo The launcher needs PySide6. It is installed now; this takes a moment.
    "%PYTHON%" -m pip install PySide6
    if errorlevel 1 (
        echo.
        echo PySide6 could not be installed. Run this file again, or install it with
        echo     python -m pip install PySide6
        pause
        exit /b 1
    )
)

"%PYTHON%" launcher\win_launcher.py %*
if errorlevel 1 pause
endlocal
