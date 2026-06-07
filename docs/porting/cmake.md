# CMake 组织

相关文件：

```text
CMakeLists.txt
CMakePresets.json
src/neopanel/
src/eui_android_elf/
third_party/EUI-NEO/
```

## 三个源码边界

```text
src/neopanel/
```

NeoPanel 自己的面板样例。

```text
src/eui_android_elf/
```

为了让 EUI-NEO 跑在 Android ELF 上补充的适配层。

```text
third_party/EUI-NEO/
```

上游 EUI-NEO 子模块。

## 构建入口

仓库使用 CMake/Ninja 直接构建，不再提供宿主机包装脚本。

```sh
cmake --preset android-arm64-release
cmake --build --preset android-arm64-release
```

如果需要指向本地 EUI-NEO 源码，可以在配置时传入：

```sh
-DEUI_ROOT=<EUI-NEO 源码目录>
```

## 关键编译定义

```text
EUI_WINDOW_BACKEND_ANDROID=1
EUI_RENDER_BACKEND_VULKAN=1
VK_USE_PLATFORM_ANDROID_KHR=1
```
