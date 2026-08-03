@echo off
REM Offline regression tests for .gn reading/writing.
REM
REM   run_tests.bat <folder containing .gn files>
REM
REM Run this from a Visual Studio developer command prompt (cl.exe must be on PATH).
REM
REM   test_gn      decrypt -> parse -> serialize -> re-encrypt, compared byte for byte
REM   test_writer  chart -> NoteData -> NotesWriterGN::BuildFrames, compares notes/structure
REM
REM GNFile.cpp and NotesWriterGN.cpp are copied straight from src, so this exercises the
REM real code.  stub_*.h stand in for the engine headers NotesWriterGN.cpp includes.
REM See docs/GN_FORMAT.md.

setlocal
if "%~1"=="" (
	echo Usage: run_tests.bat ^<folder containing .gn files^>
	exit /b 1
)

set HERE=%~dp0
set SRC=%HERE%..\..
set BUILD=%HERE%build

if not exist "%BUILD%" mkdir "%BUILD%"
copy /y "%SRC%\GNFile.cpp" "%BUILD%\" >nul
copy /y "%SRC%\GNFile.h" "%BUILD%\" >nul
copy /y "%SRC%\NotesWriterGN.cpp" "%BUILD%\" >nul
copy /y "%SRC%\NotesWriterGN.h" "%BUILD%\" >nul
copy /y "%HERE%test_gn.cpp" "%BUILD%\" >nul
copy /y "%HERE%test_writer.cpp" "%BUILD%\" >nul
copy /y "%HERE%stub_global.h" "%BUILD%\global.h" >nul
copy /y "%HERE%stub_NotesLoaderGN.h" "%BUILD%\NotesLoaderGN.h" >nul

REM Other headers NotesWriterGN.cpp includes; the contents all live in stub_global.h.
for %%F in (song.h Steps.h NoteData.h NoteTypes.h TimingData.h RageFile.h RageLog.h RageUtil.h) do (
	echo /* shim: see stub_global.h */> "%BUILD%\%%F"
)

pushd "%BUILD%"
cl /nologo /EHsc /O2 /wd4819 /Fe:test_gn.exe test_gn.cpp GNFile.cpp >nul || goto :fail
cl /nologo /EHsc /O2 /wd4819 /Fe:test_writer.exe test_writer.cpp GNFile.cpp NotesWriterGN.cpp >nul || goto :fail

echo === test_gn ===
for /r "%~1" %%G in (*.gn) do @"%BUILD%\test_gn.exe" "%%G" | findstr /c:"FAIL" /c:"failed"
echo === test_writer ===
for /r "%~1" %%G in (*.gn) do @"%BUILD%\test_writer.exe" "%%G" | findstr /c:"FAIL"
echo === done (no output above means everything passed) ===
popd
exit /b 0

:fail
echo Build failed - run this from a Visual Studio developer command prompt.
popd
exit /b 1
