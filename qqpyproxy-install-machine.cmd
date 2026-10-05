@echo off
setlocal
rem ============================================================
rem  Machine-wide redirect of the QQ Pinyin TIP InprocServer32 to the proxy.
rem  Undo: qqpyproxy-restore-machine.cmd
rem  Requires Administrator.
rem
rem  Notes:
rem   * The real DLL path is taken from the current registration when it is
rem     readable, otherwise derived from %SystemRoot% - a missing/blank
rem     InprocServer32 no longer aborts the install.
rem   * The pristine backup (backup-CLSID.reg / orig-value.first.txt) is never
rem     overwritten once it exists. If the CLSID key is missing entirely, the
rem     script tells you to import that backup first.
rem ============================================================
rem Directory comes from the environment, never hardcoded.
set "DIR=%ProgramData%\QQPYCandFix"
if not defined ProgramData set "DIR=%ALLUSERSPROFILE%\QQPYCandFix"
set "CLSID={AE51F1C0-807F-4A64-AC55-F2ADF92E2603}"
set "KEY=HKLM\SOFTWARE\Classes\CLSID\%CLSID%\InprocServer32"
set "DEFAULT_REAL=%SystemRoot%\system32\IME\QQPinyinTSF\QQPinyinTSF.dll"

net session >nul 2>&1 || ( echo [x] needs Administrator & exit /b 1 )
if not exist "%~dp0qqpyproxy.dll" ( echo [x] qqpyproxy.dll not found next to this script & exit /b 1 )
if not exist "%DIR%" mkdir "%DIR%"

rem ---- 1) determine the real DLL path -------------------------------------
set "REAL="
for /f "tokens=2,*" %%a in ('reg query "%KEY%" /ve /reg:64 2^>nul ^| findstr /i "REG_SZ"') do set "REAL=%%b"

reg query "HKLM\SOFTWARE\Classes\CLSID\%CLSID%" /reg:64 >nul 2>&1
if errorlevel 1 (
    echo [!] the 64-bit CLSID key is MISSING - new 64-bit apps cannot load the IME.
    if exist "%DIR%\backup-CLSID.reg" (
        echo     restore it first with:  reg import "%DIR%\backup-CLSID.reg"
    ) else (
        echo     no backup found; the key has to be re-registered by reinstalling QQ Pinyin.
    )
)

if "%REAL%"=="" (
    set "REAL=%DEFAULT_REAL%"
    echo [i] could not read the current InprocServer32 - assuming the default:
    echo     %DEFAULT_REAL%
)
if not exist "%REAL%" (
    echo [x] real DLL does not exist: %REAL%
    exit /b 1
)

rem ---- 2) backups (keep the first/pristine one forever) --------------------
if not exist "%DIR%\backup-CLSID.reg" (
    reg export "HKLM\SOFTWARE\Classes\CLSID\%CLSID%" "%DIR%\backup-CLSID.reg" /y >nul 2>&1
    echo [backup] created %DIR%\backup-CLSID.reg
) else (
    echo [backup] keeping existing %DIR%\backup-CLSID.reg ^(pristine^)
)
> "%DIR%\orig-value.txt" echo %REAL%
if not exist "%DIR%\orig-value.first.txt" copy /y "%DIR%\orig-value.txt" "%DIR%\orig-value.first.txt" >nul
echo [backup] original value = %REAL%

rem ---- 3) files ------------------------------------------------------------
copy /y "%~dp0qqpyproxy.dll" "%DIR%\qqpyproxy.dll" >nul
> "%DIR%\qqpyproxy.ini" echo real=%REAL%
>>"%DIR%\qqpyproxy.ini" echo hosts=WindowsTerminal.exe
>>"%DIR%\qqpyproxy.ini" echo mode=1
>>"%DIR%\qqpyproxy.ini" echo log=0

rem ---- 4) registry ---------------------------------------------------------
reg add "%KEY%" /ve /t REG_SZ /d "%DIR%\qqpyproxy.dll" /f >nul
reg add "%KEY%" /v ThreadingModel /t REG_SZ /d Apartment /f >nul

rem ---- 5) drop the per-user key from the earlier HKCU experiment ------------
rem   Guarded: never run a delete with an empty/mangled CLSID.
if defined CLSID if not "%CLSID%"=="" (
    reg delete "HKCU\Software\Classes\CLSID\%CLSID%" /f >nul 2>&1
)

echo.
echo [OK] machine-wide redirect -^> %DIR%\qqpyproxy.dll
reg query "%KEY%" /ve /reg:64
echo   real (for the proxy) = %REAL%
echo.
echo Verify: close ALL Windows Terminal windows, open a new one, type pinyin.
echo Undo:   qqpyproxy-restore-machine.cmd
