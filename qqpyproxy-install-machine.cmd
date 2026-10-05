@echo off
setlocal
rem ============================================================
rem  Machine-wide redirect of the QQ Pinyin TIP InprocServer32 to the proxy.
rem  Backs up the original value first. Undo: qqpyproxy-restore-machine.cmd
rem  Requires Administrator.
rem ============================================================
set "DIR=C:\ProgramData\QQPYCandFix"
set "CLSID={AE51F1C0-807F-4A64-AC55-F2ADF92E2603}"
set "KEY=HKLM\SOFTWARE\Classes\CLSID\%CLSID%\InprocServer32"

net session >nul 2>&1 || ( echo [x] needs Administrator & exit /b 1 )
if not exist "%~dp0qqpyproxy.dll" ( echo [x] qqpyproxy.dll not found next to this script & exit /b 1 )
if not exist "%DIR%" mkdir "%DIR%"

rem ---- backup ----
set "REAL="
for /f "tokens=2,*" %%a in ('reg query "%KEY%" /ve /reg:64 2^>nul ^| findstr /i "REG_SZ"') do set "REAL=%%b"
if "%REAL%"=="" ( echo [x] cannot read current InprocServer32 & exit /b 1 )
reg export "HKLM\SOFTWARE\Classes\CLSID\%CLSID%" "%DIR%\backup-CLSID.reg" /y >nul
> "%DIR%\orig-value.txt" echo %REAL%
if not exist "%DIR%\orig-value.first.txt" copy /y "%DIR%\orig-value.txt" "%DIR%\orig-value.first.txt" >nul
echo [backup] original = %REAL%
echo [backup] reg export  = %DIR%\backup-CLSID.reg

rem ---- files ----
copy /y "%~dp0qqpyproxy.dll" "%DIR%\qqpyproxy.dll" >nul
> "%DIR%\qqpyproxy.ini" echo real=%REAL%
>>"%DIR%\qqpyproxy.ini" echo hosts=WindowsTerminal.exe
>>"%DIR%\qqpyproxy.ini" echo mode=1

rem ---- registry ----
reg add "%KEY%" /ve /t REG_SZ /d "%DIR%\qqpyproxy.dll" /f >nul
reg add "%KEY%" /v ThreadingModel /t REG_SZ /d Apartment /f >nul

rem ---- tidy: drop the per-user key we tried earlier ----
reg delete "HKCU\Software\Classes\CLSID\%CLSID%" /f >nul 2>&1

echo.
echo [OK] machine-wide redirect -^> %DIR%\qqpyproxy.dll
reg query "%KEY%" /ve /reg:64
echo.
echo Verify: close ALL Windows Terminal windows, open a new one, type pinyin.
echo Undo:   qqpyproxy-restore-machine.cmd
