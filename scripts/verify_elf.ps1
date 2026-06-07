param(
    [string]$NdkRoot = $env:ANDROID_NDK_HOME,
    [string]$ElfPath = "build/android/neopanel_android"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($NdkRoot)) {
    throw "Set ANDROID_NDK_HOME or pass -NdkRoot."
}

$ReadElf = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe"
if (!(Test-Path -LiteralPath $ReadElf)) {
    throw "llvm-readelf.exe was not found at $ReadElf"
}

if (!(Test-Path -LiteralPath $ElfPath)) {
    throw "ELF was not found at $ElfPath"
}

& $ReadElf -h $ElfPath | Select-String -Pattern 'Class:|Machine:|Type:'

$OutputDir = Split-Path -Parent $ElfPath
$extra = Get-ChildItem -Force $OutputDir | Where-Object { $_.Name -like '*c++*' -or $_.Name -eq 'picture' }
if ($extra) {
    Write-Warning "Unexpected runtime deployment files found:"
    $extra | Select-Object Name,FullName
} else {
    Write-Host "No picture directory or libc++_shared.so found next to the ELF."
}

