@echo off
rem Build Assets-pack.zip from the game assets in the repo root (for maintainers).
rem Usage: pack-assets.cmd [-Source <dir>] [-Output <zip>]
rem See pack-assets.ps1 for details.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pack-assets.ps1" %*
set "SM_EXIT=%ERRORLEVEL%"
rem Keep the window open when double-clicked (no arguments).
if "%~1"=="" pause
exit /b %SM_EXIT%
