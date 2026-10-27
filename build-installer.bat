@echo off
rem Builds build\setup.exe from build\RemoteDesk.exe with Inno Setup 6
rem (winget install JRSoftware.InnoSetup). Run build.bat first.
setlocal
cd /d "%~dp0"
if not exist build\RemoteDesk.exe (echo build\RemoteDesk.exe not found. Run build.bat first. & exit /b 1)

set ISCC=
for %%P in ("%LOCALAPPDATA%\Programs\Inno Setup 6" "%ProgramFiles(x86)%\Inno Setup 6" "%ProgramFiles%\Inno Setup 6") do (
    if exist "%%~P\ISCC.exe" set ISCC="%%~P\ISCC.exe"
)
if not defined ISCC (
    where iscc >nul 2>nul && set ISCC=iscc
)
if not defined ISCC (echo Inno Setup 6 not found. Install it: winget install JRSoftware.InnoSetup & exit /b 1)

%ISCC% /Q installer\RemoteDesk.iss || exit /b 1
echo Built build\setup.exe
