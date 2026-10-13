@echo off
rem Runs a command in the MSVC x64 environment (cl, ninja, VCPKG_ROOT):
rem   tools\dev\vsenv.cmd cmake --workflow --preset debug
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VNMAKI_VS=%%i"
call "%VNMAKI_VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
%*
