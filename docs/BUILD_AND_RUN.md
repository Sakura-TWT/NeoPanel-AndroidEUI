# 构建与运行

本文记录直接使用 CMake 构建 `neopanel_android` 的步骤。仓库不再提供宿主机包装脚本，避免把开发机环境写进项目入口。

## 准备源码

首次克隆仓库：

```sh
git clone --recurse-submodules https://github.com/Sakura-TWT/NeoPanel-Android-ELF
```

已有仓库时补齐子模块：

```sh
git submodule update --init --recursive
```

上游 EUI-NEO 默认来自：

```text
third_party/EUI-NEO
```

## 准备工具

需要：

```text
CMake
Ninja
Android NDK
```

`cmake` 和 `ninja` 应在 `PATH` 中。Android NDK 通过环境变量传入，`ANDROID_NDK_HOME` 指向 Android NDK 根目录：

```sh
export ANDROID_NDK_HOME=<Android NDK 安装目录>
```

## 构建

使用 CMake preset：

```sh
cmake --preset android-arm64-release
cmake --build --preset android-arm64-release
```

如果不用 preset，可以显式写出配置：

```sh
cmake -S . -B build/android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build/android --parallel
```

如果要使用本地 EUI-NEO 源码，而不是仓库子模块：

```sh
cmake -S . -B build/android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DEUI_ROOT=<EUI-NEO 源码目录> \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DANDROID_STL=c++_static \
  -DCMAKE_BUILD_TYPE=Release
```

输出文件：

```text
build/android/neopanel_android
```

## 验证 ELF

```sh
"$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readelf" -h build/android/neopanel_android
```

在 Windows 以外的主机上，把 `windows-x86_64` 换成 NDK 对应的 host 目录。

期望关键信息：

```text
Class:   ELF64
Type:    DYN
Machine: AArch64
```

确认没有旁置资源目录或 shared libc++：

```sh
ls build/android
```

头像和字体已经嵌入 ELF，C++ STL 使用静态链接。

## 推送到设备

示例：

```sh
adb shell su -c "mkdir -p /data/local/tmp/neopanel"
adb push build/android/neopanel_android /data/local/tmp/neopanel/neopanel_android
adb push deploy_example.sh /data/local/tmp/neopanel/deploy_example.sh
adb shell su -c "chmod 755 /data/local/tmp/neopanel/neopanel_android /data/local/tmp/neopanel/deploy_example.sh"
adb shell su -c "/data/local/tmp/neopanel/deploy_example.sh"
```

程序从 `/dev/input/event*` 读取触摸事件。没有 root 权限时，窗口可能能创建，但触摸通常不可用。

退出程序时可以发送 `Ctrl+C` 或 `SIGTERM`。NeoPanel 会进入清理路径，停止触摸输入线程，释放输入队列、Vulkan 纹理和 Android Surface。若执行环境直接 `SIGKILL`/强杀进程，系统仍会回收进程资源，但应用自身的日志化清理路径不会执行。

默认目标帧率为 60 FPS，以降低持续 CPU/GPU 压力；FRAME 页面仍可临时调到 30-120 FPS 做性能观察。

## 常见问题

找不到 EUI-NEO：

```sh
git submodule update --init --recursive
```

或者配置时传入：

```sh
-DEUI_ROOT=<EUI-NEO 源码目录>
```

找不到 Android NDK：

```sh
export ANDROID_NDK_HOME=<Android NDK 安装目录>
```

没有触摸响应：

- 确认使用 root 启动。
- 确认 `/dev/input/event*` 存在并可读。

Vulkan 或 Surface 创建失败：

- 确认设备支持 Vulkan。
- 确认系统存在 `libvulkan.so`。
- 查看 `/data/local/tmp/neopanel/neopanel_android.log`。
