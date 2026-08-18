# Fast toolchain download script
# Usage: .\download-toolchain.ps1

$ErrorActionPreference = "Stop"

Write-Host "=== OLLVM17 Toolchain Downloader ===" -ForegroundColor Cyan

# Check if gh CLI is installed
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    Write-Host "Installing GitHub CLI..." -ForegroundColor Yellow
    winget install GitHub.cli --silent
    Write-Host "Please restart your terminal and run this script again." -ForegroundColor Green
    exit
}

# Check if logged in
$ghAuth = gh auth status 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "Please login to GitHub CLI:" -ForegroundColor Yellow
    gh auth login
}

# Get latest successful run
Write-Host "Finding latest successful build..." -ForegroundColor Yellow
$runs = gh run list --repo yusufnav2025-ui/winollvmhikari --workflow "Build OLLVM17 (Windows, for Android NDK r26)" --status success --limit 1 --json databaseId,conclusion | ConvertFrom-Json

if ($runs.Count -eq 0) {
    Write-Host "No successful builds found!" -ForegroundColor Red
    exit 1
}

$runId = $runs[0].databaseId
Write-Host "Latest build: Run #$runId" -ForegroundColor Green

# Download artifact
Write-Host "Downloading toolchain (2-3GB, this will take a few minutes)..." -ForegroundColor Yellow
Write-Host "Download location: $(Get-Location)" -ForegroundColor Cyan

gh run download $runId --repo yusufnav2025-ui/winollvmhikari --name "ollvm17-windows-ndk-r26"

if ($LASTEXITCODE -eq 0) {
    Write-Host "`n=== Download Complete ===" -ForegroundColor Green
    Write-Host "Toolchain location: $(Get-Location)\ollvm17-windows-ndk-r26" -ForegroundColor Cyan
    Write-Host "`nNext steps:" -ForegroundColor Yellow
    Write-Host "1. Extract the toolchain to your NDK location"
    Write-Host "2. Set NDK_TOOLCHAIN_VERSION or use full path in Android.mk/CMakeLists.txt"
    Write-Host "3. Build with obfuscation flags: -mllvm -fla -mllvm -bcf -mllvm -sub"
} else {
    Write-Host "Download failed!" -ForegroundColor Red
    exit 1
}
