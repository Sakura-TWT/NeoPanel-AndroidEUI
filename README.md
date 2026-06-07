# NeoPanel Android ELF

NeoPanel Android ELF 目前定位为 EUI-NEO 在 Android root ELF 环境下的移植实现和验证样例。它可以从 Android root shell 直接启动，不经过 APK、Activity、Android View 或 Java/Kotlin 入口；程序自己创建 Android Surface，并把 `ANativeWindow` 交给 Vulkan 渲染。

界面层直接复用 EUI-NEO 的渲染 primitive 和 Vulkan 后端能力。上游项目以 Git submodule 形式保留在仓库中：

```text
third_party/EUI-NEO -> https://github.com/sudoevolve/EUI-NEO
```

## 当前产物

构建输出是一个 arm64 ELF：

```text
build/android/neopanel_android
```

部署后在 root shell 中运行：

```sh
chmod 755 /data/local/tmp/neopanel/neopanel_android
/data/local/tmp/neopanel/neopanel_android
```

## 目录结构

```text
src/
  main.cpp                         面板 UI、字体、资源、渲染循环
  android_window_backend_panel.cpp 固定面板 Surface 与触摸输入后端
  embedded_assets.h                编译期嵌入资源声明
cmake/
  embed_binary.cmake               将 PNG/TTF 转成 C++ 字节数组
  vulkan_portability_compat.h      Vulkan 兼容宏
docs/
  BUILD_AND_RUN.md                 构建、部署、验证
  PORTING.md                       Android ELF 移植说明
picture/
  avatar.png                       编译期嵌入的头像资源
third_party/
  EUI-NEO/                         上游 EUI-NEO 子模块
  eui-neo-android-elf-port-kit/    Android Surface/Vulkan/input 移植代码
```

## 构建

首次克隆仓库时建议拉取子模块：

```powershell
git clone --recurse-submodules https://github.com/Sakura-TWT/NeoPanel-Android-ELF
```

如果已经克隆过仓库：

```powershell
git submodule update --init --recursive
```

在 Windows 上构建时，建议先通过环境变量指定 Android NDK：

```powershell
$env:ANDROID_NDK_HOME = '<Android NDK 安装目录>'
powershell -ExecutionPolicy Bypass -File .\build_android.ps1
```

也可以使用已有的 EUI-NEO 本地源码：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_android.ps1 `
  -NdkRoot '<Android NDK 安装目录>' `
  -EuiRoot '<EUI-NEO 源码目录>'
```

更多命令见 [docs/BUILD_AND_RUN.md](docs/BUILD_AND_RUN.md)。

## 移植核心

这个工程把桌面 UI 框架常见的窗口入口替换成 Android root ELF 路径：

- 使用 SurfaceComposer 创建 Surface。
- 通过 `VK_KHR_android_surface` 把 Vulkan 渲染到 `ANativeWindow`。
- 从 `/dev/input/event*` 读取触摸输入。
- 将手指拖动转换成 EUI 滚动输入。
- 使用 `ANDROID_STL=c++_static`，运行时不需要额外部署 `libc++_shared.so`。
- 将头像和字体在编译期嵌入 ELF，运行时不依赖旁置资源目录。

移植细节见 [docs/PORTING.md](docs/PORTING.md)。

## 许可证

本仓库保留 Apache-2.0 许可证文本。EUI-NEO 上游源码以子模块方式引用，发布和二次分发时应保留上游许可证与声明。
