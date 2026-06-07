# NeoPanel Android ELF

NeoPanel Android ELF 是 EUI-NEO 的 Android root ELF 移植实现。

它不是 APK，也不是新的 UI 框架。仓库保留的是一条 Android ELF 路线：从 root shell 启动 arm64 ELF，由程序创建 Android Surface，把 `ANativeWindow` 交给 Vulkan，并用 EUI-NEO 的 primitive 绘制界面。

移植来源：

```text
EUI-NEO -> https://github.com/sudoevolve/EUI-NEO
```

## 文档入口

- [构建与运行](docs/BUILD_AND_RUN.md)：准备子模块、配置 CMake、构建 ELF、推送到设备运行。
- [Android ELF 移植说明](docs/PORTING.md)：说明本仓库怎么拆分 EUI-NEO、Android ELF 适配层和面板样例。
- [第三方代码](third_party/README.md)：说明上游 EUI-NEO 子模块。

## 目录结构

```text
src/
  neopanel/                         NeoPanel 面板样例
  eui_android_elf/                  EUI-NEO Android ELF 适配层
cmake/
  embed_binary.cmake                将 PNG/TTF 转成 C++ 字节数组
  vulkan_portability_compat.h       Vulkan 兼容宏
docs/
  BUILD_AND_RUN.md                  构建、部署、验证
  PORTING.md                        移植说明入口
  porting/                          Surface/Vulkan/input/CMake 细节
picture/
  avatar.png                        编译期嵌入的头像资源
third_party/
  EUI-NEO/                          上游 EUI-NEO 子模块
```

`third_party/` 只放上游依赖。当前仓库自己的 Android ELF 适配代码放在 `src/eui_android_elf/`，避免把移植方法和第三方源码混在一起。

## 快速构建

首次克隆时拉取子模块：

```sh
git clone --recurse-submodules https://github.com/Sakura-TWT/NeoPanel-Android-ELF
```

已有仓库时补齐子模块：

```sh
git submodule update --init --recursive
```

设置 Android NDK 后构建。`ANDROID_NDK_HOME` 需要指向 Android NDK 根目录：

```sh
export ANDROID_NDK_HOME=<Android NDK 安装目录>
cmake --preset android-arm64-release
cmake --build --preset android-arm64-release
```

构建输出：

```text
build/android/neopanel_android
```

## 移植范围

当前仓库覆盖：

- SurfaceComposer 创建 Android Surface。
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
