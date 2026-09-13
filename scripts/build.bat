@echo off
REM ============================================================
REM MyProt V2 - Build Script
REM Usage: scripts\build.bat [Debug|Release] [Win32|x64]
REM Default: Debug Win32
REM ============================================================

setlocal enabledelayedexpansion

set CONFIG=%1
set PLATFORM=%2

if "%CONFIG%"=="" set CONFIG=Debug
if "%PLATFORM%"=="" set PLATFORM=Win32

set ROOT=%~dp0..
set SLN=%ROOT%\MyProt.sln

echo ============================================================
echo  MyProt V2 - Build
echo  Configuration: %CONFIG%
echo  Platform:      %PLATFORM%
echo  Solution:      %SLN%
echo ============================================================
echo.

REM Find MSBuild (VS2015 = v14.0)
set "MSBUILD="
if exist "C:\Program Files (x86)\MSBuild\14.0\Bin\MSBuild.exe" (
    set "MSBUILD=C:\Program Files (x86)\MSBuild\14.0\Bin\MSBuild.exe"
) else if exist "C:\Program Files\MSBuild\14.0\Bin\MSBuild.exe" (
    set "MSBUILD=C:\Program Files\MSBuild\14.0\Bin\MSBuild.exe"
) else (
    echo ERROR: VS2015 MSBuild not found.
    echo Please install Visual Studio 2015 or VS2015 Build Tools.
    exit /b 1
)

echo Using MSBuild: %MSBUILD%
echo.

"%MSBUILD%" "%SLN%" -p:Configuration=%CONFIG% -p:Platform=%PLATFORM% -m -verbosity:minimal

if %ERRORLEVEL% neq 0 (
    echo.
    echo BUILD FAILED (exit code: %ERRORLEVEL%)
    exit /b %ERRORLEVEL%
)

echo.
echo ============================================================
echo  BUILD SUCCEEDED
echo  Output: %ROOT%\build\%CONFIG%\%PLATFORM%\
echo ============================================================

endlocal
