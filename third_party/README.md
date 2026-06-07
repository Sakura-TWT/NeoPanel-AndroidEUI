# 第三方代码

本目录只保留上游依赖。

## EUI-NEO

```text
third_party/EUI-NEO -> https://github.com/sudoevolve/EUI-NEO
```

这是 NeoPanel Android ELF 的移植来源。仓库通过 Git submodule 引用它，不在本目录放本项目自己的移植说明或适配代码。

初始化：

```sh
git submodule update --init --recursive
```

本项目自己的 Android ELF 适配层位于：

```text
src/eui_android_elf
```
