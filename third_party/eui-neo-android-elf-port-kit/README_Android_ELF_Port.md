# EUI-NEO Android ELF Port Kit

这个目录保存 EUI-NEO 迁移到 Android root ELF 所需的代码和记录。

这个目录不能单独构建。里面的文件用于记录移植点，后续可以合入 EUI-NEO，或继续作为 Android ELF 后端的参考代码。

## 包含内容

```text
core/app/android_elf_app_main.cpp
core/window/android_window_backend.cpp
core/platform/android/native_surface/ANativeWindowCreator.h
core/render/render_backend.cpp
core/render/vulkan/vulkan_backend.cpp
core/input/input_state.h
core/input/input_types.h
examples/gallery_android_mobile.cpp
cmake/android-arm64-hostclang.toolchain.cmake
build_notes/
```

## 运行路径

目标运行路径：

```text
Android root shell
  -> ELF main loop
  -> SurfaceComposerClient
  -> ANativeWindow
  -> Vulkan Android surface
  -> EUI-NEO rendering
  -> /dev/input touch events
```

这条路径不使用 APK、Activity 或 Android View。

## 关键文件

### `core/app/android_elf_app_main.cpp`

Android ELF 的主循环入口。它负责创建 window backend、初始化 Vulkan backend、调用 EUI runtime，并在每帧提交渲染。

### `core/window/android_window_backend.cpp`

实现 EUI 的 window 抽象。它创建 Android Surface，返回 `ANativeWindow*` 给 Vulkan，并从 `/dev/input/event*` 读取触摸。

拖动转换为滚动输入的核心接口是：

```cpp
core::queueScrollInput(window, 0.0, scrollY);
```

### `core/platform/android/native_surface/ANativeWindowCreator.h`

封装 SurfaceComposer 相关调用。它动态解析 Android 系统符号，创建 SurfaceControl，并取得 `ANativeWindow`。

这里使用的是 Android 私有 C++ 符号。不同系统版本可能需要适配。

### `core/render/vulkan/vulkan_backend.cpp`

Android 分支使用 `VK_KHR_android_surface`：

```cpp
VkAndroidSurfaceCreateInfoKHR createInfo{};
createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
createInfo.window = nativeWindow;
```

### `core/render/render_backend.cpp`

Android 不走桌面 Vulkan loader 初始化。这里保留 Android 分支，避免误调用 GLFW 相关 loader。

### `core/input/input_state.h`

保留 pointer 和 scroll event 队列。Android 后端通过队列把 `/dev/input` 的结果交给 EUI。

## CMake 要点

Android 后端需要：

```text
EUI_WINDOW_BACKEND_ANDROID=1
EUI_RENDER_BACKEND_VULKAN=1
VK_USE_PLATFORM_ANDROID_KHR=1
```

需要链接：

```text
android
log
vulkan
dl
```

不需要桌面依赖：

```text
GLFW
SDL2
OpenGL::GL
glad
GTK
AppIndicator
```

更具体的 CMake 迁移记录在 `build_notes/CMakeLists.android_port_notes.txt`。

## 示例

`examples/gallery_android_mobile.cpp` 是移动端布局参考。它用于观察小窗布局、滚动和页面切换，不是最小移植所必需的文件。

## 限制

- 需要 root 或等效权限读取 `/dev/input`。
- SurfaceComposer 私有符号可能随 Android 版本变化。
- 没有 Activity 生命周期。
- 没有 Android View 和系统输入法。
- Surface 圆角不应作为主要 UI 方案；圆角应由 EUI 绘制层处理。
