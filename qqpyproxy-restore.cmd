@echo off
setlocal
rem ============================================================
rem  Remove the HKCU redirect; the system HKLM registration takes over again.
rem ============================================================
set "CLSID={AE51F1C0-807F-4A64-AC55-F2ADF92E2603}"
set "HKCUKEY=HKCU\Software\Classes\CLSID\%CLSID%"
reg query "%HKCUKEY%" >nul 2>&1 && reg delete "%HKCUKEY%" /f >nul
echo [OK] HKCU redirect removed
echo Effective registration now:
reg query "HKLM\SOFTWARE\Classes\CLSID\%CLSID%\InprocServer32" /ve /reg:64
echo.
echo Note: processes that already loaded the proxy must be restarted.
