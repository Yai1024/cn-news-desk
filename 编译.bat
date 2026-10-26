@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

set "GXX=C:\Users\Jerry\.vscode\mingw64\bin\g++.exe"
if not exist "%GXX%" set "GXX=g++"

echo [build] Compiling main.cpp ...
"%GXX%" -std=c++17 -O2 -static -static-libgcc -static-libstdc++ main.cpp -o fetch_news.exe -lwinhttp -lshell32
if errorlevel 1 (
  echo [build] Compile failed.
  pause
  exit /b 1
)
echo [build] OK: fetch_news.exe
pause
