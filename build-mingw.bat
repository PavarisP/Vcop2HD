@echo off
setlocal

set "GPP=g++"
where %GPP% >nul 2>nul
if errorlevel 1 set "GPP=x86_64-w64-mingw32-g++"
where %GPP% >nul 2>nul
if errorlevel 1 (
  echo A 64-bit MinGW-w64 g++ was not found on PATH.
  exit /b 1
)

if not exist dist mkdir dist

%GPP% -std=c++17 -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++ -mwindows -s -o dist\VC2HD.exe src\main.cpp -lpsapi
if errorlevel 1 exit /b 1

echo Built dist\VC2HD.exe
