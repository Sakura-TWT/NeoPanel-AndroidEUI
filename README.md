# NeoPanel Android ELF

NeoPanel Android ELF is a standalone Android root executable that renders an EUI-style panel through Vulkan without an APK, Activity, or Android View hierarchy.

The project is an Android ELF port and composition layer built around EUI-NE rendering primitives and a small Android native backend. The upstream EUI project must be credited and obtained from:

https://github.com/sudoevolve/EUI-NE

## What This Builds

The build output is a single arm64 Android ELF:

```text
build/android/neopanel_android
```

It is intended to run from a root shell on Android:

```sh
chmod 755 /data/local/tmp/neopanel/neopanel_android
/data/local/tmp/neopanel/neopanel_android
```

## Repository Layout

```text
app/
  main.cpp                         NeoPanel UI, text, embedded assets, runtime loop
  android_window_backend_panel.cpp Android Surface/input window backend for fixed panel
  embedded_assets.h                Generated embedded asset symbol declarations
cmake/
  embed_binary.cmake               Converts PNG/TTF files to C++ byte arrays
  vulkan_portability_compat.h      Vulkan portability compatibility shim
docs/
  BUILD_AND_RUN.md                 Build, deploy, verify, troubleshoot
  PORTING.md                       Android ELF migration notes
  GITHUB_RELEASE_CHECKLIST.md      Pre-upload checklist
picture/
  avatar.png                       Embedded avatar source asset
  layout_reference.jpg             Layout reference
  ui_region_reference.jpg          Fixed region reference
third_party/
  eui-neo-android-elf-port-kit/    Android Surface/Vulkan/input port support
```

The upstream EUI source is tracked as a Git submodule at `third_party/EUI-NE`, so GitHub renders it as a linked upstream directory. The user-requested attribution URL is `https://github.com/sudoevolve/EUI-NE`; the currently cloneable upstream repository used by the submodule is `https://github.com/sudoevolve/EUI-NEO`.

## Dependencies

- Windows host with PowerShell.
- CMake and Ninja.
- Android NDK with arm64 support.
- Upstream EUI source submodule from `https://github.com/sudoevolve/EUI-NEO`.
- Rooted Android device with Vulkan support.

Known working local toolchain:

```text
Android NDK r23c
arm64-v8a
ANDROID_PLATFORM=android-26
ANDROID_STL=c++_static
```

## Build

Option 1: clone this repository with submodules:

```powershell
git clone --recurse-submodules <this-repository-url>
powershell -ExecutionPolicy Bypass -File .\build_android.ps1 -NdkRoot 'D:\VSTool\Shared\Android\AndroidNDK\android-ndk-r23c'
```

If the repository was cloned without submodules:

```powershell
git submodule update --init --recursive
```

Option 2: use an existing local upstream checkout:

```powershell
powershell -ExecutionPolicy Bypass -File .\build_android.ps1 `
  -NdkRoot 'D:\VSTool\Shared\Android\AndroidNDK\android-ndk-r23c' `
  -EuiRoot 'D:\AndroidEUI\EUI-NEO-0.4.0'
```

See [docs/BUILD_AND_RUN.md](docs/BUILD_AND_RUN.md) for deployment and verification commands.

## Porting Summary

The Android port replaces desktop windowing with:

- `SurfaceComposerClient` surface creation.
- `ANativeWindow` passed to Vulkan through `VK_KHR_android_surface`.
- `/dev/input/event*` touch input discovery.
- Touch drag converted into EUI scroll input.
- Static STL linking so no `libc++_shared.so` needs to be deployed.
- PNG and fallback font embedded into the ELF at build time.

The UI is drawn with EUI-NE primitive/render backend APIs rather than the full desktop app facade. See [docs/PORTING.md](docs/PORTING.md).

## Attribution

This project depends on and derives rendering behavior from EUI-NE:

https://github.com/sudoevolve/EUI-NE

The active Git submodule points to the accessible upstream repository:

https://github.com/sudoevolve/EUI-NEO

The bundled Android ELF Port Kit files document the Android native backend approach used here. Preserve upstream notices and licenses when redistributing.

## License

See [LICENSE](LICENSE) and [NOTICE](NOTICE). Upstream EUI-NE is Apache-2.0 in the local source used for this port; always verify the license from the upstream repository before publishing a release.
