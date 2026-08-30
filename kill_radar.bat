@echo off
net session >nul 2>&1
if %ERRORLEVEL% neq 0 (
    powershell -Command "Start-Process '%~f0' -Verb RunAs"
    exit /b
)
echo Closing radar processes...
taskkill /F /IM radar.exe >nul 2>&1
taskkill /F /IM standalone_radar_drv.exe >nul 2>&1
taskkill /F /IM standalone_radar_drv_v2.exe >nul 2>&1
taskkill /F /IM standalone_radar.exe >nul 2>&1
echo Done!
timeout /t 2 >nul
