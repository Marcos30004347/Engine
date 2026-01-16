# Stop on first error
$ErrorActionPreference = "Stop"

$BinName = "wgslc-naga"
$Target = "release"

Write-Host "Building $BinName ($Target)..."
cargo build --release

$SourceExe = "target\$Target\$BinName.exe"

if (-not (Test-Path $SourceExe)) {
    throw "Build failed: $SourceExe not found"
}

# Install location (per-user, no admin required)
$InstallDir = "$env:LOCALAPPDATA\Programs\wgslc-naga\bin"

Write-Host "Installing to $InstallDir"
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

Copy-Item $SourceExe "$InstallDir\$BinName.exe" -Force

# Add to PATH (user scope)
$UserPath = [Environment]::GetEnvironmentVariable("PATH", "User")

if (-not $UserPath.Contains($InstallDir)) {
    Write-Host "Adding $InstallDir to PATH (user scope)"
    [Environment]::SetEnvironmentVariable(
        "PATH",
        "$UserPath;$InstallDir",
        "User"
    )
} else {
    Write-Host "PATH already contains install directory"
}

Write-Host "`nInstalled $BinName successfully"
Write-Host "Restart your terminal to use '$BinName'"
