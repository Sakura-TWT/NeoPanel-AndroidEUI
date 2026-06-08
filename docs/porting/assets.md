# 资源嵌入

相关文件：

```text
CMakeLists.txt
cmake/embed_binary.cmake
src/neopanel/embedded_assets.h
assets/fonts/iOS26-PingFang-Jian-VF.ttf
picture/avatar.png
```

## 编译期资源

构建阶段会生成：

```text
picture/avatar.png -> embedded_avatar.cpp
assets/fonts/iOS26-PingFang-Jian-VF.ttf -> embedded_ui_font.cpp
```

UI 字体固定使用仓库内嵌的 PingFang 简体可变字体，不再优先读取设备系统字体。

## 运行时

- `stb_image` 从内存解码头像。
- 渲染后端上传头像纹理。
- `stb_truetype` 栅格化文字 glyph。
- 文本只使用编译期嵌入字体，保证不同 Android 设备上的字形一致。

## 部署形态

最终部署只需要：

```text
neopanel_android
```


