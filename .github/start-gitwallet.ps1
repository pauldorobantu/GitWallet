# Runs dist\GitWallet.exe the way a user's PC would: from a copy of the folder
# under a path with a space, with no Qt on PATH and no Qt variables, on the real
# Windows platform plugin the folder ships. Exits with the app's exit code.
param(
    [string[]] $Arguments,
    [string] $StdOut
)

$copy = Join-Path $env:RUNNER_TEMP 'GitWallet start'
if (-not (Test-Path $copy)) {
    Copy-Item dist $copy -Recurse
}

$env:PATH = ($env:PATH -split ';' | Where-Object { $_ -notmatch '\\Qt\\' }) -join ';'
foreach ($name in 'QT_PLUGIN_PATH', 'QML2_IMPORT_PATH', 'QT_ROOT_DIR', 'Qt6_DIR', 'QT_QPA_PLATFORM', 'QT_QPA_PLATFORM_PLUGIN_PATH') {
    Remove-Item "Env:$name" -ErrorAction SilentlyContinue
}

$start = @{
    FilePath         = Join-Path $copy 'GitWallet.exe'
    ArgumentList     = $Arguments
    WorkingDirectory = $copy
    Wait             = $true
    PassThru         = $true
    NoNewWindow      = $true
}
if ($StdOut) {
    $start.RedirectStandardOutput = Join-Path $PWD $StdOut
}
$process = Start-Process @start
exit $process.ExitCode
