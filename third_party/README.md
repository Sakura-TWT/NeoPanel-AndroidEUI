# 第三方代码

本目录保留两个外部来源。

## EUI-NEO

```text
third_party/EUI-NEO -> https://github.com/sudoevolve/EUI-NEO
```

这是主渲染框架来源，以 Git submodule 形式保留，GitHub 页面会显示为可点击的上游链接目录。

初始化子模块：

```powershell
git submodule update --init --recursive
```

## Android ELF Port Kit

```text
third_party/eui-neo-android-elf-port-kit
```

这里保存 Android root ELF 移植相关的 Surface、Vulkan、input 和示例文件。它是本工程能脱离 APK 运行的关键移植层。

发布时应保留上游许可证和必要声明。
