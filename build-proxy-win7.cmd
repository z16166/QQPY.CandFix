@echo off
setlocal
rem ============================================================
rem  qqpyproxy.dll - built for Windows 7 x64
rem     toolset : v142 (VS2019 / MSVC 14.29.30133)
rem     SDK     : 10.0.17763.0
rem     CRT     : static (/MT) - no VCRUNTIME140 / UCRT dependency
rem     subsystem: 6.01 (Windows 7)
rem ============================================================
cd /d "%~dp0"

set "VS=C:\Program Files\Microsoft Visual Studio\18\Enterprise"
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x64 10.0.17763.0 -vcvars_ver=14.29
if errorlevel 1 (
    echo [x] vcvarsall failed - check that v142 toolset and SDK 10.0.17763.0 are installed
    exit /b 1
)

echo toolset = %VCToolsVersion%
echo sdk     = %WindowsSDKVersion%

cl /nologo /utf-8 /LD /MT /O2 /EHsc /W3 /D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /DNTDDI_VERSION=0x06010000 qqpyproxy.cpp qqpytsf.cpp /Fe:qqpyproxy.dll /link /OUT:qqpyproxy.dll /SUBSYSTEM:WINDOWS,6.01 /DYNAMICBASE /NXCOMPAT user32.lib

if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
echo --- built ---
dir /b qqpyproxy.dll
