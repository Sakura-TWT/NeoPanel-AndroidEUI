#include "eui/app.h"
#include "core/app/app_runner.h"
#include "core/app/dsl_window_manager.h"
#include "core/app/dsl_window_runtime.h"
#include "core/app/main_window_runtime.h"
#include "core/input/input_state.h"
#include "core/platform/android/native_surface/ANativeWindowCreator.h"
#include "core/platform/platform.h"
#include "core/render/render_backend.h"
#include "core/window/window_backend.h"

#include <android/log.h>
#include <android/native_window.h>

#include <chrono>
#include <memory>
#include <thread>

namespace {

struct WindowState : app::AppRunner {
    bool running = true;
    core::render::RenderBackend* renderBackend = nullptr;
};

int drawableWidth(core::window::Handle window) {
    const auto info = core::window::nativeWindowInfo(window);
    auto* nativeWindow = static_cast<ANativeWindow*>(info.platformWindow);
    const int width = nativeWindow != nullptr ? ANativeWindow_getWidth(nativeWindow) : 0;
    return width > 0 ? width : (app::initialWindowWidth() > 0 ? app::initialWindowWidth() : 720);
}

int drawableHeight(core::window::Handle window) {
    const auto info = core::window::nativeWindowInfo(window);
    auto* nativeWindow = static_cast<ANativeWindow*>(info.platformWindow);
    const int height = nativeWindow != nullptr ? ANativeWindow_getHeight(nativeWindow) : 0;
    return height > 0 ? height : (app::initialWindowHeight() > 0 ? app::initialWindowHeight() : 1280);
}

double androidRefreshRate() {
    return 60.0;
}

} // namespace

int main() {
    __android_log_print(ANDROID_LOG_INFO, "EUI-NEO", "Android ELF main started");

    core::render::initializeRenderBackendLoader();

    core::window::WindowCreateRequest windowRequest;
    windowRequest.width = app::initialWindowWidth();
    windowRequest.height = app::initialWindowHeight();
    windowRequest.title = app::windowTitle();
    windowRequest.renderApi = core::render::windowRenderApi();

    core::window::Handle window = core::window::createWindow(windowRequest);
    if (window == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "EUI-NEO", "Failed to create Android window");
        return -1;
    }

    auto renderBackend = core::render::createRenderBackend(window);
    if (!renderBackend) {
        __android_log_print(ANDROID_LOG_ERROR, "EUI-NEO", "Failed to create render backend");
        core::window::destroyWindow(window);
        return -1;
    }

    if (!renderBackend->initialize()) {
        __android_log_print(ANDROID_LOG_ERROR, "EUI-NEO", "Failed to initialize render backend");
        renderBackend.reset();
        core::window::destroyWindow(window);
        return -1;
    }

    if (!app::initialize(window)) {
        __android_log_print(ANDROID_LOG_ERROR, "EUI-NEO", "Failed to initialize app");
        app::shutdown();
        renderBackend.reset();
        core::window::destroyWindow(window);
        return -1;
    }

    WindowState state;
    state.resetTiming(core::window::timeSeconds());
    state.updateFrameInterval(androidRefreshRate(), core::window::timeSeconds(), true);
    state.renderBackend = renderBackend.get();

    app::MainWindowRuntime mainWindowRuntime(state);

    while (state.running) {
        const double now = core::window::timeSeconds();
        const int width = drawableWidth(window);
        const int height = drawableHeight(window);
        if (width <= 0 || height <= 0) {
            mainWindowRuntime.markUnavailableFrame(now);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        mainWindowRuntime.runFrame(
            window,
            *renderBackend,
            {width, height, 1.0f, 1.0f},
            now,
            androidRefreshRate(),
            true,
            [] {
                (void)app::consumeWindowRequests();
            },
            [](float, bool) {
            },
            [](const char*) {
            },
            [] {
                return false;
            });

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    core::releaseInputQueue(window);
    renderBackend->makeCurrent();
    renderBackend->releaseRenderCache();
    {
        core::render::ScopedRenderBackend scopedRenderBackend(*renderBackend);
        app::shutdown();
    }
    renderBackend.reset();
    core::window::destroyWindow(window);

    __android_log_print(ANDROID_LOG_INFO, "EUI-NEO", "Android ELF main exited");
    return 0;
}