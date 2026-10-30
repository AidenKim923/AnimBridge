@echo off
setlocal
if exist "%~dp0.venv\Scripts\python.exe" set "PATH=%~dp0.venv\Scripts;%PATH%"
set "ANIMBRIDGE_APP=%~dp0_verify\native\build\Release\AnimBridge.exe"
if not exist "%ANIMBRIDGE_APP%" (
  echo AnimBridge has not been built. Run these commands from this folder:
  echo cmake -S . -B _verify/native/build -A x64 -DBUILD_TESTING=ON
  echo cmake --build _verify/native/build --config Release --parallel
  pause
  exit /b 1
)
start "" "%ANIMBRIDGE_APP%"
