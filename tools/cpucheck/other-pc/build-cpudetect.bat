@echo off
rem Builds cpudetect.exe from cpudetect.cpp with the Clang that comes with Visual Studio (C++ and
rem the Clang component needed). cpudetect.exe is already included.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
"%VSROOT%\VC\Tools\Llvm\x64\bin\clang-cl.exe" /nologo /O2 /EHsc /std:c++20 /MT cpudetect.cpp /Fe:cpudetect.exe advapi32.lib
