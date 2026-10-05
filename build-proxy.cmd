@echo off
rem ============================================================
rem  Canonical build: calls the Windows 7 x64 target build.
rem  (v142 toolset + SDK 10.0.17763.0 + static CRT)
rem  Do not build qqpyproxy.dll any other way if you want it to
rem  load on Windows 7.
rem ============================================================
call "%~dp0build-proxy-win7.cmd"
exit /b %errorlevel%
