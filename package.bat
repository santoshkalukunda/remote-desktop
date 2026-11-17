@echo off
rem Builds everything you hand out, without publishing anything:
rem     build\RemoteDesk.exe   the app
rem     build\setup.exe        the installer (send this to people, or run it yourself)
rem     build\RemoteDesk.msix  the Microsoft Store package (upload in Partner Center)
rem
rem   package.bat            build the current version again (src\core\version.h)
rem   package.bat 1.4.2      set the version first, then build
rem
rem Unlike release.bat this does NOT publish to relay\updates\, so installed apps aren't offered it.
rem When it's ready for everyone: release.bat <version> "notes"  (from Git Bash: ./package.bat, ./release.bat)
setlocal
cd /d "%~dp0"
powershell -NoProfile -Command "if (Get-Process RemoteDesk -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq '%~dp0build\RemoteDesk.exe' }) { exit 1 }"
if errorlevel 1 (echo RemoteDesk is running from build\ - close it, then run package.bat again. & exit /b 1)
if not "%~1"=="" node tools\release.js bump %1 || exit /b 1
call "%~dp0build.bat" || exit /b 1
call "%~dp0build-installer.bat" || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-msix.ps1"
if errorlevel 1 (echo WARNING: build\RemoteDesk.msix was not built ^(needs the Windows SDK^). & exit /b 2)
for /f "tokens=3" %%v in ('findstr /c:"define RD_VERSION_MAJOR" src\core\version.h') do set MA=%%v
for /f "tokens=3" %%v in ('findstr /c:"define RD_VERSION_MINOR" src\core\version.h') do set MI=%%v
for /f "tokens=3" %%v in ('findstr /c:"define RD_VERSION_PATCH" src\core\version.h') do set PA=%%v
echo.
echo Built version %MA%.%MI%.%PA%:
echo   build\RemoteDesk.exe
echo   build\setup.exe        - choose "Install for all users" for lock-screen access
echo   build\RemoteDesk.msix  - Microsoft Store
