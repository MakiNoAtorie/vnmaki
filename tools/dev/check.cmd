@echo off
rem Local version of the Windows CI jobs: debug and release workflows.
rem   tools\dev\check.cmd
call "%~dp0vsenv.cmd" cmake --workflow --preset debug || exit /b 1
call "%~dp0vsenv.cmd" cmake --workflow --preset release || exit /b 1
