@echo off
rem Double-click to run the C++ check: 3 minutes alone, then 3 minutes beside its burst load.
cd /d "%~dp0"
echo ===== without load ===== 
cpudetect.exe --no-bursts --seconds 180
echo.
echo ===== beside the burst load =====
cpudetect.exe --seconds 180
pause
