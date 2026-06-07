param(
    [string]$NdkRoot = $env:ANDROID_NDK_HOME,
    [string]$BuildDir = "build/android",
    [string]$AndroidPlatform = "android-26",
    [string]$EuiRoot = "",
    [string]$EuiAndroidPortRoot = "",
    [string]$CMakePath = "",
    [string]$NinjaPath = ""
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

function Resolve-Executable {
    param(
        [string]$ExplicitPath,
        [string]$EnvironmentPath,
        [string]$CommandName,
        [string]$ParameterName
    )

    if (![string]::IsNullOrWhiteSpace($ExplicitPath)) {
        if (Test-Path -LiteralPath $ExplicitPath) {
            return (Resolve-Path -LiteralPath $ExplicitPath).Path
        }
        throw "$CommandName was not found at $ExplicitPath. Pass a valid -$ParameterName."
    }

    if (![string]::IsNullOrWhiteSpace($EnvironmentPath) -and (Test-Path -LiteralPath $EnvironmentPath)) {
        return (Resolve-Path -LiteralPath $EnvironmentPath).Path
    }

    $command = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    throw "$CommandName was not found. Add it to PATH or pass -$ParameterName."
}

$CMakeExe = Resolve-Executable -ExplicitPath $CMakePath -EnvironmentPath $env:CMAKE_EXE -CommandName "cmake.exe" -ParameterName "CMakePath"
$NinjaExe = Resolve-Executable -ExplicitPath $NinjaPath -EnvironmentPath $env:NINJA_EXE -CommandName "ninja.exe" -ParameterName "NinjaPath"

if ([string]::IsNullOrWhiteSpace($NdkRoot)) {
    $candidates = @(
        "$env:LOCALAPPDATA\Android\Sdk\ndk",
        "$env:USERPROFILE\AppData\Local\Android\Sdk\ndk"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            $latest = Get-ChildItem -LiteralPath $candidate -Directory | Sort-Object Name -Descending | Select-Object -First 1
            if ($latest) {
                $NdkRoot = $latest.FullName
                break
            }
        }
    }
}

if ([string]::IsNullOrWhiteSpace($NdkRoot) -or !(Test-Path -LiteralPath "$NdkRoot\build\cmake\android.toolchain.cmake")) {
    throw "Android NDK was not found. Set ANDROID_NDK_HOME or pass -NdkRoot to an installed NDK."
}

if ([string]::IsNullOrWhiteSpace($EuiRoot)) {
    $repoLocalEui = Join-Path $ProjectRoot "third_party\EUI-NEO"
    $siblingEui = Join-Path (Split-Path -Parent $ProjectRoot) "EUI-NEO-0.4.0"
    if (Test-Path -LiteralPath "$repoLocalEui\include\eui_neo.h") {
        $EuiRoot = $repoLocalEui
    } elseif (Test-Path -LiteralPath "$siblingEui\include\eui_neo.h") {
        $EuiRoot = $siblingEui
    }
}

if ([string]::IsNullOrWhiteSpace($EuiAndroidPortRoot)) {
    $EuiAndroidPortRoot = Join-Path $ProjectRoot "third_party\eui-neo-android-elf-port-kit"
}

if ([string]::IsNullOrWhiteSpace($EuiRoot) -or !(Test-Path -LiteralPath "$EuiRoot\include\eui_neo.h")) {
    throw "EUI-NEO source was not found. Run git submodule update --init --recursive or pass -EuiRoot."
}

if (!(Test-Path -LiteralPath "$EuiAndroidPortRoot\core\platform\android\native_surface\ANativeWindowCreator.h")) {
    throw "Android ELF port kit was not found. Pass -EuiAndroidPortRoot or restore third_party\eui-neo-android-elf-port-kit."
}

$AbsBuildDir = Join-Path $ProjectRoot $BuildDir
New-Item -ItemType Directory -Force -Path $AbsBuildDir | Out-Null

& $CMakeExe `
    -S $ProjectRoot `
    -B $AbsBuildDir `
    -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$NinjaExe" `
    "-DCMAKE_TOOLCHAIN_FILE=$NdkRoot\build\cmake\android.toolchain.cmake" `
    "-DANDROID_ABI=arm64-v8a" `
    "-DANDROID_PLATFORM=$AndroidPlatform" `
    "-DANDROID_STL=c++_static" `
    "-DCMAKE_BUILD_TYPE=Release" `
    "-DEUI_ROOT=$EuiRoot" `
    "-DEUI_ANDROID_PORT_ROOT=$EuiAndroidPortRoot"

& $CMakeExe --build $AbsBuildDir --parallel
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "Built: $AbsBuildDir\neopanel_android"
