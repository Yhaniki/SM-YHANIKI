@echo off
REM Build StandAloneServer (Release|Win32) without opening Visual Studio.
REM Output: ..\Program\StandAloneServer.exe
setlocal

REM Find MSBuild via vswhere
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
    set "MSBUILD=%%i"
)

if not defined MSBUILD (
    echo [ERROR] MSBuild not found.  Install Visual Studio 2019 or newer with C++ workload.
    exit /b 1
)

echo Using MSBuild: %MSBUILD%
"%MSBUILD%" "%~dp0StandAloneServer.vcxproj" /p:Configuration=Release /p:Platform=Win32 /m /nologo /v:minimal
exit /b %ERRORLEVEL%
