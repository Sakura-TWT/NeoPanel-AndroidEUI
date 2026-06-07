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
$VsCMake = if ([string]::IsNullOrWhiteSpace($CMakePath)) { "D:\VSTool\community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" } else { $CMakePath }
$VsNinja = if ([string]::IsNullOrWhiteSpace($NinjaPath)) { "D:\VSTool\community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" } else { $NinjaPath }

if (!(Test-Path -LiteralPath $VsCMake)) {
    throw "cmake.exe was not found at $VsCMake"
}
if (!(Test-Path -LiteralPath $VsNinja)) {
    throw "ninja.exe was not found at $VsNinja"
}

if ([string]::IsNullOrWhiteSpace($NdkRoot)) {
    $candidates = @(
        "$env:LOCALAPPDATA\Android\Sdk\ndk",
        "$env:USERPROFILE\AppData\Local\Android\Sdk\ndk",
        "D:\Android\Sdk\ndk",
        "C:\Android\Sdk\ndk",
        "D:\VSTool\Shared\Android\AndroidNDK"
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
    $repoLocalEui = Join-Path $ProjectRoot "third_party\EUI-NE"
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
    throw "EUI-NE source was not found. Clone https://github.com/sudoevolve/EUI-NE to third_party\EUI-NE or pass -EuiRoot."
}

if (!(Test-Path -LiteralPath "$EuiAndroidPortRoot\core\platform\android\native_surface\ANativeWindowCreator.h")) {
    throw "Android ELF port kit was not found. Pass -EuiAndroidPortRoot or restore third_party\eui-neo-android-elf-port-kit."
}

$AbsBuildDir = Join-Path $ProjectRoot $BuildDir
New-Item -ItemType Directory -Force -Path $AbsBuildDir | Out-Null

& $VsCMake `
    -S $ProjectRoot `
    -B $AbsBuildDir `
    -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$VsNinja" `
    "-DCMAKE_TOOLCHAIN_FILE=$NdkRoot\build\cmake\android.toolchain.cmake" `
    "-DANDROID_ABI=arm64-v8a" `
    "-DANDROID_PLATFORM=$AndroidPlatform" `
    "-DANDROID_STL=c++_static" `
    "-DCMAKE_BUILD_TYPE=Release" `
    "-DEUI_ROOT=$EuiRoot" `
    "-DEUI_ANDROID_PORT_ROOT=$EuiAndroidPortRoot"

& $VsCMake --build $AbsBuildDir --parallel
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "Built: $AbsBuildDir\neopanel_android"
