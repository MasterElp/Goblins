param(
    [string]$ConfigPath = "$PSScriptRoot\config.json"
)

$Root = $PSScriptRoot

function Find-Binary($searchDir, $name) {
    Get-ChildItem -Path $searchDir -Recurse -Filter "$name.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}

$serverBin = Find-Binary "$Root\build\server" "server"
$clientBin = Find-Binary "$Root\clients\desktop\build" "client"

if (-not $serverBin) {
    Write-Host "Server binary not found. Run build.ps1 first."
    exit 1
}
if (-not $clientBin) {
    Write-Host "Client binary not found. Run build.ps1 first."
    exit 1
}

Write-Host "Starting server: $serverBin $ConfigPath"
# Сервер — в отдельном окне консоли: клиенту нужна СВОЯ настоящая
# Win32-консоль в текущем окне (для _kbhit/_getch и ANSI-очистки экрана),
# поэтому его запускаем напрямую ниже, а не через Start-Process.
$serverProcess = Start-Process -FilePath $serverBin -ArgumentList "`"$ConfigPath`"" -PassThru

Start-Sleep -Seconds 1

try {
    Write-Host "Starting client: $clientBin $ConfigPath"
    & $clientBin $ConfigPath
}
finally {
    if ($serverProcess -and -not $serverProcess.HasExited) {
        Write-Host "Stopping server (pid $($serverProcess.Id))..."
        Stop-Process -Id $serverProcess.Id -Force -ErrorAction SilentlyContinue
    }
}
