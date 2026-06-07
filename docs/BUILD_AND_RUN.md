# Build And Run

This guide builds the `neopanel_android` arm64 Android ELF and runs it from a root shell.

## 1. Prepare Upstream EUI-NE

Use the upstream EUI submodule. The user-requested attribution URL is:

```text
https://github.com/sudoevolve/EUI-NE
```

The cloneable upstream repository currently used by the submodule is:

```text
https://github.com/sudoevolve/EUI-NEO
```

When cloning this repository:

```powershell
git clone --recurse-submodules <this-repository-url>
```

If already cloned:

```powershell
git submodule update --init --recursive
```

If you already have a local copy, pass it with `-EuiRoot`.

## 2. Build On Windows

Known working command:

```powershell
powershell -ExecutionPolicy Bypass -File .\build_android.ps1 `
  -NdkRoot 'D:\VSTool\Shared\Android\AndroidNDK\android-ndk-r23c' `
  -EuiRoot 'D:\AndroidEUI\EUI-NEO-0.4.0'
```

The script configures:

```text
ANDROID_ABI=arm64-v8a
ANDROID_PLATFORM=android-26
ANDROID_STL=c++_static
CMAKE_BUILD_TYPE=Release
```

Expected output:

```text
build/android/neopanel_android
```

## 3. Verify ELF

```powershell
& 'D:\VSTool\Shared\Android\AndroidNDK\android-ndk-r23c\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe' -h build\android\neopanel_android |
  Select-String -Pattern 'Class:|Machine:|Type:'
```

Expected:

```text
Class:   ELF64
Type:    DYN
Machine: AArch64
```

Check that no runtime asset folder or shared C++ STL is required:

```powershell
Get-ChildItem -Force build\android | Where-Object { $_.Name -like '*c++*' -or $_.Name -eq 'picture' }
```

Expected output: no rows.

## 4. Deploy To Android

Example with `adb`:

```sh
adb push build/android/neopanel_android /data/local/tmp/neopanel/neopanel_android
adb push deploy_example.sh /data/local/tmp/neopanel/deploy_example.sh
adb shell su -c "chmod 755 /data/local/tmp/neopanel/neopanel_android /data/local/tmp/neopanel/deploy_example.sh"
adb shell su -c "/data/local/tmp/neopanel/deploy_example.sh"
```

The executable reads touch input from `/dev/input/event*`, so it normally needs root.

## 5. Runtime Notes

- The Android Surface is the carrier; rounded shell, glow, masks, and panel content are drawn by EUI primitives.
- The fixed target screenshot layout is 1080 x 2400 with a 960 x 640 panel at x=64, y=872.
- If a device has a different resolution, the Android backend scales the fixed rect proportionally.
- The runtime tries Android system CJK fonts first and falls back to the embedded UI font.
- The avatar PNG and fallback font are embedded into the ELF by CMake.

## Troubleshooting

`EUI-NE source was not found`:

- Run `git submodule update --init --recursive`, or pass `-EuiRoot`.

`Android NDK was not found`:

- Set `ANDROID_NDK_HOME`, or pass `-NdkRoot`.

No touch response:

- Confirm the executable is running as root.
- Confirm `/dev/input/event*` exists and is readable.

Blank or failed Vulkan surface:

- Confirm the device supports Vulkan.
- Confirm `libvulkan.so` exists on device.
- Check the log file written by `deploy_example.sh`.
