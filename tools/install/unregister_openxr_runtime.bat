@echo off
rem Puts the previous Windows OpenXR runtime back (undoes register_openxr_runtime.bat). Asks for administrator rights.
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0register_openxr_runtime.ps1" -Unregister
