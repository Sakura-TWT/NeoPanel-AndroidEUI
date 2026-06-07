# 构建与运行

本文记录从源码构建 `neopanel_android`、验证 ELF、推送到设备运行的通用路径。示例命令不绑定某一台开发机的本地目录。

## 准备子模块

上游 EUI-NEO 以子模块方式放在 `third_party/EUI-NEO`：

```text
https://github.com/sudoevolve/EUI-NEO
```

首次克隆仓库：

```powershell
git clone --recurse-submodules https://github.com/Sakura-TWT/NeoPanel-Android-ELF
```

已有仓库时补齐子模块：

```powershell
git submodule update --init --recursive
```

也可以不用子模块，直接通过 `-EuiRoot` 指向已有的 EUI-NEO 源码目录。

## Windows 构建

推荐把 Android NDK 配置到环境变量，再直接运行脚本：

```powershell
$env:ANDROID_NDK_HOME = '<Android NDK 安装目录>'
powershell -ExecutionPolicy Bypass -File .\build_android.ps1
```

如果 CMake 或 Ninja 不在 PATH 中，可以显式传入：

```powershell
powershell -ExecutionPolicy Bypass -File .\build_android.ps1 `
  -NdkRoot '<Android NDK 安装目录>' `
  -CMakePath '<cmake.exe 路径>' `
  -NinjaPath '<ninja.exe 路径>'
```

如果不用仓库子模块，也可以指向已有的 EUI-NEO 源码目录：

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

输出：

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

确认没有额外部署文件：

```powershell
Get-ChildItem -Force build\android | Where-Object { $_.Name -like '*c++*' -or $_.Name -eq 'picture' }
```

期望无输出。头像和字体已经嵌入 ELF，`ANDROID_STL=c++_static` 也避免了 `libc++_shared.so`。

## 部署运行

示例：

```sh
adb push build/android/neopanel_android /data/local/tmp/neopanel/neopanel_android
adb push deploy_example.sh /data/local/tmp/neopanel/deploy_example.sh
adb shell su -c "chmod 755 /data/local/tmp/neopanel/neopanel_android /data/local/tmp/neopanel/deploy_example.sh"
adb shell su -c "/data/local/tmp/neopanel/deploy_example.sh"
```

程序会读取 `/dev/input/event*`，通常必须 root 才能正常响应触摸。

## 常见问题

构建提示找不到 EUI-NEO：

```powershell
git submodule update --init --recursive
```

或者显式传入：

```powershell
-EuiRoot '<EUI-NEO 源码目录>'
```

没有触摸响应：

- 确认用 root 启动。
- 确认 `/dev/input/event*` 可读。

Vulkan 或 Surface 创建失败：

- 确认设备支持 Vulkan。
- 确认系统存在 `libvulkan.so`。
- 查看 `deploy_example.sh` 生成的日志。
