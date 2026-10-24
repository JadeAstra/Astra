@echo off
rem Double-click to run the check (about 6 minutes). Results go to results.txt.
rem On a processor with two kinds of core, give the fast ones, e.g.: run.bat --mask FFFF
cd /d "%~dp0"
astra.exe run.astra %*
pause
