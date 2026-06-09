#include "core/window/window_backend.h"

#if defined(EUI_WINDOW_BACKEND_ANDROID)

#include "core/input/input_state.h"
#include "core/platform/android/native_surface/ANativeWindowCreator.h"

#include <android/log.h>
#include <android/native_window.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <cerrno>
#include <fcntl.h>
#include <linux/input.h>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace core::window {
namespace {

constexpr int kDesignScreenWidth = 1080;
constexpr int kDesignScreenHeight = 2400;
constexpr int kDesignPanelX = 64;
constexpr int kDesignPanelY = 872;
constexpr int kDesignPanelWidth = 960;
constexpr int kDesignPanelHeight = 640;

struct AndroidWindow {
    ANativeWindow* nativeWindow = nullptr;
    int width = 0;
    int height = 0;
    int screenWidth = 0;
    int screenHeight = 0;
    int originX = 0;
    int originY = 0;
    std::string title;

    std::mutex pointerMutex;
    double pointerX = 0.0;
    double pointerY = 0.0;
    bool pointerDown = false;
    bool rawTouchDown = false;
    bool panelTouchActive = false;
    bool touchScrollActive = false;
    double lastTouchY = 0.0;

    std::atomic<bool> running{false};
    std::vector<std::thread> touchThreads;
};

struct TouchDeviceInfo {
    int fd = -1;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
};

using Clock = std::chrono::steady_clock;

Clock::time_point& startTime() {
    static Clock::time_point value = Clock::now();
    return value;
}

AndroidWindow* asAndroidWindow(Handle window) {
    return static_cast<AndroidWindow*>(window);
}

bool hasAbsCode(int fd, int code) {
    unsigned long bits[(ABS_MAX + 8) / (8 * sizeof(unsigned long))]{};
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) < 0) {
        return false;
    }
    const int index = code / static_cast<int>(8 * sizeof(unsigned long));
    const int shift = code % static_cast<int>(8 * sizeof(unsigned long));
    return (bits[index] & (1UL << shift)) != 0;
}

bool getAbsInfo(int fd, int code, input_absinfo& info) {
    std::memset(&info, 0, sizeof(info));
    return ioctl(fd, EVIOCGABS(code), &info) == 0 && info.maximum > info.minimum;
}

std::vector<TouchDeviceInfo> discoverTouchDevices(int screenWidth, int screenHeight) {
    std::vector<TouchDeviceInfo> devices;
    DIR* dir = opendir("/dev/input");
    if (dir == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, "NeoPanel", "Cannot open /dev/input for touch");
        return devices;
    }

    dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (std::strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        std::string path = std::string("/dev/input/") + entry->d_name;
        int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }

        const bool hasMtX = hasAbsCode(fd, ABS_MT_POSITION_X);
        const bool hasMtY = hasAbsCode(fd, ABS_MT_POSITION_Y);
        const bool hasX = hasAbsCode(fd, ABS_X);
        const bool hasY = hasAbsCode(fd, ABS_Y);
        if ((!hasMtX || !hasMtY) && (!hasX || !hasY)) {
            close(fd);
            continue;
        }

        input_absinfo absX{};
        input_absinfo absY{};
        const int xCode = hasMtX ? ABS_MT_POSITION_X : ABS_X;
        const int yCode = hasMtY ? ABS_MT_POSITION_Y : ABS_Y;
        if (!getAbsInfo(fd, xCode, absX) || !getAbsInfo(fd, yCode, absY)) {
            close(fd);
            continue;
        }

        TouchDeviceInfo device;
        device.fd = fd;
        device.scaleX = static_cast<float>(screenWidth) / static_cast<float>(absX.maximum - absX.minimum);
        device.scaleY = static_cast<float>(screenHeight) / static_cast<float>(absY.maximum - absY.minimum);
        devices.push_back(device);

        __android_log_print(ANDROID_LOG_INFO,
                            "NeoPanel",
                            "Touch device: %s scale=(%.3f, %.3f)",
                            path.c_str(),
                            device.scaleX,
                            device.scaleY);
    }

    closedir(dir);
    return devices;
}

void setPointer(AndroidWindow* window, float screenX, float screenY, bool down) {
    if (window == nullptr) {
        return;
    }

    const double localX = static_cast<double>(screenX - window->originX);
    const double localY = static_cast<double>(screenY - window->originY);
    const bool inside = localX >= 0.0 && localY >= 0.0 &&
                        localX <= static_cast<double>(window->width) &&
                        localY <= static_cast<double>(window->height);

    double scrollY = 0.0;
    {
        std::lock_guard<std::mutex> lock(window->pointerMutex);
        const bool startingTouch = down && !window->rawTouchDown;
        if (!down) {
            window->panelTouchActive = false;
        } else if (startingTouch && inside) {
            window->panelTouchActive = true;
        }
        window->rawTouchDown = down;

        const bool active = down && window->panelTouchActive;
        const bool scrollActive = active && inside;
        if (scrollActive && window->touchScrollActive) {
            const double dy = localY - window->lastTouchY;
            if (std::fabs(dy) >= 4.0) {
                scrollY = dy / 18.0;
            }
        }
        window->touchScrollActive = scrollActive;
        window->lastTouchY = localY;
        window->pointerX = localX;
        window->pointerY = localY;
        window->pointerDown = active;
    }

    if (scrollY != 0.0) {
        core::queueScrollInput(static_cast<Handle>(window), 0.0, scrollY);
    }
}

void touchThreadMain(AndroidWindow* window, TouchDeviceInfo device) {
    int currentX = 0;
    int currentY = 0;
    bool hasX = false;
    bool hasY = false;
    bool down = false;

    input_event events[64]{};
    while (window != nullptr && window->running.load()) {
        pollfd pollInfo{};
        pollInfo.fd = device.fd;
        pollInfo.events = POLLIN;
        const int pollResult = poll(&pollInfo, 1, 50);
        if (pollResult < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (pollResult == 0 || (pollInfo.revents & POLLIN) == 0) {
            continue;
        }

        const ssize_t bytes = read(device.fd, events, sizeof(events));
        if (bytes <= 0 || bytes % static_cast<ssize_t>(sizeof(input_event)) != 0) {
            if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                continue;
            }
            continue;
        }

        const size_t count = static_cast<size_t>(bytes) / sizeof(input_event);
        for (size_t i = 0; i < count; ++i) {
            const input_event& event = events[i];
            if (event.type == EV_ABS) {
                if (event.code == ABS_MT_POSITION_X || event.code == ABS_X) {
                    currentX = event.value;
                    hasX = true;
                } else if (event.code == ABS_MT_POSITION_Y || event.code == ABS_Y) {
                    currentY = event.value;
                    hasY = true;
                } else if (event.code == ABS_MT_TRACKING_ID) {
                    down = event.value >= 0;
                }
            } else if (event.type == EV_KEY && event.code == BTN_TOUCH) {
                down = event.value != 0;
            } else if (event.type == EV_SYN && event.code == SYN_REPORT) {
                if (hasX && hasY) {
                    setPointer(window,
                               static_cast<float>(currentX) * device.scaleX,
                               static_cast<float>(currentY) * device.scaleY,
                               down);
                } else if (!down) {
                    std::lock_guard<std::mutex> lock(window->pointerMutex);
                    window->pointerDown = false;
                    window->rawTouchDown = false;
                    window->panelTouchActive = false;
                    window->touchScrollActive = false;
                }
            }
        }
    }

    close(device.fd);
}

void startTouch(AndroidWindow* window) {
    if (window == nullptr || window->screenWidth <= 0 || window->screenHeight <= 0) {
        return;
    }

    std::vector<TouchDeviceInfo> devices = discoverTouchDevices(window->screenWidth, window->screenHeight);
    if (devices.empty()) {
        __android_log_print(ANDROID_LOG_WARN, "NeoPanel", "No touch input devices found");
        return;
    }

    window->running.store(true);
    for (TouchDeviceInfo& device : devices) {
        window->touchThreads.emplace_back(touchThreadMain, window, device);
    }
}

void stopTouch(AndroidWindow* window) {
    if (window == nullptr) {
        return;
    }
    window->running.store(false);
    for (std::thread& thread : window->touchThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    window->touchThreads.clear();
}

} // namespace

Handle createWindow(const WindowCreateRequest& request) {
    auto displayInfo = android::ANativeWindowCreator::GetDisplayInfo();
    const int screenWidth = displayInfo.width > 0 ? displayInfo.width : kDesignScreenWidth;
    const int screenHeight = displayInfo.height > 0 ? displayInfo.height : kDesignScreenHeight;

    const float scaleX = static_cast<float>(screenWidth) / static_cast<float>(kDesignScreenWidth);
    const float scaleY = static_cast<float>(screenHeight) / static_cast<float>(kDesignScreenHeight);
    const int width = request.width > 0 ? request.width : std::max(320, static_cast<int>(kDesignPanelWidth * scaleX));
    const int height = request.height > 0 ? request.height : std::max(240, static_cast<int>(kDesignPanelHeight * scaleY));
    const int originX = std::max(0, static_cast<int>(kDesignPanelX * scaleX));
    const int originY = std::max(0, static_cast<int>(kDesignPanelY * scaleY));

    const char* title = request.title != nullptr ? request.title : "NeoPanel";

    auto* window = new AndroidWindow;
    window->width = width;
    window->height = height;
    window->screenWidth = screenWidth;
    window->screenHeight = screenHeight;
    window->originX = originX;
    window->originY = originY;
    window->title = title;

    window->nativeWindow = android::ANativeWindowCreator::Create(title, width, height, false);
    if (window->nativeWindow == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "NeoPanel", "Failed to create Android native window");
        delete window;
        return nullptr;
    }

    android::ANativeWindowCreator::SetWindowPosition(window->nativeWindow,
                                                     static_cast<float>(originX),
                                                     static_cast<float>(originY),
                                                     INT_MAX,
                                                     0.0f);

    __android_log_print(ANDROID_LOG_INFO,
                        "NeoPanel",
                        "Created fixed panel: screen=%dx%d panel=%dx%d origin=%d,%d",
                        screenWidth,
                        screenHeight,
                        width,
                        height,
                        originX,
                        originY);

    startTouch(window);
    return window;
}

void destroyWindow(Handle handle) {
    AndroidWindow* window = asAndroidWindow(handle);
    if (window == nullptr) {
        return;
    }

    stopTouch(window);

    if (window->nativeWindow != nullptr) {
        android::ANativeWindowCreator::Destroy(window->nativeWindow);
        window->nativeWindow = nullptr;
    }

    delete window;
}

NativeWindowInfo nativeWindowInfo(Handle handle) {
    NativeWindowInfo result;
    result.handle = handle;
    AndroidWindow* window = asAndroidWindow(handle);
    if (window != nullptr) {
        result.platformWindow = window->nativeWindow;
        result.platformView = window;
    }
    return result;
}

ContextKey currentContextKey() {
    return nullptr;
}

double timeSeconds() {
    const auto now = Clock::now();
    return std::chrono::duration<double>(now - startTime()).count();
}

void postEmptyEvent() {
}

void getCursorPosition(Handle handle, double& x, double& y) {
    AndroidWindow* window = asAndroidWindow(handle);
    if (window == nullptr) {
        x = 0.0;
        y = 0.0;
        return;
    }

    std::lock_guard<std::mutex> lock(window->pointerMutex);
    x = window->pointerX;
    y = window->pointerY;
}

bool isMouseButtonDown(Handle handle, int button) {
    AndroidWindow* window = asAndroidWindow(handle);
    if (button != 0 || window == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(window->pointerMutex);
    return window->pointerDown;
}

std::string clipboardText(Handle) {
    return {};
}

void setClipboardText(const std::string&) {
}

CursorHandle createStandardCursor(CursorType) {
    return nullptr;
}

void setCursor(Handle, CursorHandle) {
}

void destroyCursor(CursorHandle) {
}

void setWindowIcon(Handle, int, int, unsigned char*) {
}

void setImeCursorRect(Handle, float, float, float, float) {
}

} // namespace core::window

#endif // defined(EUI_WINDOW_BACKEND_ANDROID)
