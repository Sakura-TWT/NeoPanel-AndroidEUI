# GitHub Release Checklist

Use this before pushing a public update.

## Source

- Confirm `app/main.cpp` builds.
- Confirm `app/android_window_backend_panel.cpp` is included.
- Confirm `third_party/eui-neo-android-elf-port-kit` is present.
- Confirm upstream EUI-NE is credited with this exact link:

```text
https://github.com/sudoevolve/EUI-NE
```

## Build

```powershell
powershell -ExecutionPolicy Bypass -File .\build_android.ps1 `
  -NdkRoot 'D:\VSTool\Shared\Android\AndroidNDK\android-ndk-r23c' `
  -EuiRoot 'D:\AndroidEUI\EUI-NEO-0.4.0'
```

## Verify

```powershell
.\scripts\verify_elf.ps1 -NdkRoot 'D:\VSTool\Shared\Android\AndroidNDK\android-ndk-r23c'
```

Expected:

```text
Class:   ELF64
Type:    DYN
Machine: AArch64
```

## Repository Cleanliness

- Do not commit `build/`.
- Do not commit device logs.
- Do not commit local IDE files.
- Do not commit a private or modified upstream EUI-NE checkout unless that is intentional and licenses were checked.

## Documentation

- Update `README.md` if build or layout changes.
- Update `docs/PORTING.md` if the Android backend, Vulkan path, input model, or asset embedding changes.
- Update `docs/BUILD_AND_RUN.md` if commands change.
- Update project memory after every task.

## Push

```powershell
git status
git add .
git commit -m "Update NeoPanel Android ELF"
git push
```

