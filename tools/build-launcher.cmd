@echo off
rem Build the KookClean launcher (x64, no dependency, single exe).
rem Output: tools\launcher\StartKOOK.exe
setlocal

set "ROOT=%~dp0.."
set "VSDIR="

if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build"
if not defined VSDIR if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build"
if not defined VSDIR if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build"
if not defined VSDIR if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build"

if not defined VSDIR (
    echo [error] vcvars64.bat not found. Install Visual Studio 2022 with C++ desktop workload.
    exit /b 1
)

call "%VSDIR%\vcvars64.bat" >nul
if errorlevel 1 (
    echo [error] vcvars64.bat failed.
    exit /b 1
)

pushd "%ROOT%\tools\launcher" || exit /b 1
cl /nologo /utf-8 /O2 /EHsc /MT /DUNICODE /D_UNICODE launcher.cpp /Fe:StartKOOK.exe /link /SUBSYSTEM:WINDOWS user32.lib
set "RC=%errorlevel%"
popd

if not "%RC%"=="0" (
    echo [error] compile failed.
    exit /b %RC%
)
echo [ok] tools\launcher\StartKOOK.exe built.
exit /b 0
