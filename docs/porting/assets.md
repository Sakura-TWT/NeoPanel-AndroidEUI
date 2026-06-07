# 资源嵌入

相关文件：

```text
CMakeLists.txt
cmake/embed_binary.cmake
src/neopanel/embedded_assets.h
picture/avatar.png
```

## 编译期资源

构建阶段会生成：

```text
picture/avatar.png -> embedded_avatar.cpp
EUI-NEO fallback font -> embedded_ui_font.cpp
```

字体来自 EUI-NEO 上游资源目录。

## 运行时

- `stb_image` 从内存解码头像。
- 渲染后端上传头像纹理。
- `stb_truetype` 栅格化文字 glyph。
- Android 系统 CJK 字体优先，失败后使用嵌入字体。

## 部署形态

最终部署只需要：

```text
neopanel_android
```

`build/android` 下不应出现 `picture/` 或 `libc++_shared.so`。
