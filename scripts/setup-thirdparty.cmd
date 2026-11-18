@echo off
rem Install ThirdParty-pack.zip and point the VS project at it.
rem Usage: setup-thirdparty.cmd [-Package <zip>] [-ThirdPartyDir <dir>]
rem See setup-thirdparty.ps1 for details.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-thirdparty.ps1" %*
set "SM_EXIT=%ERRORLEVEL%"
rem Keep the window open when double-clicked (no arguments).
if "%~1"=="" pause
exit /b %SM_EXIT%
