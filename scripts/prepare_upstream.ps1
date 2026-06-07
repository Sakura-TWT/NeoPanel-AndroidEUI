param(
    [string]$TargetDir = "third_party/EUI-NE"
)

$ErrorActionPreference = "Stop"

if (Test-Path -LiteralPath $TargetDir) {
    Write-Host "Upstream EUI-NE already exists at $TargetDir"
    exit 0
}

git clone https://github.com/sudoevolve/EUI-NE $TargetDir

