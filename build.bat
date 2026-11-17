@echo off
rem Builds build\RemoteDesk.exe with MSVC (run from a "Developer Command Prompt")
rem or MinGW-w64 g++ (on PATH). The MinGW build is static: copy the single exe anywhere.
rem Every .cpp under src\ (core, app, remote, ui, screens) is compiled.
setlocal enabledelayedexpansion
cd /d "%~dp0"
if not exist build mkdir build

set SRCS=
for /r src %%F in (*.cpp) do set SRCS=!SRCS! "%%F"

where cl >nul 2>nul && goto msvc
where g++ >nul 2>nul && goto mingw
echo No C++ compiler found. Install Visual Studio Build Tools or MinGW-w64 (winget install BrechtSanders.WinLibs.POSIX.UCRT).
exit /b 1

:msvc
if not exist build\obj mkdir build\obj
rc /nologo /I src /fo build\app.res src\app.rc || exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /W3 /utf-8 /DUNICODE /D_UNICODE /I src /Fobuild\obj\ /Febuild\RemoteDesk.exe %SRCS% build\app.res ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO ws2_32.lib user32.lib gdi32.lib ole32.lib oleaut32.lib windowscodecs.lib bcrypt.lib ^
   gdiplus.lib dwmapi.lib uxtheme.lib advapi32.lib shell32.lib crypt32.lib winhttp.lib ^
   mfplat.lib mfreadwrite.lib mfuuid.lib comdlg32.lib wtsapi32.lib iphlpapi.lib || exit /b 1
goto done

:mingw
rem GCC can't find its own files when its folder path contains a space (e.g. a user
rem name like "Jane Doe"), so run it through the short 8.3 path.
for /f "delims=" %%G in ('where g++') do if not defined GPP set "GPP=%%~sG" & set "GBIN=%%~dpsG"
set "GCC_EXEC_PREFIX=%GBIN%..\lib\gcc\"
tasklist /fi "imagename eq RemoteDesk.exe" | "%SystemRoot%\System32\find.exe" /i "RemoteDesk.exe" >nul && echo Note: close RemoteDesk first if it was started from build\ ^(the exe can't be replaced while it runs^).
"%GBIN%windres.exe" -I src src\app.rc -O coff -o build\app.res.o || exit /b 1
"%GPP%" -std=c++17 -O2 -Wall -DUNICODE -D_UNICODE -I src -mwindows -static -o build\RemoteDesk.exe %SRCS% build\app.res.o ^
    -lws2_32 -lgdi32 -lole32 -loleaut32 -lwindowscodecs -lbcrypt -luuid -lgdiplus -ldwmapi -luxtheme -ladvapi32 ^
    -lshell32 -lcrypt32 -lwinhttp -lmfplat -lmfreadwrite -lmfuuid -lcomdlg32 -lwtsapi32 -liphlpapi > build\gcc.log 2>&1
set "ERR=%ERRORLEVEL%"
findstr /v /c:"multiple non-default manifests" build\gcc.log
del build\gcc.log build\app.res.o 2>nul
if not "%ERR%"=="0" (echo BUILD FAILED & exit /b 1)

:done
echo Built build\RemoteDesk.exe
