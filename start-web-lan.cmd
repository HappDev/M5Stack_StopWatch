@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\start-web.ps1" -Lan
if errorlevel 1 pause
