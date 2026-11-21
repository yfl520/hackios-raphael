@echo off
title HackiOS - Redmi K20 Pro
chcp 65001 >nul
cd /d "%~dp0"
"%~dp0tools\python\python.exe" -B -X utf8 "%~dp0installer.py" --verify
set "result=%errorlevel%"
echo.
pause
exit /b %result%
