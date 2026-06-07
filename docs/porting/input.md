# 输入队列

相关文件：

```text
src/eui_android_elf/core/input/input_state.h
src/eui_android_elf/core/input/input_types.h
src/eui_android_elf/core/window/android_window_backend_panel.cpp
```

## 输入来源

程序不是 APK，不会收到 Android View touch callback。触摸输入来自：

```text
/dev/input/event*
```

这个路径通常需要 root 权限。

## 坐标映射

后端读取 ABS 坐标后映射到 960 x 640 面板坐标。当前逻辑会根据真实屏幕尺寸和固定面板区域计算 local pointer。

## 滚动注入

手指纵向拖动会转成 EUI scroll input：

```cpp
core::queueScrollInput(window, 0.0, scrollY);
```

NeoPanel 的 UI 层再从 EUI input queue 中读取 pointer 和 scroll 状态。
