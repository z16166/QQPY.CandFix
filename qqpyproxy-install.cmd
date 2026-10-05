@echo off
setlocal
rem ============================================================
rem  Redirect QQ Pinyin TIP InprocServer32 to the proxy DLL.
rem  HKCU only (current user). No HKLM change, no file content change.
rem  Undo with: qqpyproxy-restore.cmd
rem ============================================================
set "DIR=C:\ProgramData\QQPYCandFix"
set "CLSID={AE51F1C0-807F-4A64-AC55-F2ADF92E2603}"
set "HKCUKEY=HKCU\Software\Classes\CLSID\%CLSID%\InprocServer32"
set "HKLMKEY=HKLM\SOFTWARE\Classes\CLSID\%CLSID%\InprocServer32"

if not exist "%~dp0qqpyproxy.dll" ( echo [x] qqpyproxy.dll not found next to this script & exit /b 1 )
if not exist "%DIR%" mkdir "%DIR%"

set "REAL="
for /f "tokens=2,*" %%a in ('reg query "%HKLMKEY%" /ve /reg:64 2^>nul ^| findstr /i "REG_SZ"') do set "REAL=%%b"
if "%REAL%"=="" ( echo [x] cannot read original InprocServer32 & exit /b 1 )
echo real dll = %REAL%

copy /y "%~dp0qqpyproxy.dll" "%DIR%\qqpyproxy.dll" >nul
> "%DIR%\qqpyproxy.ini" echo real=%REAL%
>>"%DIR%\qqpyproxy.ini" echo hosts=WindowsTerminal.exe
>>"%DIR%\qqpyproxy.ini" echo mode=1

reg add "%HKCUKEY%" /ve /t REG_SZ /d "%DIR%\qqpyproxy.dll" /f >nul
reg add "%HKCUKEY%" /v ThreadingModel /t REG_SZ /d Apartment /f >nul

echo.
echo [OK] redirected (current user only) to %DIR%\qqpyproxy.dll
reg query "%HKCUKEY%" /ve
echo.
echo Next: close ALL Windows Terminal windows, open a new one, type pinyin there.
echo Undo: qqpyproxy-restore.cmd
