# 构建与运行

本文记录从源码构建 `neopanel_android`、检查 ELF、推送到设备运行的步骤。命令使用占位符，不绑定具体开发机路径。

## 准备源码

首次克隆仓库：

```powershell
git clone --recurse-submodules https://github.com/Sakura-TWT/NeoPanel-Android-ELF
```

已有仓库时补齐子模块：

```powershell
git submodule update --init --recursive
```

上游 EUI-NEO 默认来自：

```text
third_party/EUI-NEO
```

也可以通过 `-EuiRoot` 指向本地已有的 EUI-NEO 源码。

## 构建

推荐先设置 Android NDK：

```powershell
$env:ANDROID_NDK_HOME = '<Android NDK 安装目录>'
powershell -ExecutionPolicy Bypass -File .\build_android.ps1
```

如果 CMake 或 Ninja 不在 `PATH` 中：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_android.ps1 `
  -NdkRoot '<Android NDK 安装目录>' `
  -CMakePath '<cmake.exe 路径>' `
  -NinjaPath '<ninja.exe 路径>'
```

如果不用仓库子模块：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_android.ps1 `
  -NdkRoot '<Android NDK 安装目录>' `
  -EuiRoot '<EUI-NEO 源码目录>'
```

脚本默认配置：

```text
ANDROID_ABI=arm64-v8a
ANDROID_PLATFORM=android-26
ANDROID_STL=c++_static
CMAKE_BUILD_TYPE=Release
```

输出文件：

```text
build/android/neopanel_android
```

## 验证 ELF

```powershell
& "$env:ANDROID_NDK_HOME\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe" -h build\android\neopanel_android |
  Select-String -Pattern 'Class:|Machine:|Type:'
```

期望结果：

```text
Class:   ELF64
Type:    DYN
Machine: AArch64
```

确认没有旁置资源目录或 shared libc++：

```powershell
Get-ChildItem -Force build\android | Where-Object { $_.Name -like '*c++*' -or $_.Name -eq 'picture' }
```

期望无输出。头像和字体已经嵌入 ELF，C++ STL 使用静态链接。

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

## 常见问题

找不到 EUI-NEO：

```powershell
git submodule update --init --recursive
```

或者传入：

```powershell
-EuiRoot '<EUI-NEO 源码目录>'
```

找不到 Android NDK：

```powershell
$env:ANDROID_NDK_HOME = '<Android NDK 安装目录>'
```

没有触摸响应：

- 确认使用 root 启动。
- 确认 `/dev/input/event*` 存在并可读。

Vulkan 或 Surface 创建失败：

- 确认设备支持 Vulkan。
- 确认系统存在 `libvulkan.so`。
- 查看 `/data/local/tmp/neopanel/neopanel_android.log`。
