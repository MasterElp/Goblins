param(
    [string]$BuildConfig = "Release"
)

$Root = $PSScriptRoot

Write-Host "== Building server (config: $BuildConfig) =="
cmake -S $Root -B "$Root\build" -DCMAKE_BUILD_TYPE=$BuildConfig
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build "$Root\build" --config $BuildConfig -j
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "== Building client (config: $BuildConfig) =="
cmake -S "$Root\clients\desktop" -B "$Root\clients\desktop\build" -DCMAKE_BUILD_TYPE=$BuildConfig
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build "$Root\clients\desktop\build" --config $BuildConfig -j
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "Done."
