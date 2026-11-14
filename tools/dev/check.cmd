@echo off
rem Local version of the CI jobs that run on Windows: format check, debug and release workflows.
rem Needs uv (clang-format 18 matches CI). clang-tidy and the Linux/macOS jobs run only in CI.
rem   tools\dev\check.cmd
pushd "%~dp0..\.."
git ls-files "*.cpp" "*.hpp" "*.h" > "%TEMP%\vnmaki_format_files.txt"
uvx clang-format@18.1.8 --dry-run --Werror --files="%TEMP%\vnmaki_format_files.txt" || (popd & exit /b 1)
popd
call "%~dp0vsenv.cmd" cmake --workflow --preset debug || exit /b 1
call "%~dp0vsenv.cmd" cmake --workflow --preset release || exit /b 1
