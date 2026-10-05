@echo off
setlocal
rem ============================================================
rem  Undo the machine-wide redirect: restore the original InprocServer32.
rem  Requires Administrator.
rem ============================================================
set "DIR=C:\ProgramData\QQPYCandFix"
set "CLSID={AE51F1C0-807F-4A64-AC55-F2ADF92E2603}"
set "KEY=HKLM\SOFTWARE\Classes\CLSID\%CLSID%\InprocServer32"

net session >nul 2>&1 || ( echo [x] needs Administrator & exit /b 1 )

set "REAL="
if exist "%DIR%\orig-value.first.txt" for /f "usebackq delims=" %%a in ("%DIR%\orig-value.first.txt") do set "REAL=%%a"
if "%REAL%"=="" set "REAL=C:\WINDOWS\system32\IME\QQPinyinTSF\QQPinyinTSF.dll"

reg add "%KEY%" /ve /t REG_SZ /d "%REAL%" /f >nul
reg add "%KEY%" /v ThreadingModel /t REG_SZ /d Apartment /f >nul
echo [OK] restored to: %REAL%
reg query "%KEY%" /ve /reg:64
echo.
echo Note: processes that already loaded the proxy must be restarted.
