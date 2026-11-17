@echo off
rem One-step release:  release.bat 1.2.1 "What's new in this version"
rem   1. sets the version in src\core\version.h
rem   2. builds build\RemoteDesk.exe and build\setup.exe
rem   3. signs and publishes them to relay\updates\ (served by relay\server.js on port 21117)
rem   4. builds build\RemoteDesk.msix for the Microsoft Store (upload it in Partner Center)
rem Apps installed with setup.exe that connect to our server are offered the update the
rem next time they start (or as soon as they reconnect, or within 6 hours if they stay open).
rem Copy relay\updates\ to the live server when it isn't this machine (see relay\DEPLOY.md).
setlocal
cd /d "%~dp0"
if "%~1"=="" (echo usage: release.bat ^<version, e.g. 1.2.1^> ["release notes"]   ^(from Git Bash: ./release.bat ...^) & exit /b 1)
rem Stop before changing anything if RemoteDesk is running from build\ (the exe couldn't be replaced).
powershell -NoProfile -Command "if (Get-Process RemoteDesk -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq '%~dp0build\RemoteDesk.exe' }) { exit 1 }"
if errorlevel 1 (echo RemoteDesk is running from build\ - close it, then run release.bat again. Nothing was changed. & exit /b 1)
node tools\release.js bump %1 || exit /b 1
call "%~dp0build.bat" || exit /b 1
call "%~dp0build-installer.bat" || exit /b 1
node tools\release.js publish --notes "%~2" || exit /b 1
echo.
echo Released %1. Users are offered it the next time RemoteDesk starts.

rem The Store package comes last: the update above is already live even if this step fails
rem (e.g. on a PC without the Windows SDK).
echo.
echo Building the Microsoft Store package...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-msix.ps1"
if errorlevel 1 (
    echo WARNING: build\RemoteDesk.msix was not built ^(needs the Windows SDK:
    echo          winget install Microsoft.WindowsSDK.10.0.26100^). The setup.exe release is unaffected.
    exit /b 2
)
echo Store package: build\RemoteDesk.msix ^(version %1.0^) - upload it in Partner Center to update Store users.
