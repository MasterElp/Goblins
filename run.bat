@echo off
setlocal enabledelayedexpansion

rem %~dp0 всегда заканчивается на "\" — убираем его один раз здесь, чтобы
rem everywhere дальше был предсказуемый "%ROOT%\..." без риска склеить
rem кавычки (см. build.bat).
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set SERVER_BIN=
for %%f in (
    "%ROOT%\build\server\server.exe"
    "%ROOT%\build\server\Release\server.exe"
    "%ROOT%\build\server\Debug\server.exe"
) do (
    if exist %%f set SERVER_BIN=%%~f
)

set CLIENT_BIN=
for %%f in (
    "%ROOT%\clients\desktop\build\client.exe"
    "%ROOT%\clients\desktop\build\Release\client.exe"
    "%ROOT%\clients\desktop\build\Debug\client.exe"
) do (
    if exist %%f set CLIENT_BIN=%%~f
)

if "%SERVER_BIN%"=="" (
    echo Server binary not found. Run build.bat first.
    exit /b 1
)
if "%CLIENT_BIN%"=="" (
    echo Client binary not found. Run build.bat first.
    exit /b 1
)

echo Starting server: %SERVER_BIN%
rem Сервер — в отдельном окне со своим config.json из корня репозитория.
start "Goblins Server" "%SERVER_BIN%" "%ROOT%\config.json"

timeout /t 1 /nobreak >nul

rem Клиент запускаем без аргумента — он сам найдёт (или создаст) свой
rem config.json рядом со своим исполняемым файлом.
echo Starting client: %CLIENT_BIN%
"%CLIENT_BIN%"

echo Stopping server...
taskkill /FI "WINDOWTITLE eq Goblins Server*" /T /F >nul 2>&1

endlocal
