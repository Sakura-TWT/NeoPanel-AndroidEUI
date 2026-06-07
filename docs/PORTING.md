# Android ELF 移植说明

NeoPanel 目前更准确的定位，是 EUI-NEO 在 Android root ELF 场景下的一套移植实现。它验证的是：不做 APK、不走 Activity，也能让 EUI 的 primitive 渲染、Vulkan 后端和输入路径在 Android ELF 可执行文件中跑起来。上游项目通过子模块引用：

```text
third_party/EUI-NEO -> https://github.com/sudoevolve/EUI-NEO
```

## 运行模型

当前运行链路如下：

```text
Android root shell
  -> neopanel_android
  -> SurfaceComposerClient creates Surface
  -> ANativeWindow
  -> Vulkan Android surface
  -> EUI render primitives
```

没有 Activity、AndroidManifest、Java/Kotlin 入口和 Android View 树。这个移植层自己负责窗口、渲染和输入。

## 为什么直接用底层 primitive

Android root ELF 环境和桌面示例不同：没有 GLFW/SDL2 窗口，也没有系统事件分发。为了降低移植风险，NeoPanel 没有套完整桌面 app facade，而是直接使用 EUI-NEO 的底层绘制与渲染后端：

```text
core::RoundedRectPrimitive
core::PolygonPrimitive
core::render::RenderBackend
core::render::VulkanRenderBackend
core::input queue/consume APIs
```

这样可以把 Surface、Vulkan、输入和资源生命周期收束在 Android ELF 移植层里。

## Surface 与坐标

```text
Design screen:      1080 x 2400
Panel design rect:  x=64, y=872, width=960, height=640
Internal UI size:   960 x 640
```

Android 后端用 SurfaceComposer 创建一个固定面板区域。不同分辨率设备会按比例缩放这个矩形。Surface 只负责承载像素，圆角外壳、边框、阴影、遮罩和内容全部由 EUI primitive 绘制。

## Vulkan

构建时启用：

```text
VK_USE_PLATFORM_ANDROID_KHR=1
EUI_RENDER_BACKEND_VULKAN=1
EUI_WINDOW_BACKEND_ANDROID=1
```

链接 Android 系统库：

```text
android
log
vulkan
dl
```

构建脚本使用 `ANDROID_STL=c++_static`，所以不需要额外部署 `libc++_shared.so`。

## Input

程序不是 APK，不会收到 Android View 的触摸回调。后端扫描：

```text
/dev/input/event*
```

读取 ABS 坐标后映射到 960 x 640 面板坐标。点击、释放和拖动状态会写入 EUI 输入队列，纵向拖动转换为滚动输入。这个路径通常需要 root 权限。

## 资源嵌入

构建阶段将资源转成 C++ 字节数组：

```text
picture/avatar.png -> embedded_avatar.cpp
EUI-NEO fallback font -> embedded_ui_font.cpp
```

运行时：

- `stb_image` decodes the embedded PNG from memory.
- 渲染后端把头像上传为纹理。
- `stb_truetype` 将文字 glyph 栅格化为小纹理。
- 优先使用 Android 系统 CJK 字体，失败后使用嵌入字体。

这样部署时只需要一个 ELF。

## UI 组织

主界面在 `src/main.cpp` 中绘制：

- 外壳和左侧导航。
- 头像 cover crop。
- 右侧内容面板。
- scroll scissor 和上下遮罩。
- DEMO / FRAME / EFFECT / SYS 四个内容页。

内容区固定：

```text
contentX=258
contentY=42
contentW=654
contentH=540
scrollTop=contentY + 126
scrollBottom=contentY + contentH - 42
```

可滚动内容在 scissor 内绘制，然后绘制遮罩，最后重绘固定 header，避免滚动内容压到标题区域。

## 当前边界

- 触摸输入依赖 root 读取 `/dev/input`。
- 面板是固定设计尺寸，不是通用 Android 自适应布局系统。
- EFFECT 页的 3D 视觉是 primitive 的透视投影，不是独立 3D mesh 管线。
