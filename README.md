# NeoPanel Android ELF

NeoPanel Android ELF 是 EUI-NEO 在 Android root ELF 环境下的一次移植实现。

它不是 APK，也不是一个新的 UI 框架。仓库保留的是一条 Android ELF 路线：从 root shell 启动 arm64 ELF，由程序自己创建 Android Surface，把 `ANativeWindow` 交给 Vulkan，并用 EUI-NEO 的 primitive 绘制界面。

当前面板用于测试这条移植链路。需要关注的代码主要在 Android Surface、Vulkan、输入和资源嵌入几部分。

## 文档入口

- [构建与运行](docs/BUILD_AND_RUN.md)：准备子模块、构建 ELF、验证产物、推送到设备运行。
- [Android ELF 移植说明](docs/PORTING.md)：说明窗口、Vulkan、输入和资源嵌入的移植边界。
- [第三方代码](third_party/README.md)：说明 EUI-NEO 子模块和 Android ELF Port Kit 的来源。
- [Android ELF Port Kit](third_party/eui-neo-android-elf-port-kit/README_Android_ELF_Port.md)：记录 port-kit 目录里的 Surface、Vulkan、input 和 CMake 移植点。

## 当前产物

构建输出：

```text
build/android/neopanel_android
```

运行方式：

```sh
chmod 755 /data/local/tmp/neopanel/neopanel_android
/data/local/tmp/neopanel/neopanel_android
```

程序会读取 `/dev/input/event*`，通常需要 root 权限。

## 目录结构

```text
src/
  main.cpp                         面板样例、字体、资源、渲染循环
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

## 快速构建

首次克隆时拉取子模块：

```powershell
git clone --recurse-submodules https://github.com/Sakura-TWT/NeoPanel-Android-ELF
```

如果已经克隆过仓库：

```powershell
git submodule update --init --recursive
```

设置 Android NDK 后构建：

```powershell
$env:ANDROID_NDK_HOME = '<Android NDK 安装目录>'
powershell -ExecutionPolicy Bypass -File .\build_android.ps1
```

如果 CMake、Ninja 或 EUI-NEO 不在默认位置，可以显式传入：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_android.ps1 `
  -NdkRoot '<Android NDK 安装目录>' `
  -CMakePath '<cmake.exe 路径>' `
  -NinjaPath '<ninja.exe 路径>' `
  -EuiRoot '<EUI-NEO 源码目录>'
```

## 移植范围

这个仓库目前覆盖：

- `SurfaceComposerClient` 创建 Android Surface。
- `VK_KHR_android_surface` 将 Vulkan 渲染到 `ANativeWindow`。
- `/dev/input/event*` 触摸输入读取。
- 手指拖动到 EUI scroll input 的转换。
- `ANDROID_STL=c++_static` 静态链接。
- 头像和字体在编译期嵌入 ELF。

暂不覆盖：

- APK、Activity、Android View 和输入法接入。
- 通用 Android 自适应布局系统。
- 完整 EUI-NEO 工程替代方案。

## 许可证

本仓库保留 Apache-2.0 许可证文本。EUI-NEO 以子模块方式引用，发布和二次分发时应保留上游许可证与声明。
