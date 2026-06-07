# Android ELF 移植说明

本文说明 NeoPanel 如何从桌面式 EUI 渲染路径移植为 Android root ELF 可执行程序。

必须引用的原项目：

```text
https://github.com/sudoevolve/EUI-NE
```

当前 Git submodule 实际使用的可克隆上游仓库：

```text
https://github.com/sudoevolve/EUI-NEO
```

## 目标

目标不是 APK，而是可以从 root shell 直接启动的 Android ELF：

```text
root shell
  -> neopanel_android
  -> SurfaceComposerClient 创建 Surface
  -> ANativeWindow
  -> Vulkan Android surface
  -> EUI primitive 渲染
```

因此没有 Activity、AndroidManifest、Java/Kotlin 入口、XML 布局或 Android View 树。

## 为什么不用完整桌面 App Facade

桌面 EUI 示例通常依赖 GLFW/SDL2 和普通窗口系统。Android root ELF 环境不同：

- 需要自己管理 native window 生命周期。
- Vulkan 必须渲染到 `ANativeWindow`。
- 触摸输入不来自 View 回调，而来自 Linux input 设备。
- 资源最好在编译期嵌入，减少运行时部署文件。
- 最终形态最好是单个 ELF。

所以 NeoPanel 直接使用 EUI-NE 的底层渲染能力：

```text
core::RoundedRectPrimitive
core::PolygonPrimitive
core::render::RenderBackend
core::render::VulkanRenderBackend
core::input
```

## Surface 与窗口

Android 后端创建一个固定面板区域。设计坐标：

```text
设计屏幕:       1080 x 2400
面板区域:       x=64, y=872, width=960, height=640
内部 UI 尺寸:   960 x 640
```

如果真实设备分辨率不同，后端按比例缩放这个区域。

Android Surface 只是承载层。真正的圆角外壳、阴影、边框、遮罩和内容都由 EUI primitive 绘制。

## Vulkan 路径

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

使用静态 STL：

```text
ANDROID_STL=c++_static
```

这样运行时不需要额外推送 `libc++_shared.so`。

## 输入路径

因为不是 APK，程序不会收到 Android View 的触摸事件。后端扫描：

```text
/dev/input/event*
```

读取 ABS 坐标后映射到 960 x 640 面板内部坐标，并维护：

- pointer down/up
- pointer x/y
- 触摸拖动
- 滚动输入

拖动时会转换成 EUI 的 scroll input。这个路径通常需要 root 权限。

## 资源嵌入

CMake 构建阶段使用：

```text
cmake/embed_binary.cmake
```

将资源转成 C++ 字节数组：

```text
picture/avatar.png -> embedded_avatar.cpp
EUI-NE 字体资源   -> embedded_ui_font.cpp
```

运行时：

- `stb_image` 从内存解码头像 PNG。
- 渲染后端上传纹理。
- `stb_truetype` 将文字 glyph 栅格化成小纹理。
- 优先查找 Android 系统 CJK 字体，找不到再使用嵌入字体。

最终部署时不需要旁边放 `picture/` 目录。

## UI 结构

主 UI 在 `app/main.cpp` 中通过 primitive 直接绘制：

- 外壳和左侧栏
- 头像 cover crop
- 四个导航项
- 右侧内容面板
- 固定 header
- scroll scissor 和上下遮罩
- 文本 glyph texture
- DEMO/FRAME/EFFECT/SYS 四个页面

内容区域固定为：

```text
contentX=258
contentY=42
contentW=654
contentH=540
scrollTop=contentY + 126
scrollBottom=contentY + contentH - 42
```

可滚动内容先绘制在 scissor 中，然后绘制上下遮罩，最后重绘固定 header，避免滚动内容压到标题区。

## 构建集成

仓库内包含 Android ELF Port Kit：

```text
third_party/eui-neo-android-elf-port-kit
```

完整上游 EUI 源码通过子模块提供：

```text
third_party/EUI-NE
```

也可以通过构建脚本传入已有本地目录：

```powershell
-EuiRoot 'D:\path\to\EUI-NE'
```

`CMakeLists.txt` 将 NeoPanel 源码、Port Kit 后端、EUI-NE 渲染文件和生成的嵌入资源一起编译成 `neopanel_android`。

## 当前限制

- 触摸输入依赖 root 读取 `/dev/input`。
- 面板是固定 960 x 640 内部布局，不是通用 Android 自适应布局系统。
- EFFECT 页的 3D 效果是 primitive 的透视投影，不是独立 Vulkan 3D mesh 管线。
- 上游 EUI 作为 Git submodule 链接到本仓库，不直接复制为普通源码目录。
