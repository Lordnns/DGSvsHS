# Usage:
#   .\scripts\publish_windows.ps1                # stop running → rebuild → publish to publish/
#   .\scripts\publish_windows.ps1 -Run           # …then launch the published exe in this window
#   .\scripts\publish_windows.ps1 -GodMode       # publish to publish-godmode/ with GODMODE_DEFAULT
#   .\scripts\publish_windows.ps1 -GodMode -Run  # both

param(
    [switch]$Run,
    [switch]$GodMode
)

$ErrorActionPreference = "Stop"

$projectRoot = Resolve-Path "$PSScriptRoot/.."

# Tagged output directory so godmode and normal builds can coexist on disk.
$flavorSuffix = if ($GodMode) { "-godmode" } else { "" }
$publishDir   = "$projectRoot/publish$flavorSuffix"
$publishedExe = "$publishDir\DGSvsHS.ArchServer.exe"

# ---------- 1. Stop any running server so the .exe and its plugin .dlls aren't file-locked. ----------
$running = Get-Process -Name DGSvsHS.ArchServer -ErrorAction SilentlyContinue
if ($running) {
    Write-Host "[publish] stopping $($running.Count) running ArchServer process(es)…"
    $running | Stop-Process -Force
    Start-Sleep -Milliseconds 300
}

# ---------- 2. Publish (which triggers a build first). ----------
Write-Host "[publish] dotnet publish -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true"
Push-Location $projectRoot
try {
    # IncludeAllContentForSelfExtract is required because StirlingLabs.MsQuic's static initializer
    # reads `typeof(MsQuic).Assembly.Location` — empty under the default single-file packing
    # (managed assemblies stay in the bundle). Self-extracting ALL content guarantees Location
    # points to a real on-disk path and msquic-openssl.dll ends up next to it.
    $msbuildProps = @(
        "-p:PublishSingleFile=true",
        "-p:IncludeAllContentForSelfExtract=true"
    )
    if ($GodMode) {
        Write-Host "[publish] flavor: godmode (defining GODMODE_DEFAULT)"
        $msbuildProps += "-p:GodModeDefault=true"
    }

    dotnet publish -c Release `
                   -r win-x64 `
                   --self-contained true `
                   $msbuildProps `
                   -o $publishDir
    if ($LASTEXITCODE -ne 0) {
        throw "publish failed (exit $LASTEXITCODE)"
    }
} finally {
    Pop-Location
}

Write-Host ""
Write-Host "[done] $publishedExe"
Write-Host "       (double-clickable; 62.5 Hz tick loop, heartbeat once per second)"

# ---------- 3. Optionally launch the freshly-built exe. ----------
if ($Run) {
    Write-Host ""
    Write-Host "[run] launching $publishedExe …"
    & $publishedExe
}
