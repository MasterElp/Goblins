@echo off
setlocal

set BUILD_CONFIG=%1
if "%BUILD_CONFIG%"=="" set BUILD_CONFIG=Release

rem %~dp0 всегда заканчивается на "\" — если сразу за ним поставить
rem закрывающую кавычку, cmd воспримет "\"" как экранированную кавычку и
rem не закроет строку. Поэтому убираем завершающий "\" один раз здесь.
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

echo == Building server (config: %BUILD_CONFIG%) ==
cmake -S "%ROOT%" -B "%ROOT%\build" -DCMAKE_BUILD_TYPE=%BUILD_CONFIG%
if errorlevel 1 exit /b 1
cmake --build "%ROOT%\build" --config %BUILD_CONFIG% -j
if errorlevel 1 exit /b 1

echo.
echo == Building client (config: %BUILD_CONFIG%) ==
cmake -S "%ROOT%\clients\desktop" -B "%ROOT%\clients\desktop\build" -DCMAKE_BUILD_TYPE=%BUILD_CONFIG%
if errorlevel 1 exit /b 1
cmake --build "%ROOT%\clients\desktop\build" --config %BUILD_CONFIG% -j
if errorlevel 1 exit /b 1

echo.
echo Done.

endlocal
