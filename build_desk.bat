@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

set "GXX=C:\Users\Jerry\.vscode\mingw64\bin\g++.exe"
if not exist "%GXX%" set "GXX=g++"

"%GXX%" --version >nul 2>nul
if errorlevel 1 (
  echo [build] g++ not found. Install MinGW or edit GXX in this file.
  pause
  exit /b 1
)

echo [build] Compiling agent_host.exe ...
"%GXX%" -std=c++17 -O2 -static -static-libgcc -static-libstdc++ agent_host.cpp -o agent_host.exe -lwinhttp
if errorlevel 1 goto fail

echo [build] Compiling news_desk.exe ...
"%GXX%" -std=c++17 -O2 -static -static-libgcc -static-libstdc++ -mwindows news_desk.cpp -o news_desk.exe -lshell32 -luser32 -lgdi32
if errorlevel 1 goto fail

echo [build] OK: news_desk.exe  agent_host.exe
pause
exit /b 0

:fail
echo [build] Compile failed.
pause
exit /b 1
