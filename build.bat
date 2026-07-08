@echo off
setlocal
cd /d "%~dp0"

for /f %%i in ('powershell -NoLogo -NoProfile -Command "(Get-Date).ToString('yyMMddHHmm')"') do set "HOMEPANEL_BUILD_VERSION=%%i"
echo Using build version %HOMEPANEL_BUILD_VERSION%

cmake -S native -B native/build-local -G "Visual Studio 17 2022" -A x64 -DHOMEPANEL_BUILD_VERSION=%HOMEPANEL_BUILD_VERSION%
if errorlevel 1 exit /b 1

cmake --build native/build-local --config Release --parallel
if errorlevel 1 exit /b 1

echo.
echo Build succeeded: native\build-local\Release
start "" "%~dp0native\build-local\Release"
endlocal
