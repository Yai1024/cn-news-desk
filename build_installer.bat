@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

set "GXX=C:\Users\Jerry\.vscode\mingw64\bin\g++.exe"
if not exist "%GXX%" set "GXX=g++"
set "WINDRES=C:\Users\Jerry\.vscode\mingw64\bin\windres.exe"
if not exist "%WINDRES%" set "WINDRES=windres"

"%GXX%" --version >nul 2>nul
if errorlevel 1 (
  echo [build] g++ not found.
  pause
  exit /b 1
)
"%WINDRES%" --version >nul 2>nul
if errorlevel 1 (
  echo [build] windres not found.
  pause
  exit /b 1
)

if not exist news_desk.exe (
  echo [build] news_desk.exe not found. Run build_desk.bat first.
  pause
  exit /b 1
)
if not exist agent_host.exe (
  echo [build] agent_host.exe not found. Run build_desk.bat first.
  pause
  exit /b 1
)
if not exist fetch_news.exe (
  echo [build] fetch_news.exe not found. Run 编译.bat first.
  pause
  exit /b 1
)
if not exist skill\SKILL.md (
  echo [build] skill\SKILL.md not found.
  pause
  exit /b 1
)

if not exist installer_build mkdir installer_build
copy /y installer\installer.rc installer_build\ >nul
copy /y installer\uninstaller.rc installer_build\ >nul
copy /y installer\installer.manifest installer_build\ >nul

echo [1/5] Building icon ...
"%GXX%" -std=c++17 -O2 -static -static-libgcc -static-libstdc++ tools\make_icon.cpp -o installer_build\make_icon.exe -lgdiplus -lgdi32 -lole32
if errorlevel 1 goto fail
installer_build\make_icon.exe installer_build\app.ico
if errorlevel 1 goto fail

echo [2/5] Building uninstaller ...
pushd installer_build
"%WINDRES%" -i uninstaller.rc -o uninstaller_res.o
if errorlevel 1 (
  popd
  goto fail
)
popd
"%GXX%" -std=c++17 -O2 -static -static-libgcc -static-libstdc++ -mwindows installer\uninstall.cpp installer_build\uninstaller_res.o -o installer_build\uninstall.exe -lcomctl32 -lole32 -luuid -lshell32 -ladvapi32 -lgdi32 -luser32
if errorlevel 1 goto fail

echo [3/5] Packing payload ...
"%GXX%" -std=c++17 -O2 -static -static-libgcc -static-libstdc++ tools\pack_payload.cpp -o installer_build\pack_payload.exe
if errorlevel 1 goto fail
installer_build\pack_payload.exe . installer\payload.txt installer_build\payload.bin
if errorlevel 1 goto fail

echo [4/5] Building installer resources ...
pushd installer_build
"%WINDRES%" -i installer.rc -o installer_res.o
if errorlevel 1 (
  popd
  goto fail
)
popd

echo [5/5] Linking installer ...
set "OUT=政策解读台-安装程序.exe"
"%GXX%" -std=c++17 -O2 -static -static-libgcc -static-libstdc++ -mwindows installer\installer.cpp installer_build\installer_res.o -o "%OUT%" -lcomctl32 -lole32 -luuid -lshell32 -ladvapi32 -lgdiplus -lgdi32 -luser32
if errorlevel 1 goto fail

echo.
echo [build] OK: %OUT%
for %%F in ("%OUT%") do echo [build] Size: %%~zF bytes
pause
exit /b 0

:fail
echo [build] Build failed.
pause
exit /b 1
