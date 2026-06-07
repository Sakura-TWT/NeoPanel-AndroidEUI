# Vulkan 后端

相关文件：

```text
src/eui_android_elf/core/render/render_backend.cpp
src/eui_android_elf/core/render/vulkan/vulkan_backend.cpp
cmake/vulkan_portability_compat.h
```

## Android surface

Android 分支使用：

```text
VK_KHR_android_surface
```

核心创建结构：

```cpp
VkAndroidSurfaceCreateInfoKHR createInfo{};
createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
createInfo.window = nativeWindow;
```

`nativeWindow` 来自 EUI window backend 返回的 `ANativeWindow*`。

## Loader 分支

桌面 Vulkan 示例可能会初始化 GLFW Vulkan loader。Android 不需要这一步，`render_backend.cpp` 在 Android 后端下跳过桌面 loader。

## 链接库

Android ELF 目标链接：

```text
android
log
vulkan
dl
```

构建使用 `ANDROID_STL=c++_static`，所以部署时不需要额外放置 `libc++_shared.so`。
