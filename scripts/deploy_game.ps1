# Copy freshly built netplay + caster artifacts into dist/game for EFZ trial deployment.
param(
    [string]$GameDir = "",
    [switch]$DeploySymbols
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist\game"

$netplayDll = Join-Path $root "trial_hitbox_viewer\build\bin\Release\netplay.dll"
$netplayPdb = Join-Path $root "trial_hitbox_viewer\build\bin\Release\netplay.pdb"
$netplaySdl = Join-Path $root "trial_hitbox_viewer\build\bin\Release\SDL3.dll"
$casterExe = Join-Path $root "trial_caster\build\bin\EFZ FF Netplay.exe"
$directDraw = Join-Path $root "ddraw_wrapper\build\Release\ddraw.dll"
$directDrawCache = Join-Path $root "ddraw_wrapper\build\CMakeCache.txt"
$wineLauncher = Join-Path $root "scripts\launch_wine.sh"
$winePreflight = Join-Path $root "scripts\wine_preflight.sh"
$gameReadme = Join-Path $root "assets\README.txt"

New-Item -ItemType Directory -Force -Path $dist | Out-Null

function Copy-Artifact([string]$src, [string]$name) {
    if (-not (Test-Path $src)) {
        throw "Missing build output: $src (build Release first)"
    }
    Copy-Item -Force $src (Join-Path $dist $name)
    Write-Host "Copied $name"
}

function Remove-GeneratedWrapperLog([string]$directory) {
    $logPath = Join-Path $directory "ddraw_wrapper.log"
    if (Test-Path -LiteralPath $logPath -PathType Leaf) {
        Remove-Item -LiteralPath $logPath -Force
        Write-Host "Removed stale ddraw_wrapper.log"
    }
}

function Assert-DirectDrawLoggingDisabled {
    if (-not (Test-Path -LiteralPath $directDrawCache -PathType Leaf)) {
        throw "Missing DirectDraw build cache: $directDrawCache (run ddraw_wrapper\build.bat Release first)"
    }
    $diagnosticBuild = Select-String `
        -LiteralPath $directDrawCache `
        -SimpleMatch "DDRAW_WRAPPER_ENABLE_LOGGING:BOOL=ON" `
        -Quiet
    if ($diagnosticBuild) {
        throw "Refusing to deploy a diagnostic ddraw.dll. Rebuild with ddraw_wrapper\build.bat Release."
    }
}

Assert-DirectDrawLoggingDisabled
Copy-Artifact $netplayDll "netplay.dll"
Copy-Artifact $netplaySdl "SDL3.dll"
Copy-Artifact $casterExe "EFZ FF Netplay.exe"
Copy-Artifact $directDraw "ddraw.dll"
Copy-Artifact $wineLauncher "launch-wine.sh"
Copy-Artifact $winePreflight "wine-preflight.sh"
Copy-Artifact $gameReadme "README.txt"
Remove-GeneratedWrapperLog $dist

$bannerAsset = Join-Path $root "assets\banner.png"
if (Test-Path $bannerAsset) {
    New-Item -ItemType Directory -Force -Path (Join-Path $dist "assets") | Out-Null
    Copy-Item -Force $bannerAsset (Join-Path $dist "assets\banner.png")
    Write-Host "Copied assets\banner.png"
}

if ($DeploySymbols -and (Test-Path $netplayPdb)) {
    Copy-Item -Force $netplayPdb (Join-Path $dist "netplay.pdb")
    Write-Host "Copied netplay.pdb"
}

if ($GameDir -ne "") {
    if (-not (Test-Path $GameDir)) {
        throw "Game directory not found: $GameDir"
    }
    # Best-effort per file: a locked file must not abort deploying the others.
    function Copy-IfPossible([string]$src, [string]$dst) {
        if (-not (Test-Path $src)) { return }
        try { Copy-Item -Force $src $dst }
        catch { Write-Warning "skip locked/in-use: $dst ($($_.Exception.Message))" }
    }
    Copy-IfPossible (Join-Path $dist "netplay.dll") (Join-Path $GameDir "netplay.dll")
    Copy-IfPossible (Join-Path $dist "SDL3.dll") (Join-Path $GameDir "SDL3.dll")
    Copy-IfPossible (Join-Path $dist "EFZ FF Netplay.exe") (Join-Path $GameDir "EFZ FF Netplay.exe")
    Copy-IfPossible (Join-Path $dist "ddraw.dll") (Join-Path $GameDir "ddraw.dll")
    Copy-IfPossible (Join-Path $dist "launch-wine.sh") (Join-Path $GameDir "launch-wine.sh")
    Copy-IfPossible (Join-Path $dist "wine-preflight.sh") (Join-Path $GameDir "wine-preflight.sh")
    Copy-IfPossible (Join-Path $dist "README.txt") (Join-Path $GameDir "README.txt")
    Remove-GeneratedWrapperLog $GameDir
    if ($DeploySymbols) {
        Copy-IfPossible (Join-Path $dist "netplay.pdb") (Join-Path $GameDir "netplay.pdb")
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $GameDir "assets") | Out-Null
    Copy-IfPossible (Join-Path $dist "assets\banner.png") (Join-Path $GameDir "assets\banner.png")
    # Caster SDL is the same build; game folder only needs one SDL3.dll for both.
    Write-Host "Deployed netplay.dll, SDL3.dll, ddraw.dll, EFZ FF Netplay.exe to $GameDir"
}

Write-Host "Deploy complete: $dist"
