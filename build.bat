@echo off
rem Builds build\RemoteDesk.exe with MSVC (run from a "Developer Command Prompt")
rem or MinGW-w64 g++ (on PATH). The MinGW build is static: copy the single exe anywhere.
setlocal
cd /d "%~dp0"
if not exist build mkdir build

where cl >nul 2>nul && goto msvc
where g++ >nul 2>nul && goto mingw
echo No C++ compiler found. Install Visual Studio Build Tools or MinGW-w64 (winget install BrechtSanders.WinLibs.POSIX.UCRT).
exit /b 1

:msvc
rc /nologo /fo build\app.res src\app.rc || exit /b 1
cl /nologo /std:c++20 /O2 /EHsc /W3 /DUNICODE /D_UNICODE /Fobuild\ /Febuild\RemoteDesk.exe src\*.cpp build\app.res ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO ws2_32.lib user32.lib gdi32.lib ole32.lib oleaut32.lib windowscodecs.lib bcrypt.lib || exit /b 1
goto done

:mingw
windres -I src src\app.rc -O coff -o build\app.res.o || exit /b 1
g++ -std=c++17 -O2 -Wall -DUNICODE -D_UNICODE -mwindows -static -o build\RemoteDesk.exe ^
    src\main.cpp src\host.cpp src\viewer.cpp src\crypto.cpp src\codec.cpp src\welcome.cpp src\startup.cpp build\app.res.o ^
    -lws2_32 -lgdi32 -lole32 -loleaut32 -lwindowscodecs -lbcrypt -luuid || exit /b 1

:done
echo Built build\RemoteDesk.exe
