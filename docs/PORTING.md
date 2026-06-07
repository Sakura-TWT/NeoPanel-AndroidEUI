# Android ELF 移植说明

NeoPanel Android ELF 是 EUI-NEO 的 Android root ELF 移植实现。它不走 APK、Activity 或 Android View 树，而是从 root shell 启动一个 arm64 ELF，自己创建 Surface、接入 Vulkan，并读取 `/dev/input`。

运行链路：

```text
root shell
  -> neopanel_android
  -> SurfaceComposerClient
  -> ANativeWindow
  -> VK_KHR_android_surface
  -> EUI-NEO render primitives
```

## 移植文件入口

- [Surface 与窗口](porting/surface.md)：固定面板 Surface、`ANativeWindow` 创建和窗口位置。
- [Vulkan 后端](porting/vulkan.md)：Android surface extension、loader 分支和链接库。
- [输入队列](porting/input.md)：`/dev/input/event*`、触摸坐标映射和滚动注入。
- [CMake 组织](porting/cmake.md)：项目源码、EUI-NEO 子模块、Android ELF 适配层的构建关系。
- [资源嵌入](porting/assets.md)：头像、字体和单 ELF 部署形态。

## 目录边界

```text
src/neopanel/
```

NeoPanel 面板样例。这里放 UI、字体、资源加载和渲染循环。

```text
src/eui_android_elf/
```

EUI-NEO 的 Android ELF 适配层。这里放 Android Surface、Vulkan 分支、输入队列补充和平台相关代码。

```text
third_party/EUI-NEO/
```

上游 EUI-NEO 子模块。本仓库不把自己的移植说明或适配代码放进 `third_party/`。

## 当前边界

- 触摸输入依赖 root 读取 `/dev/input`。
- 面板是固定设计尺寸，不是通用 Android 自适应布局系统。
- EFFECT 页的 3D 视觉是 primitive 透视投影，不是独立 3D mesh 管线。
- 没有系统输入法接入。
- 没有 APK 生命周期管理。
