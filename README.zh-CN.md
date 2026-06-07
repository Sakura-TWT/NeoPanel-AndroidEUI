# NeoPanel Android ELF

NeoPanel Android ELF 是一个 Android root ELF 可执行程序，不是 APK。它不依赖 Activity、Java/Kotlin 入口或 Android View 系统，而是直接创建 Android Surface，并通过 Vulkan 绘制 EUI 风格的面板 UI。

必须引用的原项目：

```text
https://github.com/sudoevolve/EUI-NE
```

## 产物

构建后得到：

```text
build/android/neopanel_android
```

在 Android root shell 中运行：

```sh
chmod 755 /data/local/tmp/neopanel/neopanel_android
/data/local/tmp/neopanel/neopanel_android
```

## 仓库结构

```text
app/
  main.cpp                         NeoPanel 面板 UI、文本、嵌入资源、主循环
  android_window_backend_panel.cpp Android Surface 和触摸输入后端
  embedded_assets.h                嵌入资源符号声明
cmake/
  embed_binary.cmake               将 PNG/TTF 转成 C++ 字节数组
  vulkan_portability_compat.h      Vulkan 兼容宏
docs/
  BUILD_AND_RUN.md                 构建、部署、验证
  PORTING.md                       Android ELF 移植说明
  PORTING.zh-CN.md                 中文移植说明
  GITHUB_RELEASE_CHECKLIST.md      上传 GitHub 前检查表
picture/
  avatar.png                       编译期嵌入的头像资源
third_party/
  eui-neo-android-elf-port-kit/    Android ELF Surface/Vulkan/input 移植代码
```

完整 EUI 上游源码已用 Git submodule 放在：

```text
third_party/EUI-NE
```

GitHub 页面会把它显示成可点击的上游链接目录。用户指定必须引用的链接是 `https://github.com/sudoevolve/EUI-NE`；当前实际可克隆并作为子模块使用的上游仓库是 `https://github.com/sudoevolve/EUI-NEO`。

## 构建

克隆本仓库时拉取子模块：

```powershell
git clone --recurse-submodules <this-repository-url>
powershell -ExecutionPolicy Bypass -File .\build_android.ps1 -NdkRoot 'D:\VSTool\Shared\Android\AndroidNDK\android-ndk-r23c'
```

如果已经克隆但没有拉取子模块：

```powershell
git submodule update --init --recursive
```

使用已有 EUI-NE 本地目录：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_android.ps1 `
  -NdkRoot 'D:\VSTool\Shared\Android\AndroidNDK\android-ndk-r23c' `
  -EuiRoot 'D:\AndroidEUI\EUI-NEO-0.4.0'
```

## 移植要点

- 用 `SurfaceComposerClient` 创建 Android Surface。
- 将 `ANativeWindow` 交给 Vulkan Android surface。
- 使用 `VK_KHR_android_surface`。
- 直接读取 `/dev/input/event*` 获取触摸输入。
- 将手指拖动转换为 EUI 滚动输入。
- 使用 `ANDROID_STL=c++_static`，避免部署 `libc++_shared.so`。
- 将头像 PNG 和字体资源在编译期嵌入 ELF。
- 运行时优先尝试 Android 系统 CJK 字体，失败后使用嵌入字体。

更完整的说明见 [docs/PORTING.zh-CN.md](docs/PORTING.zh-CN.md)。

## 上传 GitHub 前

参考：

```text
docs/GITHUB_RELEASE_CHECKLIST.md
```

注意不要提交：

- `build/`
- 设备日志
- 本地 IDE 文件
- 未确认许可证的完整上游源码副本

## 授权和引用

本仓库包含 NeoPanel Android ELF 工程代码和 Android ELF Port Kit 文件。EUI-NE 原项目必须保留引用：

```text
https://github.com/sudoevolve/EUI-NE
```

当前 Git submodule 使用的可克隆上游地址：

```text
https://github.com/sudoevolve/EUI-NEO
```

发布前请再次核对上游项目许可证，并保留所有必要声明。
