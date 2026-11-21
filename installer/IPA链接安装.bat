@echo off
chcp 65001 >nul
cd /d "%~dp0"
"%~dp0tools\python\python.exe" -B -X utf8 "%~dp0ipa_url.py" %*
set "result=%errorlevel%"
echo.
pause
exit /b %result%
