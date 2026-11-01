@echo off
setlocal EnableExtensions

rem Build the project-local EFZ DirectDraw wrapper as a 32-bit DLL.
rem Usage: build.bat [Debug^|Release] [Log]

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
if /I not "%CONFIG%"=="Debug" if /I not "%CONFIG%"=="Release" (
    echo [ERROR] Unknown configuration "%CONFIG%". Use Debug or Release.
    exit /b 1
)

set "LOGGING=OFF"
if /I "%~2"=="Log" set "LOGGING=ON"
if not "%~2"=="" if /I not "%~2"=="Log" (
    echo [ERROR] Unknown diagnostics option "%~2". Use Log or omit it.
    exit /b 1
)

set "SOURCE_DIR=%~dp0."
set "BUILD_DIR=%~dp0build"
cmake -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -A Win32 -DDDRAW_WRAPPER_ENABLE_LOGGING=%LOGGING%
if errorlevel 1 exit /b 1

cmake --build "%BUILD_DIR%" --config "%CONFIG%" --parallel
if errorlevel 1 exit /b 1

echo [OK] Built: %BUILD_DIR%\%CONFIG%\ddraw.dll
if /I "%LOGGING%"=="ON" echo [INFO] Set EFZ_DDRAW_LOG=1 when launching EFZ to create ddraw_wrapper.log.
exit /b 0
