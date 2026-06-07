# EUI-NEO Android ELF Port Kit

这是一个 **EUI-NEO Android ELF 移植代码包**。

它不是完整工程，而是保存将 EUI-NEO 移植到 Android ELF 所需的关键代码和说明，方便后续在 Windows 或其他开发环境中继续维护、复用、移植。

---

## 目标

把原本桌面端的 EUI-NEO：

```text
GLFW / SDL2
+ OpenGL / Vulkan
```

扩展为 Android root 环境下可直接运行的：

```text
Android ELF 可执行文件
+ SurfaceComposerClient 创建 Surface
+ ANativeWindow
+ Vulkan Android Surface
+ /dev/input 触摸输入
```

这个方案不是 APK，不依赖 Activity，不使用 Android View 系统。

---

## 移植核心结构

需要在原 EUI-NEO 项目中增加或修改以下部分。

```text
core/
├── app/
│   └── android_elf_app_main.cpp
├── window/
│   └── android_window_backend.cpp
├── platform/
│   └── android/
│       └── native_surface/
│           └── ANativeWindowCreator.h
├── render/
│   ├── render_backend.cpp
│   └── vulkan/
│       └── vulkan_backend.cpp
└── input/
    ├── input_state.h
    └── input_types.h

cmake/
└── android-arm64-hostclang.toolchain.cmake

examples/
└── gallery_android_mobile.cpp
```

---

## 1. Android ELF 主入口

文件：

```text
core/app/android_elf_app_main.cpp
```

作用：

- 作为 Android ELF 可执行程序入口；
- 创建 Android window backend；
- 初始化 Vulkan render backend；
- 初始化 EUI app；
- 执行主循环；
- 每帧调用 EUI runtime；
- 使用 `ANativeWindow_getWidth/Height()` 获取小窗实际渲染尺寸。

这个文件替代桌面端的 SDL2/GLFW main loop。

---

## 2. Android Window Backend

文件：

```text
core/window/android_window_backend.cpp
```

作用：

- 实现 EUI 的 `core::window` 抽象接口；
- 创建 Android Surface；
- 返回 `ANativeWindow*` 给 Vulkan；
- 自动读取屏幕分辨率；
- 创建居中悬浮小窗；
- 设置窗口位置；
- 窗口外形由 Android Surface 承载，UI 圆角推荐交给 EUI 绘制层完成；
- 处理触摸点击；
- 将触摸滑动转换成滚动输入。

当前关键能力：

```cpp
android::ANativeWindowCreator::Create(...)
android::ANativeWindowCreator::SetWindowPosition(...)
android::ANativeWindowCreator::Destroy(...)
```

触摸输入来自：

```text
/dev/input/event*
```

主要读取：

```cpp
ABS_MT_POSITION_X
ABS_MT_POSITION_Y
ABS_X
ABS_Y
ABS_MT_TRACKING_ID
BTN_TOUCH
SYN_REPORT
```

滑动滚动核心逻辑：

```cpp
const double dy = localY - window->lastTouchY;
scrollY = dy / 28.0;
core::queueScrollInput(static_cast<Handle>(window), 0.0, scrollY);
```

如需调整滑动灵敏度：

```cpp
// 更灵敏
scrollY = dy / 20.0;

// 当前
scrollY = dy / 28.0;

// 更慢
scrollY = dy / 40.0;
```

---

## 3. Android Surface 创建器

文件：

```text
core/platform/android/native_surface/ANativeWindowCreator.h
```

作用：

- 动态解析 Android 系统库；
- 通过 SurfaceComposerClient 创建 Surface；
- 从 SurfaceControl 获取 ANativeWindow；
- 控制 layer、position、show；
- 保留可选系统 Surface 圆角符号解析作为历史实验，不建议作为 UI 圆角主方案。

核心系统库：

```text
libgui.so
libutils.so
```

核心 Android 类/机制：

```text
SurfaceComposerClient
SurfaceControl
SurfaceComposerClient::Transaction
ANativeWindow
```

当前支持的关键 Transaction 操作：

```cpp
setLayer
setPosition
show
apply
setCornerRadius
setClientDrawnCornerRadius
```

圆角相关：

```cpp
Transaction::setCornerRadius(...)
Transaction::setClientDrawnCornerRadius(...)
```

这些系统 Surface 圆角接口只应视为可选实验能力。EUI-NEO 源码本身已经提供圆角绘制能力，实际 UI 小窗、卡片、图片和组件圆角应优先在 DSL / 组件层通过绘制接口完成，例如：

```cpp
ui.rect("page.shell")
    .size(width, height)
    .color(surface())
    .radius(42.0f)
    .build();

ui.image("about.logo.image")
    .size(logoSize, logoSize)
    .source("assets/icon.png")
    .radius(28.0f)
    .cover()
    .build();
```

注意：

- 这些是 Android 私有 C++ 符号；
- 不同 Android 版本符号可能变化；
- 因此系统 Surface 圆角只能作为可选解析；
- 解析失败不会阻止程序运行；
- 推荐使用 EUI 的 `radius()` 绘制圆角，避免把视觉效果绑定到不同设备的 SurfaceComposer 行为。

---

## 4. Vulkan Android Surface

文件：

```text
core/render/vulkan/vulkan_backend.cpp
```

Android 下需要启用：

```cpp
#include <vulkan/vulkan_android.h>
```

Vulkan instance extension：

```cpp
VK_KHR_surface
VK_KHR_android_surface
```

创建 surface：

```cpp
VkAndroidSurfaceCreateInfoKHR createInfo{};
createInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
createInfo.window = nativeWindow;

vkCreateAndroidSurfaceKHR(instance, &createInfo, nullptr, &surface);
```

其中 `nativeWindow` 来自：

```cpp
core::window::nativeWindowInfo(window).platformWindow
```

---

## 5. Render Backend Android 分支

文件：

```text
core/render/render_backend.cpp
```

Android 下需要避免桌面 Vulkan loader 初始化，例如：

```cpp
glfwInitVulkanLoader(...)
```

Android Vulkan 直接使用系统 Vulkan loader 和 `vkGetInstanceProcAddr`。

---

## 6. Input State 修改

文件：

```text
core/input/input_state.h
core/input/input_types.h
```

重点：

1. Android 下不能误走 GLFW 分支；
2. 需要保留 scroll event 队列；
3. Android 触摸滑动通过 `queueScrollInput()` 注入。

关键接口：

```cpp
queueScrollInput(window, x, y)
consumeInputEvents(window)
readPointerEvent(window)
```

Android 条件判断应排除 GLFW：

```cpp
#if !defined(EUI_WINDOW_BACKEND_SDL2) && !defined(EUI_WINDOW_BACKEND_ANDROID)
    // GLFW-only input callback code
#endif
```

---

## 7. CMake 移植要点

需要让 EUI 支持新的 window backend：

```text
EUI_WINDOW_BACKEND=android
```

需要新增 Android 后端源文件：

```text
core/window/android_window_backend.cpp
core/app/android_elf_app_main.cpp
```

Android + Vulkan 链接库：

```text
vulkan
android
log
```

Android 下跳过桌面依赖：

```text
GLFW
SDL2
glad
OpenGL::GL
GTK / AppIndicator
```

Android 编译定义：

```cpp
EUI_WINDOW_BACKEND_ANDROID=1
EUI_RENDER_BACKEND_VULKAN=1
```

---

## 8. Toolchain

文件：

```text
cmake/android-arm64-hostclang.toolchain.cmake
```

用途：

- 用 host clang 交叉生成 Android/Bionic ELF；
- 使用 Android NDK sysroot；
- 适用于 host clang 可用，但 NDK 自带 clang 不适合当前 host 的情况。

在 Windows 开发时，你可以选择：

### 方案 A：使用标准 NDK CMake toolchain

如果你在 Windows 上安装了 Android NDK，优先使用官方工具链：

```text
android.toolchain.cmake
```

### 方案 B：参考本包中的自定义 toolchain

如果你需要在特殊 Linux/ARM64 环境下交叉编译，可以参考：

```text
android-arm64-hostclang.toolchain.cmake
```

---

## 9. Android Mobile Gallery 示例

文件：

```text
examples/gallery_android_mobile.cpp
```

这是从原 `gallery.cpp` 改出的 Android 移动端示例，包含：

- 中文化；
- 更紧凑侧栏；
- 更小窗口布局；
- 隐藏滚动条；
- 手机式滑动滚动；
- 页面切换淡入/滑入动画；
- 使用 EUI 绘制层增强内部和外层视觉圆角。

它主要用于参考 Android 小窗布局如何改，不是移植的必需文件。

---

## 10. 当前方案限制

这个 Android ELF 方案有一些天然限制：

1. 需要 root 或足够权限访问系统接口；
2. 使用 Android 私有 C++ 符号，系统版本变化可能需要适配；
3. 没有 Activity；
4. 没有标准 Android View；
5. 没有系统输入法接入，文本输入需要额外实现；
6. 系统 Surface 圆角不应作为主要 UI 方案；圆角应优先使用 EUI 绘制层 `radius()`，系统合成器行为只作为可选实验；
7. `/dev/input` 触摸坐标方向在部分设备上可能需要旋转/反转修正。

---

## 11. 推荐后续开发方式

如果你在 Windows 上继续开发，建议：

1. 保留完整 EUI-NEO 项目；
2. 将本包里的 Android 移植文件合入对应目录；
3. 使用 Android NDK 编译 Android ELF；
4. 将输出 ELF、assets、libc++_shared.so 部署到 Android 设备；
5. 通过 root shell 运行 ELF。

本包的价值是保存 Android 移植所需的核心代码，而不是替代完整 EUI 工程。

---

## 12. 最重要的移植文件

如果只看最关键文件，优先看：

```text
core/window/android_window_backend.cpp
core/platform/android/native_surface/ANativeWindowCreator.h
core/render/vulkan/vulkan_backend.cpp
core/app/android_elf_app_main.cpp
core/input/input_state.h
```

其中：

- `android_window_backend.cpp` 负责窗口、触摸、滑动；
- `ANativeWindowCreator.h` 负责 Android Surface；
- `vulkan_backend.cpp` 负责 Android Vulkan surface；
- `android_elf_app_main.cpp` 负责 ELF 主循环；
- `input_state.h` 负责输入队列和 scroll event。
