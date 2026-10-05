@echo off
setlocal enabledelayedexpansion
rem ============================================================
rem  qqpyproxy.dll - built for Windows 7 x64
rem     toolset : v142 (VS2019 / MSVC 14.29)
rem     SDK     : 10.0.17763.0
rem     CRT     : static (/MT) - no VCRUNTIME140 / UCRT dependency
rem     subsystem: 6.01 (Windows 7)
rem
rem  All paths (Visual Studio, ATL, SDK) are discovered, never hardcoded.
rem
rem  ATL note: the code uses CComPtr / CComQIPtr / CComObject for COM
rem  resource management. If the installed v142 toolset does not carry the
rem  "C++ ATL" component, this script falls back to the oldest toolset under
rem  the same Visual Studio that does have ATL, and uses that toolset's
rem  headers AND its atls.lib together (never mixing versions - ATL class
rem  layouts must match the library). MSVC v14x is binary compatible, so an
rem  older ATL links fine with the v142 compiler.
rem  Installing "C++ ATL for v142" makes the fallback unnecessary.
rem ============================================================
cd /d "%~dp0"

rem ---------------- locate Visual Studio (prefer an install with v142) --------
set "VS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.v142.x86.x64 -property installationPath`) do set "VS=%%i"
)
if not defined VS if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -products * -property installationPath`) do set "VS=%%i"
)
if not defined VS if exist "%ProgramFiles%\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" set "VS=%ProgramFiles%\Microsoft Visual Studio\18\Enterprise"
if not defined VS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" set "VS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
if not defined VS (
    echo [x] Visual Studio not found - set VS=... manually
    exit /b 1
)
if not exist "%VS%\VC\Auxiliary\Build\vcvarsall.bat" (
    echo [x] vcvarsall.bat not found under "%VS%"
    exit /b 1
)
echo VS     = %VS%

rem ---------------- locate a complete ATL (headers + atls.lib, same version) -
rem   Ascending order => oldest first => closest to v142, fewest new OS deps.
set "ATLROOT="
for /f "delims=" %%d in ('dir /b /ad /on "%VS%\VC\Tools\MSVC" 2^>nul') do (
    if not defined ATLROOT (
        if exist "%VS%\VC\Tools\MSVC\%%d\atlmfc\include\atlbase.h" (
            if exist "%VS%\VC\Tools\MSVC\%%d\atlmfc\lib\x64\atls.lib" set "ATLROOT=%VS%\VC\Tools\MSVC\%%d\atlmfc"
        )
    )
)
if defined ATLROOT (
    echo ATL    = !ATLROOT!
) else (
    echo [x] no complete ATL found - install the "C++ ATL" component for any toolset
    exit /b 1
)

rem ---------------- toolset + SDK ------------------------------------------
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x64 10.0.17763.0 -vcvars_ver=14.29
if errorlevel 1 (
    echo [x] vcvarsall failed - check that the v142 toolset and SDK 10.0.17763.0 are installed
    exit /b 1
)
echo toolset = %VCToolsVersion%
echo sdk     = %WindowsSDKVersion%

cl /nologo /utf-8 /LD /MT /std:c++17 /O2 /EHsc /W3 ^
   /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /DNTDDI_VERSION=0x06010000 ^
   /I"!ATLROOT!\include" ^
   qqpyproxy.cpp qqpytsf.cpp qqpyutil.cpp ^
   /Fe:qqpyproxy.dll /link /OUT:qqpyproxy.dll /MAP /SUBSYSTEM:WINDOWS,6.01 /DYNAMICBASE /NXCOMPAT ^
   /LIBPATH:"!ATLROOT!\lib\x64" ^
   user32.lib shell32.lib ole32.lib uuid.lib

if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
echo --- built ---
dir /b qqpyproxy.dll
