# OLLVM17 NDK Replacement Script
# Replaces stock NDK r26 toolchain with OLLVM17 obfuscation-enabled clang
# Usage: .\install-ollvm-ndk.ps1

$ErrorActionPreference = "Stop"

Write-Host "=== OLLVM17 NDK Installer ===" -ForegroundColor Cyan
Write-Host ""

# Configuration
$NDK_PATH = "C:\Users\nabee\AppData\Local\Android\Sdk\ndk\26.3.11579264"
$TOOLCHAIN_DIR = "ollvm17-windows-ndk-r26"
$BACKUP_SUFFIX = "_stock_backup_$(Get-Date -Format 'yyyyMMdd_HHmmss')"

# Check if NDK exists
if (-not (Test-Path $NDK_PATH)) {
    Write-Host "ERROR: NDK not found at: $NDK_PATH" -ForegroundColor Red
    Write-Host "Please check your NDK installation path." -ForegroundColor Yellow
    exit 1
}

Write-Host "Found NDK at: $NDK_PATH" -ForegroundColor Green

# Check if OLLVM toolchain exists
if (-not (Test-Path $TOOLCHAIN_DIR)) {
    Write-Host "ERROR: OLLVM toolchain not found!" -ForegroundColor Red
    Write-Host "Expected location: $(Get-Location)\$TOOLCHAIN_DIR" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Please download the toolchain first:" -ForegroundColor Yellow
    Write-Host "  1. Run: .\download-toolchain.ps1" -ForegroundColor Cyan
    Write-Host "  OR" -ForegroundColor Yellow
    Write-Host "  2. Manually download from GitHub Actions and extract here" -ForegroundColor Cyan
    exit 1
}

Write-Host "Found OLLVM toolchain at: $(Get-Location)\$TOOLCHAIN_DIR" -ForegroundColor Green
Write-Host ""

# Show what will be replaced
Write-Host "=== Installation Plan ===" -ForegroundColor Yellow
Write-Host "The following directories will be backed up and replaced:" -ForegroundColor White
Write-Host "  - $NDK_PATH\toolchains\llvm\prebuilt\windows-x86_64\bin\" -ForegroundColor Cyan
Write-Host "  - $NDK_PATH\toolchains\llvm\prebuilt\windows-x86_64\lib\" -ForegroundColor Cyan
Write-Host "  - $NDK_PATH\toolchains\llvm\prebuilt\windows-x86_64\include\" -ForegroundColor Cyan
Write-Host ""
Write-Host "Backups will be saved with suffix: $BACKUP_SUFFIX" -ForegroundColor Yellow
Write-Host ""

# Confirm
$confirm = Read-Host "Continue with installation? (y/N)"
if ($confirm -ne "y" -and $confirm -ne "Y") {
    Write-Host "Installation cancelled." -ForegroundColor Yellow
    exit 0
}

Write-Host ""
Write-Host "=== Starting Installation ===" -ForegroundColor Cyan

# Define paths
$PREBUILT_PATH = "$NDK_PATH\toolchains\llvm\prebuilt\windows-x86_64"
$BIN_PATH = "$PREBUILT_PATH\bin"
$LIB_PATH = "$PREBUILT_PATH\lib"
$INCLUDE_PATH = "$PREBUILT_PATH\include"

# Step 1: Backup existing toolchain
Write-Host ""
Write-Host "[1/4] Backing up stock NDK toolchain..." -ForegroundColor Yellow

if (Test-Path "$BIN_PATH\clang.exe") {
    Write-Host "  Backing up bin/..." -ForegroundColor Gray
    Copy-Item -Path $BIN_PATH -Destination "${BIN_PATH}${BACKUP_SUFFIX}" -Recurse -Force
    Write-Host "  ✓ Backup: ${BIN_PATH}${BACKUP_SUFFIX}" -ForegroundColor Green
} else {
    Write-Host "  Warning: bin/clang.exe not found, skipping backup" -ForegroundColor Yellow
}

if (Test-Path $LIB_PATH) {
    Write-Host "  Backing up lib/..." -ForegroundColor Gray
    Copy-Item -Path $LIB_PATH -Destination "${LIB_PATH}${BACKUP_SUFFIX}" -Recurse -Force
    Write-Host "  ✓ Backup: ${LIB_PATH}${BACKUP_SUFFIX}" -ForegroundColor Green
}

if (Test-Path $INCLUDE_PATH) {
    Write-Host "  Backing up include/..." -ForegroundColor Gray
    Copy-Item -Path $INCLUDE_PATH -Destination "${INCLUDE_PATH}${BACKUP_SUFFIX}" -Recurse -Force
    Write-Host "  ✓ Backup: ${INCLUDE_PATH}${BACKUP_SUFFIX}" -ForegroundColor Green
}

# Step 2: Copy OLLVM binaries
Write-Host ""
Write-Host "[2/4] Installing OLLVM clang/lld..." -ForegroundColor Yellow

if (Test-Path "$TOOLCHAIN_DIR\bin") {
    Write-Host "  Copying bin/ (clang.exe, lld.exe, etc.)..." -ForegroundColor Gray
    Copy-Item -Path "$TOOLCHAIN_DIR\bin\*" -Destination $BIN_PATH -Recurse -Force
    Write-Host "  ✓ Installed: $BIN_PATH" -ForegroundColor Green
} else {
    Write-Host "  ERROR: $TOOLCHAIN_DIR\bin not found!" -ForegroundColor Red
    exit 1
}

# Step 3: Copy OLLVM libraries
Write-Host ""
Write-Host "[3/4] Installing OLLVM libraries..." -ForegroundColor Yellow

if (Test-Path "$TOOLCHAIN_DIR\lib") {
    Write-Host "  Copying lib/ (LLVM libraries)..." -ForegroundColor Gray
    Copy-Item -Path "$TOOLCHAIN_DIR\lib\*" -Destination $LIB_PATH -Recurse -Force
    Write-Host "  ✓ Installed: $LIB_PATH" -ForegroundColor Green
} else {
    Write-Host "  Warning: $TOOLCHAIN_DIR\lib not found, skipping" -ForegroundColor Yellow
}

# Step 4: Copy OLLVM headers (optional, for plugin development)
Write-Host ""
Write-Host "[4/4] Installing OLLVM headers (optional)..." -ForegroundColor Yellow

if (Test-Path "$TOOLCHAIN_DIR\include") {
    Write-Host "  Copying include/ (LLVM headers)..." -ForegroundColor Gray
    Copy-Item -Path "$TOOLCHAIN_DIR\include\*" -Destination $INCLUDE_PATH -Recurse -Force
    Write-Host "  ✓ Installed: $INCLUDE_PATH" -ForegroundColor Green
} else {
    Write-Host "  Skipped: headers not included in toolchain" -ForegroundColor Gray
}

# Verify installation
Write-Host ""
Write-Host "=== Verifying Installation ===" -ForegroundColor Cyan

$clangPath = "$BIN_PATH\clang.exe"
if (Test-Path $clangPath) {
    Write-Host "Testing clang..." -ForegroundColor Gray
    $version = & $clangPath --version 2>&1 | Select-Object -First 1
    Write-Host "  ✓ $version" -ForegroundColor Green

    # Check for obfuscation passes (grep for "obfuscation" in help)
    Write-Host "Checking obfuscation flags..." -ForegroundColor Gray
    $helpOutput = & $clangPath -mllvm -help 2>&1 | Select-String -Pattern "fla|bcf|sub|vmp|expiry" -Quiet
    if ($helpOutput) {
        Write-Host "  ✓ Obfuscation flags available" -ForegroundColor Green
    } else {
        Write-Host "  ⚠ Warning: Could not verify obfuscation flags" -ForegroundColor Yellow
    }
} else {
    Write-Host "  ✗ ERROR: clang.exe not found at $clangPath" -ForegroundColor Red
    exit 1
}

# Success!
Write-Host ""
Write-Host "=== Installation Complete ===" -ForegroundColor Green
Write-Host ""
Write-Host "OLLVM17 toolchain installed successfully!" -ForegroundColor Green
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "  1. Build your project with obfuscation flags:" -ForegroundColor White
Write-Host "     LOCAL_CFLAGS += -mllvm -fla -mllvm -bcf -mllvm -sub" -ForegroundColor Cyan
Write-Host ""
Write-Host "  2. Test with examples:" -ForegroundColor White
Write-Host "     cd examples\basic" -ForegroundColor Cyan
Write-Host "     ndk-build" -ForegroundColor Cyan
Write-Host ""
Write-Host "  3. See README.md for complete flag reference (27 flags available)" -ForegroundColor White
Write-Host ""
Write-Host "Backup location: ${BIN_PATH}${BACKUP_SUFFIX}" -ForegroundColor Gray
Write-Host ""
Write-Host "To restore stock NDK:" -ForegroundColor Yellow
Write-Host "  1. Delete current bin/lib/include folders" -ForegroundColor Gray
Write-Host "  2. Rename *${BACKUP_SUFFIX} folders back to original names" -ForegroundColor Gray
Write-Host ""
