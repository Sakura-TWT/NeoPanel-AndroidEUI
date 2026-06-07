# 第三方代码

本目录只放与移植直接相关的外部代码。

## EUI-NEO

```text
third_party/EUI-NEO -> https://github.com/sudoevolve/EUI-NEO
```

这是上游渲染框架，以 Git submodule 形式引用。

初始化：

```powershell
git submodule update --init --recursive
```

## Android ELF Port Kit

```text
third_party/eui-neo-android-elf-port-kit
```

这是本仓库整理出的 Android ELF 移植代码包，包含 Surface、Vulkan、input、示例和移植说明。它不是独立上游项目，也不是 EUI-NEO 的完整替代品。

发布或继续改造时，保留上游许可证和必要声明。
