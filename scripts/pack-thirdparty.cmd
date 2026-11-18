@echo off
rem Build ThirdParty-pack.zip from the current ThirdParty folder (for maintainers).
rem Usage: pack-thirdparty.cmd [-Source <dir>] [-Output <zip>]
rem See pack-thirdparty.ps1 for details.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0pack-thirdparty.ps1" %*
set "SM_EXIT=%ERRORLEVEL%"
rem Keep the window open when double-clicked (no arguments).
if "%~1"=="" pause
exit /b %SM_EXIT%
