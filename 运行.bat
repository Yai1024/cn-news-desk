@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

set "GXX=C:\Users\Jerry\.vscode\mingw64\bin\g++.exe"
if not exist "%GXX%" set "GXX=g++"

"%GXX%" --version >nul 2>nul
if errorlevel 1 goto runonly

echo [build] Compiling main.cpp ...
"%GXX%" -std=c++17 -O2 -static -static-libgcc -static-libstdc++ main.cpp -o fetch_news.exe -lwinhttp -lshell32
if errorlevel 1 (
  echo [build] Compile failed.
  if not exist "%~dp0fetch_news.exe" (
    pause
    exit /b 1
  )
  echo [build] Using existing fetch_news.exe
)

:runonly
if not exist "%~dp0fetch_news.exe" (
  echo fetch_news.exe not found and g++ is unavailable.
  pause
  exit /b 1
)

"%~dp0fetch_news.exe" %*
echo.
echo Done. Output files are in the "output" folder.
pause
