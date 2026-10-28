@echo off
chcp 65001 >nul
setlocal
cd /d "%~dp0"

if not exist "%~dp0fetch_news.exe" (
  echo 找不到 fetch_news.exe，请重新运行安装程序。
  pause
  exit /b 1
)

"%~dp0fetch_news.exe" %*
echo.
echo 完成。稿件保存在 "output" 文件夹。
pause
