# Third Party Code

This repository includes the small Android ELF Port Kit used by NeoPanel:

```text
third_party/eui-neo-android-elf-port-kit
```

It contains the Android native Surface, Vulkan, input, and example files needed for the root ELF port.

The full upstream EUI project is linked as a Git submodule at:

```text
third_party/EUI-NE
```

The user-requested attribution URL is:

```text
https://github.com/sudoevolve/EUI-NE
```

The currently cloneable submodule remote is:

```text
https://github.com/sudoevolve/EUI-NEO
```

Initialize it with:

```powershell
git submodule update --init --recursive
```

You can also pass a local path explicitly:

```powershell
powershell -ExecutionPolicy Bypass -File .\build_android.ps1 -EuiRoot 'D:\path\to\EUI-NE'
```

Before publishing a release that vendors upstream source, verify the upstream license and preserve all required notices.
