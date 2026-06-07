# Android ELF 移植说明

NeoPanel 目前是 EUI-NEO 的 Android root ELF 移植实现。它验证的不是 APK 路线，而是一个直接从 root shell 启动的 ELF 路线。

运行链路：

```text
root shell
  -> neopanel_android
  -> SurfaceComposerClient
  -> ANativeWindow
  -> VK_KHR_android_surface
  -> EUI-NEO render primitives
```

没有 Activity、AndroidManifest、Java/Kotlin 入口和 Android View 树。

## 为什么不用桌面入口

EUI-NEO 的常规示例依赖桌面窗口后端。Android root ELF 环境没有 GLFW/SDL2 窗口，也没有 Android View 事件分发。

因此本仓库直接使用底层接口：

```text
core::RoundedRectPrimitive
core::PolygonPrimitive
core::render::RenderBackend
core::render::VulkanRenderBackend
core::input queue/consume APIs
```

这能先把 Surface、Vulkan、输入和资源生命周期收束到一个可验证的 Android ELF 目标里。

## Surface

设计坐标：

```text
Design screen:      1080 x 2400
Panel design rect:  x=64, y=872, width=960, height=640
Internal UI size:   960 x 640
```

Android 后端通过 SurfaceComposer 创建固定区域的 Surface。不同分辨率设备按比例缩放。Surface 只承载像素；圆角、阴影、遮罩和内容由 EUI primitive 绘制。

## Vulkan

Android 构建启用：

```text
VK_USE_PLATFORM_ANDROID_KHR=1
EUI_RENDER_BACKEND_VULKAN=1
EUI_WINDOW_BACKEND_ANDROID=1
```

链接库：

```text
android
log
vulkan
dl
```

构建脚本使用 `ANDROID_STL=c++_static`，部署时不需要额外放置 `libc++_shared.so`。

## 输入

程序不是 APK，所以没有 Android View touch callback。后端读取：

```text
/dev/input/event*
```

读取 ABS 坐标后映射到 960 x 640 面板坐标。点击、释放和拖动写入 EUI 输入队列，纵向拖动转换为 scroll input。这个路径通常需要 root 权限。

## 资源

构建阶段嵌入：

```text
picture/avatar.png -> embedded_avatar.cpp
EUI-NEO fallback font -> embedded_ui_font.cpp
```

运行时：

- `stb_image` 从内存解码头像。
- 渲染后端上传头像纹理。
- `stb_truetype` 栅格化文字 glyph。
- 优先使用 Android 系统 CJK 字体，失败后使用嵌入字体。

最终部署形态仍是一个 ELF。

## UI 样例

主界面在 `src/main.cpp` 中绘制。它用于验证移植链路，不是通用 Android UI 系统。

当前包含：

- 左侧头像和导航。
- 右侧内容面板。
- scroll scissor 和上下遮罩。
- DEMO / FRAME / EFFECT / SYS 四个页面。

内容区固定：

```text
contentX=258
contentY=42
contentW=654
contentH=540
scrollTop=contentY + 126
scrollBottom=contentY + contentH - 42
```

可滚动内容先在 scissor 内绘制，再绘制遮罩，最后重绘固定 header，避免滚动内容压到标题区域。

## 当前边界

- 触摸输入依赖 root 读取 `/dev/input`。
- 面板是固定设计尺寸。
- EFFECT 页的 3D 视觉是 primitive 透视投影，不是独立 3D mesh 管线。
- 没有系统输入法接入。
- 没有 APK 生命周期管理。
