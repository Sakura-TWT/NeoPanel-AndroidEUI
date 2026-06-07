# Surface 与窗口

相关文件：

```text
src/eui_android_elf/core/platform/android/native_surface/ANativeWindowCreator.h
src/eui_android_elf/core/window/android_window_backend_panel.cpp
```

## Surface 创建

`ANativeWindowCreator.h` 负责解析 Android SurfaceComposer 相关符号，创建 SurfaceControl，并取得 `ANativeWindow`。

当前路径使用 Android 私有 C++ 符号。不同系统版本可能需要适配，所以这里不把 SurfaceComposer 当成稳定公共 API。

## 固定面板区域

设计坐标：

```text
Design screen:      1080 x 2400
Panel design rect:  x=64, y=872, width=960, height=640
Internal UI size:   960 x 640
```

`android_window_backend_panel.cpp` 根据真实屏幕尺寸按比例缩放这个矩形。Surface 只负责承载像素；圆角、阴影、遮罩和内容都由 EUI primitive 绘制。

## 为什么不依赖系统圆角

系统 Surface 圆角在不同 Android 版本上不稳定。NeoPanel 只把系统 Surface 当作渲染目标，视觉边界由 EUI 绘制层控制。
