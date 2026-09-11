@echo off
SETLOCAL EnableDelayedExpansion

set GAME=%1

if not "%GAME%"=="five" if not "%GAME%"=="rdr3" (
    echo Invalid game specified: %GAME%
    exit /b 1
)

:: check if Yarn exists

where /q yarn || exit /b !ERRORLEVEL!

:: make sure no app leftovers
if exist %~dp0\data\app (
    rmdir /s /q %~dp0\data\app\
)
if exist %~dp0\data_big\app (
    rmdir /s /q %~dp0\data_big\app\
)

:: build mpMenu from source
::
:: upstream downloads a prebuilt bundle from their CDN here. We cannot: ext/cfx-ui
:: holds our own UI, put there by Invoke-UI (code/tools/ci/psm1/uiReplace.psm1),
:: and a downloaded bundle would silently ship upstream's menu instead of ours.
echo Building mpMenu...
pushd ..\cfx-ui\

call yarn --ignore-engines --frozen-lockfile || exit /b !ERRORLEVEL!
call yarn test || exit /b !ERRORLEVEL!

if exist build (
    rmdir /s /q build
)

call yarn build 2>&1 || exit /b !ERRORLEVEL!

echo Copying mpMenu files...
move /y build\mpMenu %~dp0\data\app || exit /b !ERRORLEVEL!
popd

:: build loading screen
echo Building loading screen...
pushd loadscreen
call yarn || exit /b !ERRORLEVEL!
call node_modules\.bin\webpack || exit /b !ERRORLEVEL!

echo Copying loadscreen files...
xcopy /y /e dist\*.* %~dp0\data\loadscreen\ || exit /b !ERRORLEVEL!
popd

:: split the heavy assets out into data_big so a small UI change does not make
:: every client re-download the media
echo Moving large files to data_big...
mkdir %~dp0\data_big\app\static\media
move /y %~dp0\data\app\static\media\*.* %~dp0\data_big\app\static\media\

mkdir %~dp0\data_big\loadscreen
move /y %~dp0\data\loadscreen\*.jpg %~dp0\data_big\loadscreen\

powershell -ExecutionPolicy Unrestricted .\make_dates.ps1 %~dp0\data
powershell -ExecutionPolicy Unrestricted .\make_dates.ps1 %~dp0\data_big

if exist %~dp0\data.zip (
    del %~dp0\data.zip
)
if exist %~dp0\data_big.zip (
    del %~dp0\data_big.zip
)

:: 'a' rather than 'u': the archives are built from our own output only, there is
:: no downloaded bundle underneath to update
%~dp0\..\..\code\tools\ci\7z a -mx=0 %~dp0\data.zip %~dp0\data\* || exit /b !ERRORLEVEL!
%~dp0\..\..\code\tools\ci\7z a -mx=0 %~dp0\data_big.zip %~dp0\data_big\* || exit /b !ERRORLEVEL!

exit /B 0
