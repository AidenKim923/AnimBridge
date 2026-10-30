@echo off
setlocal
cd /d "%~dp0"
echo AnimBridge includes Microsoft Edge WebView2 Fixed Version Runtime.
echo Its license terms are in licenses\WebView2-Fixed-LICENSE.txt.
echo By using that runtime you accept those Microsoft terms.
echo.
echo Preparing read/execute access for the bundled WebView2 runtime...
"%SystemRoot%\System32\icacls.exe" "%~dp0runtime\webview2" /grant *S-1-15-2-1:(OI)(CI)(RX) /grant *S-1-15-2-2:(OI)(CI)(RX) /T /Q
if errorlevel 1 (
  echo Preparation failed. Extract the complete ZIP into a folder you own.
  pause
  exit /b 1
)
start "" "%~dp0AnimBridge.exe"
