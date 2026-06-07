# Android ELF Porting Notes

This document explains how NeoPanel was moved from a desktop-style EUI rendering stack into a standalone Android ELF executable.

The original project that must be credited is EUI-NE:

```text
https://github.com/sudoevolve/EUI-NE
```

The active Git submodule uses the currently cloneable upstream repository:

```text
https://github.com/sudoevolve/EUI-NEO
```

## Goal

The target is not an APK. It is a root-launched Android ELF:

```text
Android root shell
  -> neopanel_android
  -> SurfaceComposerClient creates Surface
  -> ANativeWindow
  -> Vulkan Android surface
  -> EUI render primitives
```

There is no Activity, Java/Kotlin entry point, Android View tree, XML layout, or APK packaging.

## Why A Low-Level Integration

The original EUI desktop examples assume a normal app facade and desktop windowing. The Android root ELF path has different constraints:

- The executable must own the native window lifecycle.
- Vulkan must render into an Android `ANativeWindow`.
- Input must be read from Linux input devices.
- Runtime assets should not require an external app directory.
- The final artifact should stay deployable as one executable.

For that reason, NeoPanel uses EUI low-level primitives and render backend calls directly:

```text
core::RoundedRectPrimitive
core::PolygonPrimitive
core::render::RenderBackend
core::render::VulkanRenderBackend
core::input queue/consume APIs
```

## Window And Surface

The Android backend creates a fixed panel surface using SurfaceComposer APIs from the port kit. The intended design coordinate system is:

```text
Design screen:      1080 x 2400
Panel design rect:  x=64, y=872, width=960, height=640
Internal UI size:   960 x 640
```

On devices with different physical sizes, the backend scales the panel rect proportionally.

The Android Surface does not provide the visual frame. It is only the rendering target. The shell, rounded corners, masks, borders, and glass effect are drawn by EUI primitives.

## Vulkan

The port uses `VK_KHR_android_surface` and links:

```text
android
log
vulkan
dl
m
c
```

The CMake build defines:

```text
VK_USE_PLATFORM_ANDROID_KHR=1
EUI_RENDER_BACKEND_VULKAN=1
EUI_WINDOW_BACKEND_ANDROID=1
```

Static STL is selected with:

```text
ANDROID_STL=c++_static
```

This avoids deploying `libc++_shared.so` next to the ELF.

## Input

Because the executable is not an APK, it does not receive Android View touch callbacks. The backend discovers touch devices under:

```text
/dev/input/event*
```

It reads ABS coordinates, maps them into the fixed panel coordinate system, tracks press/release state, and converts vertical drag movement into scroll input through the EUI input queue.

This is why root access is normally required.

## Assets

The executable embeds its resources during CMake configure/build:

```text
picture/avatar.png
EUI-NE asset font fallback
```

`cmake/embed_binary.cmake` converts binary files into generated C++ byte arrays:

```text
embedded_avatar.cpp
embedded_ui_font.cpp
```

At runtime:

- `stb_image` decodes the embedded PNG from memory.
- The render backend uploads it as a texture.
- `stb_truetype` rasterizes text glyphs into small textures.
- Android system CJK fonts are preferred before the embedded fallback font.

This keeps the runtime deploy shape to one executable.

## UI Rendering

`app/main.cpp` draws the full panel using primitive operations:

- outer shell and side rail
- avatar cover crop
- navigation list
- content frame
- scroll masks and scissor clipping
- typography and glyph textures
- metrics, graphs, controls, material studies, and motion preview

The content area is fixed:

```text
contentX=258
contentY=42
contentW=654
contentH=540
scrollTop=contentY + 126
scrollBottom=contentY + contentH - 42
```

Scrollable content is rendered under backend scissor clipping. The fixed header is redrawn after scroll masks so scrolled content cannot visually intrude into the header.

## Current Pages

```text
DEMO    material/control study
FRAME   isolated performance telemetry and FPS selector
EFFECT  perspective card motion stage
SYS     Android/Vulkan/static STL/deployment notes
```

Performance telemetry is intentionally isolated to `FRAME`; other pages are designed to avoid becoming repeated dashboard cards.

## Build Integration

The project CMake file combines:

- NeoPanel app files.
- Android ELF Port Kit render/window/input support.
- Required EUI-NE render primitives and Vulkan files.
- Generated embedded binary files.

The upstream EUI source is supplied by submodule:

```text
third_party/EUI-NE
```

or by overriding with a local checkout:

```powershell
-EuiRoot 'path\to\EUI-NE'
```

## Limitations

- The executable assumes root-level access for `/dev/input`.
- The UI is a custom fixed-size panel, not a general Android layout system.
- The 3D card stage is perspective-projected 2D primitive rendering, not a separate Vulkan 3D mesh pipeline.
- The upstream EUI-NE source remains an external dependency unless the repository owner intentionally vendors it.
