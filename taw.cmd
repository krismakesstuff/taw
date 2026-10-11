@echo off
rem Dev launcher: run the newest local build of taw.
rem
rem   taw               open or start a session in .\sessions
rem   taw my-song       open or create sessions\my-song
rem   taw build         build Release, then exit
rem
rem Sessions made from the repo root live in .\sessions (git-ignored) so they stay out of the
rem source tree. Put this folder on PATH and `taw` anywhere else works in that folder.

setlocal
set "ROOT=%~dp0"
set "REL=%ROOT%build\taw_artefacts\Release\taw.exe"
set "DBG=%ROOT%build\taw_artefacts\Debug\taw.exe"

if /i "%~1"=="build" (
    if not exist "%ROOT%build" cmake -S "%ROOT%." -B "%ROOT%build" -G "Visual Studio 17 2022" -A x64 || exit /b 1
    cmake --build "%ROOT%build" --config Release
    exit /b
)

set "EXE="
if exist "%REL%" set "EXE=%REL%"
if not defined EXE if exist "%DBG%" set "EXE=%DBG%"

rem Both exist: take Debug only if it's newer (xcopy /D lists the source when it's newer).
if exist "%REL%" if exist "%DBG%" (
    xcopy /L /D /Y "%DBG%" "%REL%" 2>nul | findstr /b /c:"1 " >nul && set "EXE=%DBG%"
)

if not defined EXE (
    echo No build yet. Run: taw build
    exit /b 1
)

rem Started from the repo root: sessions go in .\sessions. Anywhere else (on PATH): taw runs
rem right there, so `taw` in a session's folder opens it.
if /i not "%CD%\"=="%ROOT%" (
    "%EXE%" %*
    exit /b
)

if not exist "%ROOT%sessions" mkdir "%ROOT%sessions"
pushd "%ROOT%sessions"
"%EXE%" %*
set "CODE=%ERRORLEVEL%"
popd
exit /b %CODE%
