@echo off
rem Install Assets-pack.zip (game assets) into the repo root.
rem Usage: setup-assets.cmd [-Package <zip>]
rem See setup-assets.ps1 for details.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-assets.ps1" %*
set "SM_EXIT=%ERRORLEVEL%"
rem Keep the window open when double-clicked (no arguments).
if "%~1"=="" pause
exit /b %SM_EXIT%
