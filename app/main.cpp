#include "app/embedded_assets.h"

#include "core/input/input_state.h"
#include "core/render/primitive.h"
#include "core/render/render_backend.h"
#include "core/window/window_backend.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "3rd/stb_truetype.h"
#include "3rd/stb_image.h"

#include <android/log.h>
#include <android/native_window.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kPanelWidth = 960;
constexpr int kPanelHeight = 640;
constexpr float kShellRadius = 56.0f;
constexpr int kGraphSamples = 56;
constexpr float kFpsMin = 30.0f;
constexpr float kFpsMax = 120.0f;

bool gChinese = false;
bool gNight = true;
float gTargetFps = 90.0f;
int gSceneMode = 0;

struct EmbeddedTexture {
    core::render::RenderBackend::TextureHandle handle = nullptr;
    int width = 0;
    int height = 0;
    bool decoded = false;
    bool uploaded = false;
    std::vector<unsigned char> pixels;
};

struct RuntimeStats {
    double previousSampleTime = 0.0;
    double previousCpuTime = 0.0;
    float fps = 0.0f;
    float frameMs = 0.0f;
    float cpuPercent = 0.0f;
    float memoryMb = 0.0f;
    float loadPercent = 0.0f;
    float frameAccumulator = 0.0f;
    int frameCount = 0;
};

struct PanelState {
    int selected = 0;
    int pressedNav = -1;
    int pressedDemoButton = -1;
    int demoButton = 0;
    int activeControl = 0;
    int pressedSceneMode = -1;
    bool pressedLanguage = false;
    bool pressedTheme = false;
    bool pressedDemoSwitch = false;
    bool demoSwitch = true;
    float demoSlider = 0.44f;
    float navBlend = 0.0f;
    float contentScroll = 0.0f;
    float targetScroll = 0.0f;
    float launchTime = 0.0f;
    double previousTime = 0.0;
    bool previousDown = false;
    EmbeddedTexture avatar;
    RuntimeStats stats;
    std::array<float, kGraphSamples> fpsSamples{};
    std::array<float, kGraphSamples> cpuSamples{};
    int graphCursor = 0;
};

struct NavItem {
    const char* labelEn;
    const char* detailEn;
    const char* labelZh;
    const char* detailZh;
    int icon;
};

struct PageInfo {
    const char* titleEn;
    const char* subtitleEn;
    const char* tagEn;
    const char* titleZh;
    const char* subtitleZh;
    const char* tagZh;
};

struct Metric {
    const char* label;
    char value[24];
    const char* noteEn;
    const char* noteZh;
    float normalized;
    core::Color color;
};

struct LocalText {
    const char* en;
    const char* zh;
};

constexpr std::array<NavItem, 4> kNavItems{{
    {"DEMO", "atelier", "展示", "工坊", 0},
    {"FRAME", "telemetry", "帧率", "性能", 1},
    {"EFFECT", "motion", "动效", "空间", 2},
    {"SYS", "runtime", "系统", "运行", 3},
}};

constexpr std::array<PageInfo, 4> kPages{{
    {"EUI NEO ATELIER", "A compact study of controls, material, image and rhythm.", "SHOWCASE",
     "EUI NEO 工坊", "控件 材质 图像与节奏的紧凑展示", "展示"},
    {"FRAME FIELD", "Performance is isolated here with a live frame-rate selector.", "TELEMETRY",
     "帧率场", "性能独立展示 可实时选择整体帧率", "性能"},
    {"EFFECT STAGE", "Perspective card motion inspired by the EUI gallery.", "MOTION",
     "动效舞台", "参考 EUI Gallery 的透视卡片动效", "动效"},
    {"SYSTEM NOTES", "Android Surface, Vulkan backend, static STL and ELF deployment.", "RUNTIME",
     "系统笔记", "安卓 Surface Vulkan 静态 STL 与 ELF 部署", "运行"},
}};

constexpr std::array<std::array<unsigned char, 5>, 43> kGlyphs{{
    {{0x7C, 0x82, 0x82, 0x82, 0x7C}}, // 0
    {{0x00, 0x84, 0xFE, 0x80, 0x00}}, // 1
    {{0xC4, 0xA2, 0x92, 0x92, 0x8C}}, // 2
    {{0x44, 0x82, 0x92, 0x92, 0x6C}}, // 3
    {{0x30, 0x28, 0x24, 0xFE, 0x20}}, // 4
    {{0x5E, 0x92, 0x92, 0x92, 0x62}}, // 5
    {{0x7C, 0x92, 0x92, 0x92, 0x64}}, // 6
    {{0x02, 0xE2, 0x12, 0x0A, 0x06}}, // 7
    {{0x6C, 0x92, 0x92, 0x92, 0x6C}}, // 8
    {{0x4C, 0x92, 0x92, 0x92, 0x7C}}, // 9
    {{0xFC, 0x12, 0x12, 0x12, 0xFC}}, // A
    {{0xFE, 0x92, 0x92, 0x92, 0x6C}}, // B
    {{0x7C, 0x82, 0x82, 0x82, 0x44}}, // C
    {{0xFE, 0x82, 0x82, 0x82, 0x7C}}, // D
    {{0xFE, 0x92, 0x92, 0x92, 0x82}}, // E
    {{0xFE, 0x12, 0x12, 0x12, 0x02}}, // F
    {{0x7C, 0x82, 0x92, 0x92, 0x74}}, // G
    {{0xFE, 0x10, 0x10, 0x10, 0xFE}}, // H
    {{0x82, 0x82, 0xFE, 0x82, 0x82}}, // I
    {{0x40, 0x80, 0x82, 0x7E, 0x02}}, // J
    {{0xFE, 0x10, 0x28, 0x44, 0x82}}, // K
    {{0xFE, 0x80, 0x80, 0x80, 0x80}}, // L
    {{0xFE, 0x04, 0x18, 0x04, 0xFE}}, // M
    {{0xFE, 0x08, 0x10, 0x20, 0xFE}}, // N
    {{0x7C, 0x82, 0x82, 0x82, 0x7C}}, // O
    {{0xFE, 0x12, 0x12, 0x12, 0x0C}}, // P
    {{0x7C, 0x82, 0xA2, 0x42, 0xBC}}, // Q
    {{0xFE, 0x12, 0x32, 0x52, 0x8C}}, // R
    {{0x4C, 0x92, 0x92, 0x92, 0x64}}, // S
    {{0x02, 0x02, 0xFE, 0x02, 0x02}}, // T
    {{0x7E, 0x80, 0x80, 0x80, 0x7E}}, // U
    {{0x3E, 0x40, 0x80, 0x40, 0x3E}}, // V
    {{0x7E, 0x80, 0x70, 0x80, 0x7E}}, // W
    {{0xC6, 0x28, 0x10, 0x28, 0xC6}}, // X
    {{0x06, 0x08, 0xF0, 0x08, 0x06}}, // Y
    {{0xC2, 0xA2, 0x92, 0x8A, 0x86}}, // Z
    {{0x00, 0x00, 0xC0, 0xC0, 0x00}}, // .
    {{0x80, 0x60, 0x18, 0x06, 0x00}}, // /
    {{0x10, 0x10, 0x10, 0x10, 0x10}}, // -
    {{0x00, 0x6C, 0x6C, 0x00, 0x00}}, // :
    {{0x4C, 0x2A, 0x1A, 0x28, 0x64}}, // %
    {{0x00, 0x00, 0x00, 0x00, 0x00}}, // space
    {{0x02, 0x02, 0x02, 0x02, 0x02}}, // _
}};

core::Color rgba(float r, float g, float b, float a = 1.0f) {
    return {r, g, b, a};
}

core::Color mix(const core::Color& a, const core::Color& b, float t) {
    return core::mixColor(a, b, t);
}

float clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

float approach(float current, float target, float deltaSeconds, float sharpness) {
    const float t = 1.0f - std::exp(-sharpness * std::max(0.0f, deltaSeconds));
    return current + (target - current) * t;
}

float easeOutCubic(float t) {
    t = clamp01(t);
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

float easeOutBack(float t) {
    t = clamp01(t);
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    const float p = t - 1.0f;
    return 1.0f + c3 * p * p * p + c1 * p * p;
}

bool contains(const core::Rect& rect, double x, double y) {
    return rect.contains(x, y);
}

const char* tr(const char* en, const char* zh) {
    return gChinese ? zh : en;
}

const char* tr(const LocalText& text) {
    return tr(text.en, text.zh);
}

core::Color textMain() {
    return gNight ? rgba(0.96f, 0.97f, 1.0f, 1.0f) : rgba(0.12f, 0.11f, 0.14f, 1.0f);
}

core::Color textSoft() {
    return gNight ? rgba(0.70f, 0.75f, 0.86f, 0.92f) : rgba(0.32f, 0.34f, 0.42f, 0.92f);
}

core::Color tileText() {
    return gNight ? rgba(0.92f, 0.94f, 1.0f, 0.98f) : rgba(0.13f, 0.12f, 0.16f, 0.98f);
}

core::Color tileTextSoft() {
    return gNight ? rgba(0.70f, 0.74f, 0.84f, 0.86f) : rgba(0.34f, 0.35f, 0.43f, 0.90f);
}

core::Color trackColor(float opacity) {
    return gNight ? rgba(1.0f, 1.0f, 1.0f, 0.12f * opacity)
                  : rgba(0.14f, 0.15f, 0.20f, 0.16f * opacity);
}

core::Color shellStart() {
    return gNight ? rgba(0.055f, 0.044f, 0.064f, 0.90f) : rgba(0.955f, 0.955f, 0.970f, 0.95f);
}

core::Color shellEnd() {
    return gNight ? rgba(0.030f, 0.026f, 0.038f, 0.90f) : rgba(0.830f, 0.875f, 0.925f, 0.94f);
}

core::Color railFill() {
    return gNight ? rgba(1.0f, 1.0f, 1.0f, 0.060f) : rgba(0.10f, 0.12f, 0.18f, 0.075f);
}

core::Color contentFill(float opacity) {
    return gNight ? rgba(0.10f, 0.075f, 0.105f, 0.70f * opacity)
                  : rgba(0.985f, 0.975f, 0.982f, 0.94f * opacity);
}

core::Color contentMask(float opacity) {
    return gNight ? rgba(0.10f, 0.075f, 0.105f, 0.985f * opacity)
                  : rgba(0.985f, 0.975f, 0.982f, 0.995f * opacity);
}

core::Color softTile(float opacity) {
    return gNight ? rgba(1.0f, 1.0f, 1.0f, 0.060f * opacity)
                  : rgba(0.12f, 0.12f, 0.17f, 0.070f * opacity);
}

core::Color subtleBorder(float opacity) {
    return gNight ? rgba(1.0f, 1.0f, 1.0f, 0.12f * opacity)
                  : rgba(0.18f, 0.16f, 0.22f, 0.14f * opacity);
}

core::Rect languageToggleRect() {
    return {46.0f, 588.0f, 170.0f, 34.0f};
}

core::Rect themeToggleRect() {
    return {868.0f, 18.0f, 44.0f, 44.0f};
}

core::Rect fpsSliderRect(float contentX, float contentW, float top) {
    return {contentX + 54.0f, top + 318.0f, contentW - 130.0f, 34.0f};
}

core::Rect expandedRect(core::Rect rect, float amount) {
    return {rect.x - amount, rect.y - amount, rect.width + amount * 2.0f, rect.height + amount * 2.0f};
}

core::Rect sceneModeRect(int index, float contentX, float contentW, float stageY) {
    constexpr float buttonW = 74.0f;
    constexpr float gap = 8.0f;
    constexpr float totalW = buttonW * 3.0f + gap * 2.0f;
    const float startX = contentX + contentW - 30.0f - totalW;
    return {startX + static_cast<float>(index) * (buttonW + gap), stageY + 22.0f, buttonW, 32.0f};
}

core::Rect demoButtonRect(int index, float x, float y, float width) {
    const float bw = (width - 28.0f) / 3.0f;
    return {x + static_cast<float>(index) * (bw + 14.0f), y, bw, 54.0f};
}

core::Rect demoSwitchRect(float x, float y, float width) {
    return {x + width * 0.62f + 18.0f, y + 42.0f, 94.0f, 38.0f};
}

core::Rect demoSliderRect(float x, float y, float width) {
    return {x, y + 64.0f, width, 42.0f};
}

void setRenderScissor(bool enabled, const core::Rect& rect) {
    if (core::render::RenderBackend* backend = core::render::activeRenderBackend()) {
        backend->setScissor(enabled, rect, kPanelHeight);
    }
}

float animatedX(float x, float progress) {
    return kPanelWidth * 0.5f + (x - kPanelWidth * 0.5f) * progress;
}

float animatedY(float y, float progress) {
    return kPanelHeight * 0.5f + (y - kPanelHeight * 0.5f) * progress;
}

float animatedW(float width, float progress) {
    return width * progress;
}

float animatedH(float height, float progress) {
    return height * progress;
}

void drawRectRaw(float x,
                 float y,
                 float width,
                 float height,
                 float radius,
                 const core::Color& color,
                 const core::Border& border = {},
                 const core::Shadow& shadow = {},
                 const core::Gradient& gradient = {},
                 float opacity = 1.0f,
                 float blur = 0.0f,
                 core::Transform transform = {}) {
    if (width <= 0.0f || height <= 0.0f || opacity <= 0.001f || color.a <= 0.001f) {
        return;
    }

    core::RoundedRectPrimitive primitive;
    primitive.initialize();
    primitive.setBounds(x, y, width, height);
    primitive.setColor(color);
    primitive.setGradient(gradient);
    primitive.setCornerRadius(radius);
    primitive.setBorder(border);
    primitive.setShadow(shadow);
    primitive.setOpacity(opacity);
    primitive.setBlur(blur);
    primitive.setTransform(transform);
    primitive.render(kPanelWidth, kPanelHeight);
}

core::TransformMatrix matrixForTransform(const core::Rect& frame, const core::Transform& transform) {
    const core::Vec2 origin = {
        frame.x + frame.width * transform.origin.x,
        frame.y + frame.height * transform.origin.y
    };
    const float cosX = std::cos(transform.rotateX);
    const float sinX = std::sin(transform.rotateX);
    const float cosY = std::cos(transform.rotateY);
    const float sinY = std::sin(transform.rotateY);
    const float cosZ = std::cos(transform.rotate);
    const float sinZ = std::sin(transform.rotate);
    const float scaleX = transform.scale.x;
    const float scaleY = transform.scale.y;

    const float xFromX = scaleX * cosY;
    const float xFromY = scaleY * sinX * sinY;
    const float yFromY = scaleY * cosX;
    const float zFromX = -scaleX * sinY;
    const float zFromY = scaleY * sinX * cosY;

    const float ax = xFromX * cosZ;
    const float bx = xFromY * cosZ - yFromY * sinZ;
    const float ay = xFromX * sinZ;
    const float by = xFromY * sinZ + yFromY * cosZ;
    const float az = zFromX;
    const float bz = zFromY;

    if (transform.perspective <= 0.0001f) {
        return {
            ax,
            bx,
            origin.x + transform.translate.x - ax * origin.x - bx * origin.y,
            ay,
            by,
            origin.y + transform.translate.y - ay * origin.x - by * origin.y,
            0.0f,
            0.0f,
            1.0f
        };
    }

    const float perspective = std::max(1.0f, transform.perspective);
    const float tx = transform.translate.x;
    const float ty = transform.translate.y;
    const float tz = transform.translateZ;
    const float nxDx = perspective * ax - origin.x * az;
    const float nxDy = perspective * bx - origin.x * bz;
    const float nyDx = perspective * ay - origin.y * az;
    const float nyDy = perspective * by - origin.y * bz;

    return {
        nxDx,
        nxDy,
        perspective * (origin.x + tx) - origin.x * tz - nxDx * origin.x - nxDy * origin.y,
        nyDx,
        nyDy,
        perspective * (origin.y + ty) - origin.y * tz - nyDx * origin.x - nyDy * origin.y,
        -az,
        -bz,
        perspective - tz + az * origin.x + bz * origin.y
    };
}

void drawRectMatrix(float x,
                    float y,
                    float width,
                    float height,
                    float radius,
                    const core::Color& color,
                    const core::TransformMatrix& matrix,
                    const core::Border& border = {},
                    const core::Shadow& shadow = {},
                    const core::Gradient& gradient = {},
                    float opacity = 1.0f,
                    float blur = 0.0f) {
    if (width <= 0.0f || height <= 0.0f || opacity <= 0.001f || color.a <= 0.001f) {
        return;
    }

    core::RoundedRectPrimitive primitive;
    primitive.initialize();
    primitive.setBounds(x, y, width, height);
    primitive.setColor(color);
    primitive.setGradient(gradient);
    primitive.setCornerRadius(radius);
    primitive.setBorder(border);
    primitive.setShadow(shadow);
    primitive.setOpacity(opacity);
    primitive.setBlur(blur);
    primitive.setTransformMatrix(matrix);
    primitive.render(kPanelWidth, kPanelHeight);
}

void drawRect(float x,
              float y,
              float width,
              float height,
              float radius,
              const core::Color& color,
              const core::Border& border = {},
              const core::Shadow& shadow = {},
              const core::Gradient& gradient = {},
              float opacity = 1.0f,
              float blur = 0.0f) {
    drawRectRaw(x, y, width, height, radius, color, border, shadow, gradient, opacity, blur);
}

void drawIntroRect(float x,
                   float y,
                   float width,
                   float height,
                   float radius,
                   const core::Color& color,
                   float intro,
                   const core::Border& border = {},
                   const core::Shadow& shadow = {},
                   const core::Gradient& gradient = {},
                   float opacity = 1.0f,
                   float blur = 0.0f) {
    drawRect(animatedX(x, intro),
             animatedY(y, intro),
             animatedW(width, intro),
             animatedH(height, intro),
             radius * intro,
             color,
             border,
             shadow,
             gradient,
             opacity * clamp01(intro),
             blur);
}

void drawAccentDot(float cx, float cy, float radius, const core::Color& color, float opacity = 1.0f) {
    drawRect(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, radius, color, {}, {}, {}, opacity);
}

void drawLine(float x1, float y1, float x2, float y2, float thickness, const core::Color& color, float opacity = 1.0f) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.001f) {
        return;
    }

    core::Transform transform;
    transform.rotate = std::atan2(dy, dx);
    transform.origin = {0.0f, 0.5f};
    drawRectRaw(x1,
                y1 - thickness * 0.5f,
                length,
                thickness,
                thickness * 0.5f,
                color,
                {},
                {},
                {},
                opacity,
                0.0f,
                transform);
}

void drawPolygonAbs(const std::array<core::Vec2, 4>& points, const core::Color& color, float opacity = 1.0f) {
    if (opacity <= 0.001f || color.a <= 0.001f) {
        return;
    }

    float minX = points[0].x;
    float minY = points[0].y;
    float maxX = points[0].x;
    float maxY = points[0].y;
    for (const core::Vec2& point : points) {
        minX = std::min(minX, point.x);
        minY = std::min(minY, point.y);
        maxX = std::max(maxX, point.x);
        maxY = std::max(maxY, point.y);
    }
    if (maxX <= minX || maxY <= minY) {
        return;
    }

    std::vector<core::Vec2> relative;
    relative.reserve(points.size());
    for (const core::Vec2& point : points) {
        relative.push_back({point.x - minX, point.y - minY});
    }

    core::PolygonPrimitive primitive;
    primitive.initialize();
    primitive.setBounds(minX, minY, maxX - minX, maxY - minY);
    primitive.setPoints(relative);
    primitive.setColor(color);
    primitive.setOpacity(opacity);
    primitive.render(kPanelWidth, kPanelHeight);
}

int glyphIndex(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'z') {
        ch = static_cast<char>(ch - 'a' + 'A');
    }
    if (ch >= 'A' && ch <= 'Z') {
        return 10 + (ch - 'A');
    }
    if (ch == '.') {
        return 36;
    }
    if (ch == '/') {
        return 37;
    }
    if (ch == '-') {
        return 38;
    }
    if (ch == ':') {
        return 39;
    }
    if (ch == '%') {
        return 40;
    }
    if (ch == ' ') {
        return 41;
    }
    if (ch == '_') {
        return 42;
    }
    return 41;
}

struct FontGlyph {
    core::render::RenderBackend::TextureHandle handle = nullptr;
    int width = 0;
    int height = 0;
    float xOffset = 0.0f;
    float yOffset = 0.0f;
    float advance = 0.0f;
};

struct FontRuntime {
    bool attempted = false;
    bool ready = false;
    stbtt_fontinfo font{};
    std::vector<unsigned char> systemFontBytes;
    const unsigned char* fontData = nullptr;
    std::size_t fontDataSize = 0;
    int fontIndex = 0;
    std::unordered_map<std::uint64_t, FontGlyph> glyphs;
};

FontRuntime& fontRuntime() {
    static FontRuntime runtime;
    return runtime;
}

void destroyFontTextures(core::render::RenderBackend& backend) {
    FontRuntime& runtime = fontRuntime();
    for (auto& entry : runtime.glyphs) {
        if (entry.second.handle != nullptr) {
            backend.destroyTexture(entry.second.handle);
            entry.second.handle = nullptr;
        }
    }
    runtime.glyphs.clear();
}

std::vector<unsigned char> readBinaryFile(const char* path) {
    std::vector<unsigned char> bytes;
    if (path == nullptr) {
        return bytes;
    }

    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return bytes;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return bytes;
    }
    const long size = std::ftell(file);
    if (size <= 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return bytes;
    }

    bytes.resize(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(bytes.data(), 1u, bytes.size(), file);
    std::fclose(file);
    if (read != bytes.size()) {
        bytes.clear();
    }
    return bytes;
}

bool tryInitFont(FontRuntime& runtime,
                 const unsigned char* bytes,
                 std::size_t size,
                 int index,
                 const char* sourceName,
                 bool requireCjk) {
    if (bytes == nullptr || size == 0) {
        return false;
    }
    const int offset = stbtt_GetFontOffsetForIndex(bytes, index);
    if (offset < 0 || stbtt_InitFont(&runtime.font, bytes, offset) == 0) {
        return false;
    }
    if (requireCjk &&
        (stbtt_FindGlyphIndex(&runtime.font, 0x4E2D) == 0 ||
         stbtt_FindGlyphIndex(&runtime.font, 0x6587) == 0)) {
        return false;
    }

    runtime.fontData = bytes;
    runtime.fontDataSize = size;
    runtime.fontIndex = index;
    __android_log_print(ANDROID_LOG_INFO, "NeoPanel", "UI font: %s index=%d", sourceName, index);
    return true;
}

std::vector<std::string> systemFontCandidatesFromXml(const char* path) {
    std::vector<std::string> candidates;
    std::vector<unsigned char> bytes = readBinaryFile(path);
    if (bytes.empty()) {
        return candidates;
    }
    const std::string xml(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::size_t pos = 0;
    while ((pos = xml.find("<font", pos)) != std::string::npos) {
        const std::size_t close = xml.find("</font>", pos);
        if (close == std::string::npos) {
            break;
        }
        const std::size_t nameStart = xml.find('>', pos);
        if (nameStart == std::string::npos || nameStart >= close) {
            pos = close + 7u;
            continue;
        }
        std::string name = xml.substr(nameStart + 1u, close - nameStart - 1u);
        const std::size_t first = name.find_first_not_of(" \t\r\n");
        const std::size_t last = name.find_last_not_of(" \t\r\n");
        if (first == std::string::npos || last == std::string::npos) {
            pos = close + 7u;
            continue;
        }
        name = name.substr(first, last - first + 1u);
        if (name.find(".ttf") != std::string::npos ||
            name.find(".ttc") != std::string::npos ||
            name.find(".otf") != std::string::npos) {
            const bool likelyCjk = name.find("CJK") != std::string::npos ||
                                   name.find("MiSans") != std::string::npos ||
                                   name.find("DroidSansFallback") != std::string::npos ||
                                   name.find("NotoSansSC") != std::string::npos ||
                                   name.find("NotoSerifCJK") != std::string::npos ||
                                   name.find("NotoSansHans") != std::string::npos;
            if (likelyCjk) {
                candidates.push_back(std::string("/system/fonts/") + name);
            }
        }
        pos = close + 7u;
    }
    return candidates;
}

bool ensureFontRuntime() {
    FontRuntime& runtime = fontRuntime();
    if (runtime.attempted) {
        return runtime.ready;
    }
    runtime.attempted = true;

    std::vector<std::string> candidates;
    const std::array<const char*, 2> fontXmlFiles{{
        "/system/etc/fonts.xml",
        "/system/etc/fallback_fonts.xml",
    }};
    for (const char* xmlPath : fontXmlFiles) {
        std::vector<std::string> parsed = systemFontCandidatesFromXml(xmlPath);
        candidates.insert(candidates.end(), parsed.begin(), parsed.end());
    }
    const std::array<const char*, 14> directCandidates{{
        "/system/fonts/MiSansVF.ttf",
        "/product/fonts/MiSansVF.ttf",
        "/system/fonts/MiSans-Regular.ttf",
        "/product/fonts/MiSans-Regular.ttf",
        "/system/fonts/MiSans-Semibold.ttf",
        "/system/fonts/DroidSansFallback.ttf",
        "/system/fonts/NotoSansCJK-Regular.ttf",
        "/system/fonts/NotoSansCJK-Regular.ttc",
        "/system/fonts/NotoSansCJKsc-Regular.otf",
        "/system/fonts/NotoSansSC-Regular.otf",
        "/system/fonts/NotoSerifCJK-Regular.ttc",
        "/system/fonts/SourceHanSansCN-Regular.otf",
        "/system/fonts/SourceHanSansCN-Regular.ttf",
        "/system/fonts/Roboto-Regular.ttf",
    }};
    for (const char* candidate : directCandidates) {
        candidates.emplace_back(candidate);
    }

    for (const std::string& candidate : candidates) {
        std::vector<unsigned char> bytes = readBinaryFile(candidate.c_str());
        if (bytes.empty()) {
            continue;
        }
        for (int index = 0; index < 8; ++index) {
            if (tryInitFont(runtime, bytes.data(), bytes.size(), index, candidate.c_str(), true)) {
                runtime.systemFontBytes = std::move(bytes);
                runtime.ready = tryInitFont(runtime,
                                            runtime.systemFontBytes.data(),
                                            runtime.systemFontBytes.size(),
                                            index,
                                            candidate.c_str(),
                                            true);
                if (runtime.ready) {
                    return true;
                }
            }
        }
    }

    runtime.ready = tryInitFont(runtime,
                                neopanel::assets::uiFontTtf,
                                neopanel::assets::uiFontTtfSize,
                                0,
                                "embedded-ui-font",
                                false);
    if (!runtime.ready) {
        __android_log_print(ANDROID_LOG_ERROR, "NeoPanel", "Failed to initialize UI font");
    }
    return runtime.ready;
}

bool containsNonAscii(const char* text) {
    if (text == nullptr) {
        return false;
    }
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != 0; ++p) {
        if (*p >= 0x80) {
            return true;
        }
    }
    return false;
}

bool nextCodepoint(const char*& text, std::uint32_t& codepoint) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
    if (*p == 0) {
        return false;
    }
    if (*p < 0x80) {
        codepoint = *p;
        text += 1;
        return true;
    }
    if ((*p & 0xE0u) == 0xC0u && p[1] != 0) {
        codepoint = ((*p & 0x1Fu) << 6u) | (p[1] & 0x3Fu);
        text += 2;
        return true;
    }
    if ((*p & 0xF0u) == 0xE0u && p[1] != 0 && p[2] != 0) {
        codepoint = ((*p & 0x0Fu) << 12u) | ((p[1] & 0x3Fu) << 6u) | (p[2] & 0x3Fu);
        text += 3;
        return true;
    }
    if ((*p & 0xF8u) == 0xF0u && p[1] != 0 && p[2] != 0 && p[3] != 0) {
        codepoint = ((*p & 0x07u) << 18u) | ((p[1] & 0x3Fu) << 12u) | ((p[2] & 0x3Fu) << 6u) | (p[3] & 0x3Fu);
        text += 4;
        return true;
    }
    codepoint = ' ';
    text += 1;
    return true;
}

int fontPixelHeight(float scale) {
    return std::clamp(static_cast<int>(std::round(scale * 14.2f)), 13, 64);
}

float pixelTextWidth(const char* text, float scale) {
    if (text == nullptr) {
        return 0.0f;
    }
    float width = 0.0f;
    for (const char* p = text; *p != '\0'; ++p) {
        width += (*p == ' ') ? 4.0f * scale : 6.0f * scale;
    }
    return width;
}

float fontTextWidth(const char* text, float scale) {
    if (text == nullptr || !ensureFontRuntime()) {
        return pixelTextWidth(text, scale);
    }

    FontRuntime& runtime = fontRuntime();
    const int pixelHeight = fontPixelHeight(scale);
    const float fontScale = stbtt_ScaleForPixelHeight(&runtime.font, static_cast<float>(pixelHeight));
    float width = 0.0f;
    const char* p = text;
    std::uint32_t codepoint = 0;
    while (nextCodepoint(p, codepoint)) {
        if (codepoint == ' ') {
            width += static_cast<float>(pixelHeight) * 0.30f;
            continue;
        }
        int advanceWidth = 0;
        int leftSideBearing = 0;
        stbtt_GetCodepointHMetrics(&runtime.font, static_cast<int>(codepoint), &advanceWidth, &leftSideBearing);
        width += std::max(static_cast<float>(pixelHeight) * 0.30f, static_cast<float>(advanceWidth) * fontScale * 0.94f);
    }
    return width;
}

float textWidth(const char* text, float scale) {
    return ensureFontRuntime() ? fontTextWidth(text, scale) : pixelTextWidth(text, scale);
}

void drawTextureQuad(core::render::RenderBackend::TextureHandle handle,
                     float x,
                     float y,
                     float width,
                     float height,
                     const core::Color& tint,
                     float radius = 0.0f) {
    if (handle == nullptr || width <= 0.0f || height <= 0.0f || tint.a <= 0.001f) {
        return;
    }
    core::render::RenderBackend* backend = core::render::activeRenderBackend();
    if (backend == nullptr) {
        return;
    }
    const float positions[4][3] = {
        {x, y, 1.0f},
        {x + width, y, 1.0f},
        {x + width, y + height, 1.0f},
        {x, y + height, 1.0f},
    };
    const float locals[4][2] = {
        {x, y},
        {x + width, y},
        {x + width, y + height},
        {x, y + height},
    };
    constexpr float uvs[4][2] = {
        {0.0f, 0.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f},
        {0.0f, 1.0f},
    };
    constexpr int order[6] = {0, 1, 2, 0, 2, 3};
    std::array<float, 42> vertices{};
    for (int i = 0; i < 6; ++i) {
        const int index = order[i];
        const int offset = i * 7;
        vertices[static_cast<std::size_t>(offset + 0)] = positions[index][0];
        vertices[static_cast<std::size_t>(offset + 1)] = positions[index][1];
        vertices[static_cast<std::size_t>(offset + 2)] = positions[index][2];
        vertices[static_cast<std::size_t>(offset + 3)] = locals[index][0];
        vertices[static_cast<std::size_t>(offset + 4)] = locals[index][1];
        vertices[static_cast<std::size_t>(offset + 5)] = uvs[index][0];
        vertices[static_cast<std::size_t>(offset + 6)] = uvs[index][1];
    }
    backend->drawTexture(handle, vertices.data(), vertices.size(), tint, {x, y, width, height}, radius, kPanelWidth, kPanelHeight);
}

FontGlyph* ensureFontGlyph(std::uint32_t codepoint, int pixelHeight) {
    if (!ensureFontRuntime()) {
        return nullptr;
    }
    FontRuntime& runtime = fontRuntime();
    const std::uint64_t key = (static_cast<std::uint64_t>(pixelHeight) << 32u) | static_cast<std::uint64_t>(codepoint);
    if (auto it = runtime.glyphs.find(key); it != runtime.glyphs.end()) {
        return &it->second;
    }

    const float fontScale = stbtt_ScaleForPixelHeight(&runtime.font, static_cast<float>(pixelHeight));
    int advanceWidth = 0;
    int leftSideBearing = 0;
    stbtt_GetCodepointHMetrics(&runtime.font, static_cast<int>(codepoint), &advanceWidth, &leftSideBearing);

    int width = 0;
    int height = 0;
    int xOffset = 0;
    int yOffset = 0;
    unsigned char* bitmap = stbtt_GetCodepointBitmap(&runtime.font,
                                                     0.0f,
                                                     fontScale,
                                                     static_cast<int>(codepoint),
                                                     &width,
                                                     &height,
                                                     &xOffset,
                                                     &yOffset);

    FontGlyph glyph;
    glyph.width = width;
    glyph.height = height;
    glyph.xOffset = static_cast<float>(xOffset);
    glyph.yOffset = static_cast<float>(yOffset);
    glyph.advance = std::max(static_cast<float>(pixelHeight) * 0.30f, static_cast<float>(advanceWidth) * fontScale * 0.94f);

    if (bitmap != nullptr && width > 0 && height > 0) {
        std::vector<unsigned char> rgbaPixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
        for (int i = 0; i < width * height; ++i) {
            const unsigned char alpha = bitmap[i];
            const std::size_t offset = static_cast<std::size_t>(i) * 4u;
            rgbaPixels[offset + 0] = 255;
            rgbaPixels[offset + 1] = 255;
            rgbaPixels[offset + 2] = 255;
            rgbaPixels[offset + 3] = alpha;
        }
        if (core::render::RenderBackend* backend = core::render::activeRenderBackend()) {
            glyph.handle = backend->createTexture(rgbaPixels.data(), width, height);
        }
    }
    if (bitmap != nullptr) {
        stbtt_FreeBitmap(bitmap, nullptr);
    }

    auto [it, inserted] = runtime.glyphs.emplace(key, glyph);
    (void)inserted;
    return &it->second;
}

void drawFontText(float x, float y, const char* text, float scale, const core::Color& color, float opacity) {
    if (text == nullptr || opacity <= 0.001f || !ensureFontRuntime()) {
        return;
    }

    FontRuntime& runtime = fontRuntime();
    const int pixelHeight = fontPixelHeight(scale);
    const float fontScale = stbtt_ScaleForPixelHeight(&runtime.font, static_cast<float>(pixelHeight));
    int ascent = 0;
    int descent = 0;
    int lineGap = 0;
    stbtt_GetFontVMetrics(&runtime.font, &ascent, &descent, &lineGap);
    const float baseline = y + static_cast<float>(ascent) * fontScale;

    float cursor = x;
    const char* p = text;
    std::uint32_t codepoint = 0;
    while (nextCodepoint(p, codepoint)) {
        if (codepoint == ' ') {
            cursor += static_cast<float>(pixelHeight) * 0.30f;
            continue;
        }
        FontGlyph* glyph = ensureFontGlyph(codepoint, pixelHeight);
        if (glyph == nullptr) {
            cursor += static_cast<float>(pixelHeight) * 0.50f;
            continue;
        }
        if (glyph->handle != nullptr && glyph->width > 0 && glyph->height > 0) {
            drawTextureQuad(glyph->handle,
                            cursor + glyph->xOffset,
                            baseline + glyph->yOffset,
                            static_cast<float>(glyph->width),
                            static_cast<float>(glyph->height),
                            rgba(color.r, color.g, color.b, color.a * opacity));
        }
        cursor += glyph->advance;
    }
}

void drawText(float x, float y, const char* text, float scale, const core::Color& color, float opacity = 1.0f) {
    if (text == nullptr || opacity <= 0.001f) {
        return;
    }
    if (ensureFontRuntime()) {
        drawFontText(x, y, text, scale, color, opacity);
        return;
    }

    float cursor = x;
    for (const char* p = text; *p != '\0'; ++p) {
        const char ch = *p;
        if (ch == ' ') {
            cursor += 4.0f * scale;
            continue;
        }

        const auto& columns = kGlyphs[static_cast<std::size_t>(glyphIndex(ch))];
        for (int col = 0; col < 5; ++col) {
            for (int row = 0; row < 8; ++row) {
                if ((columns[static_cast<std::size_t>(col)] & (1u << row)) == 0) {
                    continue;
                }
                drawRect(cursor + static_cast<float>(col) * scale,
                         y + static_cast<float>(row) * scale,
                         scale,
                         scale,
                         std::max(1.0f, scale * 0.22f),
                         color,
                         {},
                         {},
                         {},
                         opacity);
            }
        }
        cursor += 6.0f * scale;
    }
}

void drawTextFit(float x,
                 float y,
                 float maxWidth,
                 const char* text,
                 float scale,
                 const core::Color& color,
                 float opacity = 1.0f,
                 float minScale = 0.92f) {
    if (text == nullptr || maxWidth <= 0.0f || opacity <= 0.001f) {
        return;
    }
    const float width = textWidth(text, scale);
    const float fitted = width > maxWidth ? std::max(minScale, scale * maxWidth / std::max(1.0f, width)) : scale;
    drawText(x, y, text, fitted, color, opacity);
}

void drawTextCenteredFit(float centerX,
                         float y,
                         float maxWidth,
                         const char* text,
                         float scale,
                         const core::Color& color,
                         float opacity = 1.0f,
                         float minScale = 0.92f) {
    const float width = textWidth(text, scale);
    const float fitted = width > maxWidth ? std::max(minScale, scale * maxWidth / std::max(1.0f, width)) : scale;
    drawText(centerX - textWidth(text, fitted) * 0.5f, y, text, fitted, color, opacity);
}

void drawTextCentered(float centerX, float y, const char* text, float scale, const core::Color& color, float opacity = 1.0f) {
    drawText(centerX - textWidth(text, scale) * 0.5f, y, text, scale, color, opacity);
}

void drawTextRight(float rightX, float y, const char* text, float scale, const core::Color& color, float opacity = 1.0f) {
    drawText(rightX - textWidth(text, scale), y, text, scale, color, opacity);
}

bool decodeAvatar(EmbeddedTexture& texture) {
    if (texture.decoded) {
        return !texture.pixels.empty();
    }
    texture.decoded = true;

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(neopanel::assets::avatarPng,
                                            static_cast<int>(neopanel::assets::avatarPngSize),
                                            &width,
                                            &height,
                                            &channels,
                                            STBI_rgb_alpha);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        if (pixels != nullptr) {
            stbi_image_free(pixels);
        }
        __android_log_print(ANDROID_LOG_ERROR, "NeoPanel", "Failed to decode embedded avatar");
        return false;
    }

    texture.width = width;
    texture.height = height;
    texture.pixels.assign(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u);
    stbi_image_free(pixels);
    return true;
}

bool ensureTextureUploaded(EmbeddedTexture& texture) {
    if (texture.uploaded && texture.handle != nullptr) {
        return true;
    }
    if (!decodeAvatar(texture)) {
        return false;
    }

    core::render::RenderBackend* backend = core::render::activeRenderBackend();
    if (backend == nullptr) {
        return false;
    }
    texture.handle = backend->createTexture(texture.pixels.data(), texture.width, texture.height);
    texture.uploaded = texture.handle != nullptr;
    if (texture.uploaded) {
        texture.pixels.clear();
        texture.pixels.shrink_to_fit();
    }
    return texture.uploaded;
}

void rebuildImageVertices(float x,
                          float y,
                          float width,
                          float height,
                          int textureWidth,
                          int textureHeight,
                          std::array<float, 42>& vertices) {
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;

    if (textureWidth > 0 && textureHeight > 0 && width > 0.0f && height > 0.0f) {
        const float rectAspect = width / height;
        const float imageAspect = static_cast<float>(textureWidth) / static_cast<float>(textureHeight);
        if (imageAspect > rectAspect) {
            const float visible = std::clamp(rectAspect / imageAspect, 0.0f, 1.0f);
            u0 = (1.0f - visible) * 0.5f;
            u1 = 1.0f - u0;
        } else if (imageAspect < rectAspect) {
            const float visible = std::clamp(imageAspect / rectAspect, 0.0f, 1.0f);
            v0 = (1.0f - visible) * 0.5f;
            v1 = 1.0f - v0;
        }
    }

    const float positions[4][3] = {
        {x, y, 1.0f},
        {x + width, y, 1.0f},
        {x + width, y + height, 1.0f},
        {x, y + height, 1.0f},
    };
    const float locals[4][2] = {
        {x, y},
        {x + width, y},
        {x + width, y + height},
        {x, y + height},
    };
    const float uvs[4][2] = {
        {u0, v0},
        {u1, v0},
        {u1, v1},
        {u0, v1},
    };
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; ++i) {
        const int index = order[i];
        const int offset = i * 7;
        vertices[static_cast<std::size_t>(offset + 0)] = positions[index][0];
        vertices[static_cast<std::size_t>(offset + 1)] = positions[index][1];
        vertices[static_cast<std::size_t>(offset + 2)] = positions[index][2];
        vertices[static_cast<std::size_t>(offset + 3)] = locals[index][0];
        vertices[static_cast<std::size_t>(offset + 4)] = locals[index][1];
        vertices[static_cast<std::size_t>(offset + 5)] = uvs[index][0];
        vertices[static_cast<std::size_t>(offset + 6)] = uvs[index][1];
    }
}

void drawEmbeddedAvatar(PanelState& state, float x, float y, float width, float height, float radius, float opacity) {
    if (!ensureTextureUploaded(state.avatar)) {
        drawRect(x, y, width, height, radius, rgba(1.0f, 0.44f, 0.54f, 0.18f));
        drawTextCentered(x + width * 0.5f, y + height * 0.5f - 9.0f, "IMG", 2.4f, rgba(1.0f, 0.76f, 0.80f, 1.0f), opacity);
        return;
    }

    std::array<float, 42> vertices{};
    rebuildImageVertices(x, y, width, height, state.avatar.width, state.avatar.height, vertices);
    core::render::RenderBackend* backend = core::render::activeRenderBackend();
    if (backend == nullptr) {
        return;
    }
    backend->drawTexture(state.avatar.handle,
                         vertices.data(),
                         vertices.size(),
                         rgba(1.0f, 1.0f, 1.0f, opacity),
                         {x, y, width, height},
                         radius,
                         kPanelWidth,
                         kPanelHeight);
}

double processCpuSeconds() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0;
    }
    const double user = static_cast<double>(usage.ru_utime.tv_sec) + static_cast<double>(usage.ru_utime.tv_usec) / 1000000.0;
    const double system = static_cast<double>(usage.ru_stime.tv_sec) + static_cast<double>(usage.ru_stime.tv_usec) / 1000000.0;
    return user + system;
}

float processMemoryMb() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0.0f;
    }
#if defined(__APPLE__)
    return static_cast<float>(usage.ru_maxrss) / (1024.0f * 1024.0f);
#else
    return static_cast<float>(usage.ru_maxrss) / 1024.0f;
#endif
}

float systemLoadPercent() {
    struct sysinfo info {};
    if (sysinfo(&info) != 0) {
        return 0.0f;
    }
    return std::clamp(static_cast<float>(info.loads[0]) / 65536.0f * 18.0f, 0.0f, 100.0f);
}

void updateRuntimeStats(PanelState& state, float deltaSeconds, double now) {
    RuntimeStats& stats = state.stats;
    stats.frameAccumulator += deltaSeconds;
    ++stats.frameCount;

    const bool first = stats.previousSampleTime <= 0.0;
    if (first) {
        stats.previousSampleTime = now;
        stats.previousCpuTime = processCpuSeconds();
    }

    const double elapsed = now - stats.previousSampleTime;
    if (first || elapsed >= 0.30) {
        const double cpuNow = processCpuSeconds();
        const double cpuDelta = std::max(0.0, cpuNow - stats.previousCpuTime);
        const float averageFrame = stats.frameCount > 0
            ? stats.frameAccumulator / static_cast<float>(stats.frameCount)
            : deltaSeconds;

        stats.fps = averageFrame > 0.0001f ? 1.0f / averageFrame : 0.0f;
        stats.frameMs = averageFrame * 1000.0f;
        stats.cpuPercent = static_cast<float>(std::clamp(cpuDelta / std::max(0.001, elapsed) * 100.0, 0.0, 100.0));
        stats.memoryMb = processMemoryMb();
        stats.loadPercent = systemLoadPercent();
        stats.previousSampleTime = now;
        stats.previousCpuTime = cpuNow;
        stats.frameAccumulator = 0.0f;
        stats.frameCount = 0;

        state.fpsSamples[static_cast<std::size_t>(state.graphCursor)] = std::clamp(stats.fps / 120.0f, 0.0f, 1.0f);
        state.cpuSamples[static_cast<std::size_t>(state.graphCursor)] = std::clamp(stats.cpuPercent / 100.0f, 0.0f, 1.0f);
        state.graphCursor = (state.graphCursor + 1) % kGraphSamples;
    }
}

core::Rect navRect(int index) {
    constexpr float x = 46.0f;
    constexpr float top = 224.0f;
    constexpr float h = 72.0f;
    constexpr float gap = 18.0f;
    return {x, top + static_cast<float>(index) * (h + gap), 170.0f, h};
}

core::Color pageAccent(int page) {
    if (page == 0) {
        return rgba(1.0f, 0.42f, 0.52f, 1.0f);
    }
    if (page == 1) {
        return rgba(0.48f, 0.74f, 1.0f, 1.0f);
    }
    if (page == 2) {
        return rgba(0.72f, 0.56f, 1.0f, 1.0f);
    }
    return rgba(0.55f, 0.94f, 0.74f, 1.0f);
}

void drawIconGauge(float x, float y, float size, const core::Color& color, float opacity) {
    drawRect(x + size * 0.10f, y + size * 0.24f, size * 0.80f, size * 0.60f, size * 0.30f, rgba(color.r, color.g, color.b, 0.16f * opacity), {1.2f, rgba(color.r, color.g, color.b, 0.52f * opacity)});
    drawLine(x + size * 0.50f, y + size * 0.54f, x + size * 0.72f, y + size * 0.40f, size * 0.07f, color, opacity);
    drawAccentDot(x + size * 0.50f, y + size * 0.54f, size * 0.075f, color, opacity);
    for (int i = 0; i < 5; ++i) {
        const float t = static_cast<float>(i) / 4.0f;
        drawAccentDot(x + size * (0.22f + t * 0.56f), y + size * (0.66f - std::sin(t * 3.14159f) * 0.27f), size * 0.030f, color, 0.60f * opacity);
    }
}

void drawIconChart(float x, float y, float size, const core::Color& color, float opacity) {
    drawRect(x + size * 0.13f, y + size * 0.15f, size * 0.74f, size * 0.70f, size * 0.16f, rgba(color.r, color.g, color.b, 0.12f * opacity), {1.1f, rgba(color.r, color.g, color.b, 0.44f * opacity)});
    const float base = y + size * 0.75f;
    for (int i = 0; i < 4; ++i) {
        const float h = size * (0.18f + static_cast<float>((i * 7) % 5) * 0.055f);
        drawRect(x + size * (0.25f + i * 0.13f), base - h, size * 0.075f, h, size * 0.035f, color, {}, {}, {}, (0.58f + i * 0.10f) * opacity);
    }
    drawLine(x + size * 0.24f, y + size * 0.56f, x + size * 0.46f, y + size * 0.42f, size * 0.035f, color, 0.75f * opacity);
    drawLine(x + size * 0.46f, y + size * 0.42f, x + size * 0.70f, y + size * 0.30f, size * 0.035f, color, 0.75f * opacity);
}

void drawIconSpark(float x, float y, float size, const core::Color& color, float opacity) {
    drawAccentDot(x + size * 0.52f, y + size * 0.52f, size * 0.12f, color, 0.92f * opacity);
    drawLine(x + size * 0.52f, y + size * 0.16f, x + size * 0.52f, y + size * 0.36f, size * 0.055f, color, opacity);
    drawLine(x + size * 0.52f, y + size * 0.68f, x + size * 0.52f, y + size * 0.88f, size * 0.055f, color, opacity);
    drawLine(x + size * 0.16f, y + size * 0.52f, x + size * 0.36f, y + size * 0.52f, size * 0.055f, color, opacity);
    drawLine(x + size * 0.68f, y + size * 0.52f, x + size * 0.88f, y + size * 0.52f, size * 0.055f, color, opacity);
    drawLine(x + size * 0.28f, y + size * 0.28f, x + size * 0.40f, y + size * 0.40f, size * 0.045f, color, 0.70f * opacity);
    drawLine(x + size * 0.72f, y + size * 0.28f, x + size * 0.62f, y + size * 0.40f, size * 0.045f, color, 0.70f * opacity);
    drawLine(x + size * 0.72f, y + size * 0.72f, x + size * 0.62f, y + size * 0.62f, size * 0.045f, color, 0.70f * opacity);
    drawLine(x + size * 0.28f, y + size * 0.72f, x + size * 0.40f, y + size * 0.62f, size * 0.045f, color, 0.70f * opacity);
}

void drawIconChip(float x, float y, float size, const core::Color& color, float opacity) {
    drawRect(x + size * 0.25f, y + size * 0.25f, size * 0.50f, size * 0.50f, size * 0.11f, rgba(color.r, color.g, color.b, 0.18f * opacity), {1.2f, rgba(color.r, color.g, color.b, 0.55f * opacity)});
    drawRect(x + size * 0.37f, y + size * 0.37f, size * 0.26f, size * 0.26f, size * 0.06f, color, {}, {}, {}, 0.38f * opacity);
    for (int i = 0; i < 4; ++i) {
        const float p = size * (0.30f + i * 0.13f);
        drawRect(x + p, y + size * 0.13f, size * 0.035f, size * 0.12f, size * 0.016f, color, {}, {}, {}, 0.78f * opacity);
        drawRect(x + p, y + size * 0.75f, size * 0.035f, size * 0.12f, size * 0.016f, color, {}, {}, {}, 0.78f * opacity);
        drawRect(x + size * 0.13f, y + p, size * 0.12f, size * 0.035f, size * 0.016f, color, {}, {}, {}, 0.78f * opacity);
        drawRect(x + size * 0.75f, y + p, size * 0.12f, size * 0.035f, size * 0.016f, color, {}, {}, {}, 0.78f * opacity);
    }
}

void drawNavIcon(int icon, float x, float y, float size, const core::Color& color, float opacity) {
    if (icon == 0) {
        drawIconGauge(x, y, size, color, opacity);
    } else if (icon == 1) {
        drawIconChart(x, y, size, color, opacity);
    } else if (icon == 2) {
        drawIconSpark(x, y, size, color, opacity);
    } else {
        drawIconChip(x, y, size, color, opacity);
    }
}

void renderAvatar(PanelState& state, float intro, float opacity) {
    drawIntroRect(46.0f, 42.0f, 170.0f, 154.0f, 38.0f,
                  rgba(1.0f, 1.0f, 1.0f, 0.10f),
                  intro,
                  {1.4f, rgba(1.0f, 1.0f, 1.0f, 0.26f)},
                  {true, {0.0f, 14.0f}, 34.0f, 0.0f, rgba(0.12f, 0.04f, 0.08f, 0.26f)});

    const float ix = animatedX(55.0f, intro);
    const float iy = animatedY(51.0f, intro);
    const float iw = animatedW(152.0f, intro);
    const float ih = animatedH(136.0f, intro);
    if (intro > 0.05f) {
        drawEmbeddedAvatar(state, ix, iy, iw, ih, 32.0f * intro, opacity);
    }

    drawRect(58.0f, 154.0f, 146.0f, 28.0f, 14.0f,
             rgba(0.08f, 0.06f, 0.08f, 0.58f * opacity),
             {1.0f, rgba(1.0f, 0.78f, 0.80f, 0.26f * opacity)});
    drawTextFit(82.0f, 160.0f, 78.0f, tr("READY", "就绪"), 1.20f, rgba(1.0f, 0.74f, 0.78f, 1.0f), opacity, 0.82f);
    drawAccentDot(178.0f, 168.0f, 4.0f, rgba(0.48f, 1.0f, 0.72f, 0.82f), opacity);
}

void renderNavigation(PanelState& state, float opacity) {
    for (int i = 0; i < 4; ++i) {
        const core::Rect r = navRect(i);
        const float distance = std::fabs(state.navBlend - static_cast<float>(i));
        const float active = clamp01(1.0f - distance);
        const float pressed = i == state.pressedNav ? 1.0f : 0.0f;
        const core::Color accent = pageAccent(i);
        const core::Color base = mix(rgba(1.0f, 1.0f, 1.0f, 0.075f * opacity), rgba(accent.r, accent.g, accent.b, 0.22f * opacity), active);
        const core::Color border = mix(rgba(1.0f, 1.0f, 1.0f, 0.15f * opacity), rgba(accent.r, accent.g, accent.b, 0.62f * opacity), active);
        const float yPulse = pressed > 0.0f ? 2.0f : 0.0f;
        const float scale = pressed > 0.0f ? 0.985f : 1.0f + active * 0.012f;

        core::Transform transform;
        transform.origin = {0.5f, 0.5f};
        transform.scale = {scale, scale};
        drawRectRaw(r.x, r.y + yPulse, r.width, r.height, 24.0f,
                    base,
                    {1.2f, border},
                    {true, {0.0f, 10.0f}, active > 0.0f ? 28.0f : 18.0f, 0.0f, rgba(0.10f, 0.03f, 0.07f, active > 0.0f ? 0.28f : 0.15f)},
                    {},
                    opacity,
                    0.0f,
                    transform);
        if (active > 0.04f) {
            drawRect(r.x + 9.0f, r.y + 13.0f, 5.0f, r.height - 26.0f, 2.5f, accent, {}, {}, {}, active * opacity);
        }

        drawNavIcon(kNavItems[static_cast<std::size_t>(i)].icon, r.x + 20.0f, r.y + 16.0f, 40.0f, mix(rgba(0.58f, 0.76f, 1.0f, 1.0f), accent, active), opacity);
        const NavItem& item = kNavItems[static_cast<std::size_t>(i)];
        drawTextFit(r.x + 72.0f, r.y + 18.0f, 78.0f, tr(item.labelEn, item.labelZh), 1.62f, textMain(), opacity, 1.08f);
        drawTextFit(r.x + 72.0f, r.y + 45.0f, 78.0f, tr(item.detailEn, item.detailZh), 1.03f, textSoft(), opacity * 0.82f, 0.82f);
    }
}

void renderLanguageToggle(const PanelState& state, float opacity) {
    const core::Rect r = languageToggleRect();
    const core::Color accent = pageAccent(0);
    const float pressed = state.pressedLanguage ? 1.0f : 0.0f;
    drawRect(r.x, r.y, r.width, r.height, r.height * 0.5f,
             gNight ? rgba(1.0f, 1.0f, 1.0f, (0.055f + pressed * 0.030f) * opacity)
                    : rgba(0.08f, 0.10f, 0.14f, (0.070f + pressed * 0.035f) * opacity),
             {1.0f, subtleBorder(opacity)});
    const float knobX = gChinese ? r.x + r.width * 0.5f + 3.0f : r.x + 3.0f;
    drawRect(knobX, r.y + 3.0f, r.width * 0.5f - 6.0f, r.height - 6.0f, (r.height - 6.0f) * 0.5f,
             rgba(accent.r, accent.g, accent.b, 0.24f * opacity),
             {1.0f, rgba(accent.r, accent.g, accent.b, 0.42f * opacity)});
    drawTextCenteredFit(r.x + r.width * 0.25f, r.y + 9.0f, 58.0f, "EN", 1.08f,
                        gChinese ? textSoft() : rgba(accent.r, accent.g, accent.b, 1.0f), opacity, 0.92f);
    drawTextCenteredFit(r.x + r.width * 0.75f, r.y + 9.0f, 62.0f, "中文", 1.08f,
                        gChinese ? rgba(accent.r, accent.g, accent.b, 1.0f) : textSoft(), opacity, 0.92f);
}

void renderThemeToggle(const PanelState& state, float opacity) {
    const core::Rect r = themeToggleRect();
    const core::Color accent = gNight ? rgba(0.72f, 0.56f, 1.0f, 1.0f) : rgba(1.0f, 0.62f, 0.28f, 1.0f);
    const float pressed = state.pressedTheme ? 1.0f : 0.0f;
    drawRect(r.x, r.y, r.width, r.height, r.width * 0.5f,
             rgba(accent.r, accent.g, accent.b, (0.12f + pressed * 0.08f) * opacity),
             {1.0f, rgba(accent.r, accent.g, accent.b, 0.36f * opacity)},
             {true, {0.0f, 8.0f}, 22.0f, 0.0f, rgba(accent.r, accent.g, accent.b, 0.18f * opacity)});

    if (gNight) {
        drawAccentDot(r.x + 21.0f, r.y + 21.0f, 11.0f, rgba(0.90f, 0.86f, 1.0f, 0.90f), opacity);
        drawAccentDot(r.x + 26.0f, r.y + 16.0f, 10.0f, rgba(0.08f, 0.06f, 0.11f, 0.96f), opacity);
        drawAccentDot(r.x + 13.0f, r.y + 12.0f, 1.8f, rgba(0.92f, 0.94f, 1.0f, 0.82f), opacity);
        drawAccentDot(r.x + 32.0f, r.y + 29.0f, 1.5f, rgba(0.92f, 0.94f, 1.0f, 0.72f), opacity);
    } else {
        drawAccentDot(r.x + 22.0f, r.y + 22.0f, 9.0f, rgba(1.0f, 0.72f, 0.28f, 0.95f), opacity);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * 0.785398f;
            drawLine(r.x + 22.0f + std::cos(a) * 14.0f,
                     r.y + 22.0f + std::sin(a) * 14.0f,
                     r.x + 22.0f + std::cos(a) * 18.0f,
                     r.y + 22.0f + std::sin(a) * 18.0f,
                     2.0f,
                     rgba(1.0f, 0.72f, 0.28f, 0.95f),
                     opacity);
        }
    }
}

void renderFpsSlider(float contentX, float contentW, float top, float opacity) {
    const core::Rect r = fpsSliderRect(contentX, contentW, top);
    const float t = std::clamp((gTargetFps - kFpsMin) / (kFpsMax - kFpsMin), 0.0f, 1.0f);
    const core::Color accent = pageAccent(1);
    char value[24]{};
    std::snprintf(value, sizeof(value), "%.0f FPS", gTargetFps);

    drawText(r.x, r.y - 20.0f, tr("FRAME RATE", "帧率选择"), 1.20f, textSoft(), opacity);
    drawTextRight(r.x + r.width, r.y - 20.0f, value, 1.35f, rgba(accent.r, accent.g, accent.b, 1.0f), opacity);
    drawRect(r.x, r.y + 16.0f, r.width, 10.0f, 5.0f, trackColor(opacity));
    drawRect(r.x, r.y + 16.0f, r.width * t, 10.0f, 5.0f, accent, {}, {}, {}, 0.78f * opacity);
    for (int i = 0; i < 4; ++i) {
        const float x = r.x + r.width * static_cast<float>(i) / 3.0f;
        drawRect(x - 1.0f, r.y + 10.0f, 2.0f, 22.0f, 1.0f,
                 gNight ? rgba(1.0f, 1.0f, 1.0f, 0.20f * opacity) : rgba(0.12f, 0.14f, 0.20f, 0.18f * opacity));
    }
    const float knobX = r.x + r.width * t;
    drawAccentDot(knobX, r.y + 21.0f, 14.0f, rgba(accent.r, accent.g, accent.b, 0.30f), opacity);
    drawAccentDot(knobX, r.y + 21.0f, 7.0f, rgba(0.96f, 0.98f, 1.0f, 0.95f), opacity);
}

void formatMetric(char* out, std::size_t size, float value, const char* suffix, int decimals = 0) {
    if (decimals == 0) {
        std::snprintf(out, size, "%.0f%s", value, suffix);
    } else {
        std::snprintf(out, size, "%.*f%s", decimals, value, suffix);
    }
}

std::array<Metric, 4> buildMetrics(const PanelState& state) {
    std::array<Metric, 4> metrics{};
    metrics[0] = {"FPS", {}, "FRAME RATE", "帧率", std::clamp(state.stats.fps / 120.0f, 0.0f, 1.0f), rgba(0.48f, 0.74f, 1.0f, 1.0f)};
    metrics[1] = {"CPU", {}, "PROCESS", "进程", std::clamp(state.stats.cpuPercent / 100.0f, 0.0f, 1.0f), rgba(1.0f, 0.50f, 0.58f, 1.0f)};
    metrics[2] = {"RAM", {}, "MAX RSS", "内存", std::clamp(state.stats.memoryMb / 512.0f, 0.0f, 1.0f), rgba(0.58f, 0.94f, 0.72f, 1.0f)};
    metrics[3] = {"MS", {}, "FRAME COST", "帧耗时", std::clamp(state.stats.frameMs / 33.3f, 0.0f, 1.0f), rgba(0.75f, 0.58f, 1.0f, 1.0f)};
    formatMetric(metrics[0].value, sizeof(metrics[0].value), state.stats.fps, "", 0);
    formatMetric(metrics[1].value, sizeof(metrics[1].value), state.stats.cpuPercent, "%", 0);
    formatMetric(metrics[2].value, sizeof(metrics[2].value), state.stats.memoryMb, "M", 0);
    formatMetric(metrics[3].value, sizeof(metrics[3].value), state.stats.frameMs, "", 1);
    return metrics;
}

void renderMetricCard(float x, float y, float width, float height, const Metric& metric, float pulse, float opacity) {
    const core::Color fill = rgba(metric.color.r, metric.color.g, metric.color.b, 0.12f + 0.05f * pulse);
    drawRect(x, y, width, height, 22.0f, fill, {1.0f, rgba(metric.color.r, metric.color.g, metric.color.b, 0.28f * opacity)}, {}, {}, opacity);
    drawTextFit(x + 16.0f, y + 10.0f, width - 32.0f, metric.label, 0.96f, tileTextSoft(), opacity, 0.72f);
    drawTextFit(x + 16.0f, y + 31.0f, width - 32.0f, metric.value, 1.62f, tileText(), opacity, 1.04f);
    drawTextFit(x + 16.0f, y + height - 30.0f, width - 32.0f, tr(metric.noteEn, metric.noteZh), 0.72f, tileTextSoft(), opacity, 0.56f);
    drawRect(x + 16.0f, y + height - 12.0f, width - 32.0f, 5.0f, 2.5f, trackColor(opacity));
    drawRect(x + 16.0f, y + height - 12.0f, (width - 32.0f) * metric.normalized, 5.0f, 2.5f, metric.color, {}, {}, {}, 0.78f * opacity);
}

void drawGraph(const std::array<float, kGraphSamples>& samples,
               int cursor,
               float x,
               float y,
               float width,
               float height,
               const core::Color& color,
               float opacity) {
    drawRect(x, y, width, height, 20.0f, softTile(opacity), {1.0f, subtleBorder(opacity)});
    for (int i = 0; i < 4; ++i) {
        const float gy = y + 16.0f + static_cast<float>(i) * (height - 32.0f) / 3.0f;
        drawRect(x + 18.0f, gy, width - 36.0f, 1.5f, 0.75f,
                 gNight ? rgba(1.0f, 1.0f, 1.0f, 0.06f * opacity) : rgba(0.12f, 0.14f, 0.20f, 0.070f * opacity));
    }

    const float left = x + 22.0f;
    const float graphW = width - 44.0f;
    const float bottom = y + height - 24.0f;
    const float graphH = height - 48.0f;
    float prevX = left;
    float prevY = bottom - samples[static_cast<std::size_t>(cursor % kGraphSamples)] * graphH;
    for (int i = 1; i < kGraphSamples; ++i) {
        const int sampleIndex = (cursor + i) % kGraphSamples;
        const float sx = left + graphW * static_cast<float>(i) / static_cast<float>(kGraphSamples - 1);
        const float sy = bottom - samples[static_cast<std::size_t>(sampleIndex)] * graphH;
        drawLine(prevX, prevY, sx, sy, 3.0f, color, opacity * 0.70f);
        drawAccentDot(sx, sy, 2.6f, color, opacity * 0.68f);
        prevX = sx;
        prevY = sy;
    }
}

bool itemVisible(float y, float height, float top, float bottom) {
    return y + height >= top - 6.0f && y <= bottom + 6.0f;
}

void drawContentFrame(float x, float y, float width, float height, const core::Color& accent, float opacity) {
    drawRect(x, y, width, height, 42.0f,
             contentFill(opacity),
             {1.2f, subtleBorder(opacity)},
             {true, {0.0f, 18.0f}, 38.0f, 0.0f, gNight ? rgba(0.05f, 0.02f, 0.04f, 0.34f * opacity) : rgba(0.26f, 0.20f, 0.25f, 0.12f * opacity)});
    drawRect(x + 1.5f, y + 1.5f, width - 3.0f, height - 3.0f, 40.0f,
             rgba(accent.r, accent.g, accent.b, 0.015f * opacity),
             {1.0f, rgba(accent.r, accent.g, accent.b, 0.12f * opacity)});
}

void drawScrollMasks(float contentX,
                     float contentY,
                     float contentW,
                     float contentH,
                     float top,
                     float bottom,
                     const core::Color& accent,
                     float opacity) {
    const core::Color panelColor = contentMask(opacity);
    if (top > contentY) {
        drawRect(contentX + 3.0f, contentY + 3.0f, contentW - 6.0f, top - contentY + 8.0f, 38.0f, panelColor);
    }
    if (bottom < contentY + contentH) {
        drawRect(contentX + 3.0f, bottom - 8.0f, contentW - 6.0f, contentY + contentH - bottom + 5.0f, 30.0f, panelColor);
    }
    drawRect(contentX, contentY, contentW, contentH, 42.0f,
             rgba(0.0f, 0.0f, 0.0f, 0.002f),
             {1.2f, subtleBorder(opacity)});
    drawRect(contentX + 2.0f, contentY + 2.0f, contentW - 4.0f, contentH - 4.0f, 40.0f,
             rgba(0.0f, 0.0f, 0.0f, 0.002f),
             {1.0f, rgba(accent.r, accent.g, accent.b, 0.10f * opacity)});
}

void renderChip(float x, float y, const char* label, const core::Color& accent, float opacity) {
    drawRect(x, y, 118.0f, 32.0f, 16.0f,
             rgba(accent.r, accent.g, accent.b, 0.13f * opacity),
             {1.0f, rgba(accent.r, accent.g, accent.b, 0.34f * opacity)});
    drawTextCenteredFit(x + 59.0f, y + 9.0f, 96.0f, label, 0.92f, rgba(accent.r, accent.g, accent.b, 1.0f), opacity, 0.70f);
}

void renderContentHeader(float contentX,
                         float contentY,
                         float contentW,
                         float scrollTop,
                         int page,
                         const core::Color& accent,
                         float opacity) {
    const PageInfo& info = kPages[static_cast<std::size_t>(page)];
    drawTextFit(contentX + 28.0f, contentY + 25.0f, contentW - 210.0f, tr(info.tagEn, info.tagZh), 1.24f, rgba(accent.r, accent.g, accent.b, 1.0f), opacity, 0.90f);
    drawTextFit(contentX + 28.0f, contentY + 54.0f, contentW - 218.0f, tr(info.titleEn, info.titleZh), 2.12f, textMain(), opacity, 1.35f);
    drawTextFit(contentX + 28.0f, contentY + 89.0f, contentW - 88.0f, tr(info.subtitleEn, info.subtitleZh), 1.03f, textSoft(), opacity, 0.82f);
    renderChip(contentX + contentW - 150.0f, contentY + 24.0f, page == 1 ? tr("LIVE DATA", "实时") : tr("ELF ONLY", "单ELF"), accent, opacity);
    drawRect(contentX + 30.0f, scrollTop - 12.0f, contentW - 60.0f, 1.5f, 0.75f,
             gNight ? rgba(1.0f, 1.0f, 1.0f, 0.070f * opacity) : rgba(0.12f, 0.14f, 0.20f, 0.080f * opacity));
}

void renderMiniSwatch(float x, float y, float width, float height, const LocalText& label, const core::Color& color, float opacity) {
    core::Gradient gradient;
    gradient.enabled = true;
    gradient.start = rgba(color.r, color.g, color.b, 0.24f * opacity);
    gradient.end = rgba(color.r * 0.72f + 0.16f, color.g * 0.72f + 0.16f, color.b * 0.72f + 0.16f, 0.08f * opacity);
    gradient.direction = core::GradientDirection::Vertical;
    drawRect(x, y, width, height, 16.0f,
             rgba(color.r, color.g, color.b, 0.12f * opacity),
             {1.0f, rgba(color.r, color.g, color.b, 0.30f * opacity)},
             {},
             gradient);
    drawRect(x + 12.0f, y + 12.0f, width - 24.0f, 26.0f, 13.0f,
             rgba(color.r, color.g, color.b, 0.78f * opacity));
    drawTextCenteredFit(x + width * 0.5f, y + height - 27.0f, width - 18.0f, tr(label), 0.98f, tileText(), opacity, 0.78f);
}

void renderControlSpec(float x,
                       float y,
                       float width,
                       const LocalText& title,
                       const LocalText& note,
                       float value,
                       const core::Color& accent,
                       float opacity) {
    drawRect(x, y, width, 70.0f, 18.0f,
             rgba(accent.r, accent.g, accent.b, 0.070f * opacity),
             {1.0f, rgba(accent.r, accent.g, accent.b, 0.18f * opacity)});
    drawTextFit(x + 16.0f, y + 13.0f, width - 160.0f, tr(title), 1.18f, tileText(), opacity, 0.86f);
    drawTextFit(x + 16.0f, y + 39.0f, width - 166.0f, tr(note), 0.92f, tileTextSoft(), opacity, 0.72f);
    drawRect(x + width - 130.0f, y + 42.0f, 82.0f, 6.0f, 3.0f, trackColor(opacity));
    drawRect(x + width - 130.0f, y + 42.0f, 82.0f * clamp01(value), 6.0f, 3.0f, accent, {}, {}, {}, 0.74f * opacity);
    drawTextRight(x + width - 18.0f, y + 36.0f, value > 0.84f ? tr("ON", "开启") : tr("OK", "就绪"), 0.94f, rgba(accent.r, accent.g, accent.b, 1.0f), opacity);
}

void renderEffectTiles(float contentX,
                       float contentW,
                       int page,
                       float yBase,
                       float scroll,
                       float top,
                       float bottom,
                       float opacity) {
    const float startY = yBase - scroll;
    const core::Color accent = pageAccent(page);
    const std::array<LocalText, 6> labels{{
        {"BLUR", "模糊"},
        {"RADIUS", "圆角"},
        {"IMAGE", "图像"},
        {"SCROLL", "滚动"},
        {"SHADOW", "阴影"},
        {"TOUCH", "触控"},
    }};
    const std::array<float, 6> values{{0.84f, 0.92f, 1.0f, 0.70f, 0.76f, 0.88f}};
    const float tileW = (contentW - 90.0f) / 3.0f;
    for (int i = 0; i < 6; ++i) {
        const float col = static_cast<float>(i % 3);
        const float row = static_cast<float>(i / 3);
        const float x = contentX + 30.0f + col * (tileW + 15.0f);
        const float y = startY + row * 94.0f;
        if (!itemVisible(y, 76.0f, top, bottom)) {
            continue;
        }
        const core::Color tileColor = mix(softTile(opacity), rgba(accent.r, accent.g, accent.b, 0.15f), values[static_cast<std::size_t>(i)]);
        drawRect(x, y, tileW, 76.0f, 18.0f, tileColor, {1.0f, subtleBorder(opacity)}, {}, {}, opacity);
        drawAccentDot(x + 20.0f, y + 22.0f, 5.0f, accent, opacity);
        drawTextFit(x + 34.0f, y + 14.0f, tileW - 50.0f, tr(labels[static_cast<std::size_t>(i)]), 1.12f, tileText(), opacity, 0.82f);
        drawRect(x + 16.0f, y + 50.0f, tileW - 44.0f, 6.0f, 3.0f, trackColor(opacity));
        drawRect(x + 16.0f, y + 50.0f, (tileW - 44.0f) * values[static_cast<std::size_t>(i)], 6.0f, 3.0f, accent, {}, {}, {}, 0.70f * opacity);
        drawTextRight(x + tileW - 14.0f, y + 43.0f, values[static_cast<std::size_t>(i)] > 0.9f ? tr("ON", "开") : tr("OK", "稳"), 0.82f, rgba(accent.r, accent.g, accent.b, 1.0f), opacity);
    }
}

void renderButtonStudy(PanelState& state, float x, float y, float width, float top, float bottom, float opacity) {
    if (!itemVisible(y, 72.0f, top, bottom)) {
        return;
    }
    const core::Color accent = pageAccent(0);
    const std::array<LocalText, 3> labels{{{"PRIMARY", "主按钮"}, {"QUIET", "轻按钮"}, {"GHOST", "幽灵"}}};
    for (int i = 0; i < 3; ++i) {
        const core::Rect r = demoButtonRect(i, x, y, width);
        const float active = state.demoButton == i ? 1.0f : 0.0f;
        const float pressed = state.pressedDemoButton == i ? 1.0f : 0.0f;
        const core::Color fill = active > 0.0f
            ? rgba(accent.r, accent.g, accent.b, (0.68f - pressed * 0.08f) * opacity)
            : (i == 1 ? rgba(accent.r, accent.g, accent.b, 0.12f * opacity) : softTile(opacity));
        const float yPulse = pressed > 0.0f ? 2.0f : 0.0f;
        drawRect(r.x, r.y + yPulse, r.width, r.height, 17.0f,
                 fill,
                 {1.0f, rgba(accent.r, accent.g, accent.b, (active > 0.0f ? 0.48f : 0.18f) * opacity)},
                 {true, {0.0f, 7.0f}, active > 0.0f ? 18.0f : 0.0f, 0.0f, rgba(accent.r, accent.g, accent.b, 0.16f * active * opacity)});
        drawAccentDot(r.x + 23.0f, r.y + 27.0f + yPulse, 4.6f, active > 0.0f ? rgba(1.0f, 1.0f, 1.0f, 0.92f) : accent, opacity);
        drawTextCenteredFit(r.x + r.width * 0.5f + 8.0f, r.y + 18.0f + yPulse, r.width - 52.0f, tr(labels[static_cast<std::size_t>(i)]), 0.98f,
                            active > 0.0f ? rgba(0.98f, 0.98f, 1.0f, 1.0f) : rgba(accent.r, accent.g, accent.b, 1.0f), opacity, 0.76f);
    }
}

void renderInputAndSwitchStudy(PanelState& state, float x, float y, float width, float top, float bottom, float opacity) {
    if (!itemVisible(y, 106.0f, top, bottom)) {
        return;
    }
    const core::Color accent = pageAccent(0);
    drawRect(x, y, width * 0.58f, 92.0f, 20.0f,
             rgba(1.0f, 1.0f, 1.0f, 0.044f * opacity),
             {1.0f, subtleBorder(opacity)});
    drawTextFit(x + 18.0f, y + 14.0f, width * 0.58f - 36.0f, tr("INPUT FIELD", "输入框"), 0.98f, textSoft(), opacity, 0.78f);
    drawRect(x + 18.0f, y + 43.0f, width * 0.58f - 36.0f, 32.0f, 12.0f,
             rgba(0.0f, 0.0f, 0.0f, gNight ? 0.16f * opacity : 0.050f * opacity),
             {1.0f, rgba(accent.r, accent.g, accent.b, 0.30f * opacity)});
    drawTextFit(x + 34.0f, y + 51.0f, width * 0.58f - 76.0f, tr("Hello EUI", "你好 EUI"), 0.94f, textMain(), opacity, 0.78f);
    drawRect(x + width * 0.58f - 48.0f, y + 51.0f, 2.0f, 17.0f, 1.0f, accent, {}, {}, {}, 0.90f * opacity);

    const float rightX = x + width * 0.62f;
    drawRect(rightX, y, width * 0.38f, 92.0f, 20.0f,
             rgba(accent.r, accent.g, accent.b, 0.070f * opacity),
             {1.0f, rgba(accent.r, accent.g, accent.b, 0.20f * opacity)});
    drawTextFit(rightX + 18.0f, y + 14.0f, width * 0.38f - 36.0f, tr("SWITCH", "开关"), 0.98f, textSoft(), opacity, 0.78f);
    const float press = state.pressedDemoSwitch ? 1.0f : 0.0f;
    drawRect(rightX + 18.0f, y + 48.0f + press, 70.0f, 26.0f, 13.0f,
             state.demoSwitch ? rgba(accent.r, accent.g, accent.b, 0.32f * opacity) : trackColor(opacity),
             {1.0f, rgba(accent.r, accent.g, accent.b, (state.demoSwitch ? 0.42f : 0.18f) * opacity)});
    drawAccentDot(rightX + (state.demoSwitch ? 68.0f : 38.0f), y + 61.0f + press, 10.0f, rgba(0.98f, 0.99f, 1.0f, 0.96f), opacity);
    drawTextFit(rightX + 98.0f, y + 51.0f, width * 0.38f - 104.0f, state.demoSwitch ? tr("ON", "开启") : tr("OFF", "关闭"), 1.02f,
                state.demoSwitch ? rgba(accent.r, accent.g, accent.b, 1.0f) : textSoft(), opacity, 0.76f);
}

void renderProgressAndSliderStudy(PanelState& state, float x, float y, float width, float top, float bottom, float opacity) {
    if (!itemVisible(y, 112.0f, top, bottom)) {
        return;
    }
    const core::Color accent = pageAccent(0);
    drawTextFit(x, y, width, tr("SLIDER + PROGRESS", "滑块与进度"), 1.00f, textSoft(), opacity, 0.78f);
    const float progress = 0.68f + std::sin(gTargetFps * 0.03f) * 0.04f;
    drawRect(x, y + 34.0f, width, 11.0f, 5.5f, trackColor(opacity));
    drawRect(x, y + 34.0f, width * clamp01(progress), 11.0f, 5.5f, accent, {}, {}, {}, 0.72f * opacity);
    drawRect(x, y + 77.0f, width, 8.0f, 4.0f, trackColor(opacity));
    drawRect(x, y + 77.0f, width * state.demoSlider, 8.0f, 4.0f, rgba(0.48f, 0.74f, 1.0f, 1.0f), {}, {}, {}, 0.72f * opacity);
    drawAccentDot(x + width * state.demoSlider, y + 81.0f, state.activeControl == 2 ? 16.0f : 13.0f, rgba(0.48f, 0.74f, 1.0f, 0.32f), opacity);
    drawAccentDot(x + width * state.demoSlider, y + 81.0f, 7.0f, rgba(0.98f, 0.99f, 1.0f, 0.96f), opacity);
    drawTextRight(x + width, y + 12.0f, tr("TACTILE", "触感"), 0.86f, rgba(accent.r, accent.g, accent.b, 0.96f), opacity);
}

void renderRhythmStudy(float x, float y, float width, float top, float bottom, float opacity) {
    if (!itemVisible(y, 168.0f, top, bottom)) {
        return;
    }
    const std::array<float, 7> values{{0.28f, 0.52f, 0.44f, 0.78f, 0.62f, 0.92f, 0.70f}};
    const std::array<core::Color, 3> colors{{pageAccent(0), pageAccent(1), pageAccent(3)}};
    drawRect(x, y, width, 158.0f, 22.0f,
             rgba(1.0f, 1.0f, 1.0f, 0.042f * opacity),
             {1.0f, subtleBorder(opacity)});
    drawTextFit(x + 18.0f, y + 15.0f, width * 0.5f, tr("RHYTHM MAP", "节奏图"), 1.05f, textSoft(), opacity, 0.80f);
    drawTextRight(x + width - 18.0f, y + 15.0f, tr("SCROLL SAFE", "滚动安全"), 0.88f, tileTextSoft(), opacity);
    const float base = y + 130.0f;
    float prevX = x + 34.0f;
    float prevY = base - values[0] * 76.0f;
    for (int i = 0; i < static_cast<int>(values.size()); ++i) {
        const float bx = x + 34.0f + static_cast<float>(i) * ((width - 68.0f) / 6.0f);
        const float h = 76.0f * values[static_cast<std::size_t>(i)];
        const core::Color color = colors[static_cast<std::size_t>(i % 3)];
        drawRect(bx - 13.0f, base - h, 26.0f, h, 9.0f,
                 rgba(color.r, color.g, color.b, 0.62f * opacity),
                 {1.0f, rgba(color.r, color.g, color.b, 0.26f * opacity)});
        drawAccentDot(bx, base - h - 7.0f, 3.2f, color, opacity);
        if (i > 0) {
            drawLine(prevX, prevY, bx, base - h - 7.0f, 2.0f, rgba(1.0f, 1.0f, 1.0f, 0.20f), opacity);
        }
        prevX = bx;
        prevY = base - h - 7.0f;
    }
}

void renderDemoPage(PanelState& state, float contentX, float contentY, float contentW, float top, float bottom, float scroll, float opacity) {
    (void)contentY;
    const core::Color accent = pageAccent(0);
    const float y0 = top + 16.0f - scroll;

    const float heroY = y0;
    if (itemVisible(heroY, 122.0f, top, bottom)) {
        core::Gradient gradient;
        gradient.enabled = true;
        gradient.start = rgba(1.0f, 0.42f, 0.52f, 0.20f * opacity);
        gradient.end = rgba(0.48f, 0.74f, 1.0f, 0.09f * opacity);
        gradient.direction = core::GradientDirection::Horizontal;
        drawRect(contentX + 30.0f, heroY, contentW - 60.0f, 122.0f, 26.0f,
                 rgba(1.0f, 0.42f, 0.52f, 0.080f * opacity),
                 {1.0f, rgba(1.0f, 0.70f, 0.76f, 0.16f * opacity)},
                 {},
                 gradient);
        drawRect(contentX + 52.0f, heroY + 24.0f, 4.0f, 72.0f, 2.0f, accent, {}, {}, {}, 0.72f * opacity);
        drawTextFit(contentX + 70.0f, heroY + 23.0f, contentW - 244.0f, tr("MATERIAL ATELIER", "材质工坊"), 1.28f, tileText(), opacity, 0.90f);
        drawTextFit(contentX + 70.0f, heroY + 53.0f, contentW - 252.0f, tr("Buttons, fields, motion rhythm and embedded imagery.", "按钮 输入 动效节奏与内嵌图像"), 0.95f, tileTextSoft(), opacity, 0.72f);
        drawRect(contentX + 70.0f, heroY + 86.0f, 188.0f, 7.0f, 3.5f, rgba(1.0f, 1.0f, 1.0f, 0.12f * opacity));
        drawRect(contentX + 70.0f, heroY + 86.0f, 124.0f, 7.0f, 3.5f, accent, {}, {}, {}, 0.72f * opacity);
        drawEmbeddedAvatar(state, contentX + contentW - 158.0f, heroY + 24.0f, 104.0f, 76.0f, 22.0f, opacity);
        drawRect(contentX + contentW - 164.0f, heroY + 18.0f, 116.0f, 88.0f, 24.0f,
                 rgba(1.0f, 1.0f, 1.0f, 0.002f),
                 {1.0f, rgba(1.0f, 1.0f, 1.0f, 0.18f * opacity)});
    }

    renderButtonStudy(state, contentX + 30.0f, y0 + 144.0f, contentW - 60.0f, top, bottom, opacity);
    renderInputAndSwitchStudy(state, contentX + 30.0f, y0 + 224.0f, contentW - 60.0f, top, bottom, opacity);
    renderProgressAndSliderStudy(state, contentX + 30.0f, y0 + 342.0f, contentW - 60.0f, top, bottom, opacity);

    const std::array<LocalText, 4> swatches{{{"SURFACE", "表面"}, {"PRIMARY", "主色"}, {"ACCENT", "强调"}, {"BORDER", "边界"}}};
    const std::array<core::Color, 4> colors{{
        rgba(0.72f, 0.56f, 1.0f, 1.0f),
        rgba(1.0f, 0.42f, 0.52f, 1.0f),
        rgba(0.48f, 0.74f, 1.0f, 1.0f),
        rgba(0.55f, 0.94f, 0.74f, 1.0f),
    }};
    const float swatchY = y0 + 458.0f;
    for (int i = 0; i < 4; ++i) {
        const float x = contentX + 30.0f + static_cast<float>(i) * 149.0f;
        if (itemVisible(swatchY, 82.0f, top, bottom)) {
            renderMiniSwatch(x, swatchY, 130.0f, 82.0f, swatches[static_cast<std::size_t>(i)], colors[static_cast<std::size_t>(i)], opacity);
        }
    }

    renderRhythmStudy(contentX + 30.0f, y0 + 562.0f, contentW - 60.0f, top, bottom, opacity);

    const std::array<LocalText, 3> titles{{{"PICKERS", "选择器"}, {"DATA TABLE", "数据表"}, {"IMAGE COVER", "图像裁切"}}};
    const std::array<LocalText, 3> notes{{{"Date, time and color stay compact.", "日期 时间 颜色保持紧凑"}, {"Rows, badges and marks align safely.", "行 徽标 标记稳定对齐"}, {"Embedded PNG keeps rounded cover crop.", "内嵌 PNG 保持圆角裁切"}}};
    const std::array<float, 3> values{{0.86f, 0.72f, 1.0f}};
    for (int i = 0; i < 3; ++i) {
        const float y = y0 + 744.0f + static_cast<float>(i) * 84.0f;
        if (itemVisible(y, 70.0f, top, bottom)) {
            renderControlSpec(contentX + 30.0f, y, contentW - 60.0f, titles[static_cast<std::size_t>(i)], notes[static_cast<std::size_t>(i)], values[static_cast<std::size_t>(i)], accent, opacity);
        }
    }
}

void renderTelemetryPage(PanelState& state, float contentX, float contentY, float contentW, float top, float bottom, float scroll, float opacity) {
    (void)contentY;
    (void)bottom;
    (void)scroll;
    const auto metrics = buildMetrics(state);
    for (int i = 0; i < 4; ++i) {
        const float x = contentX + 28.0f + static_cast<float>(i) * 151.0f;
        const float pulse = std::sin(state.launchTime * 2.0f + static_cast<float>(i)) * 0.5f + 0.5f;
        renderMetricCard(x, top + 12.0f, 134.0f, 92.0f, metrics[static_cast<std::size_t>(i)], pulse, opacity);
    }

    const float graphY = top + 116.0f;
    drawGraph(state.fpsSamples, state.graphCursor, contentX + 28.0f, graphY, 286.0f, 106.0f, pageAccent(1), opacity);
    drawGraph(state.cpuSamples, state.graphCursor, contentX + 332.0f, graphY, 286.0f, 106.0f, pageAccent(0), opacity);
    drawTextFit(contentX + 48.0f, graphY + 18.0f, 190.0f, tr("FPS HISTORY", "帧率历史"), 1.00f, tileText(), opacity, 0.78f);
    drawTextFit(contentX + 352.0f, graphY + 18.0f, 190.0f, tr("CPU HISTORY", "CPU 历史"), 1.00f, tileText(), opacity, 0.78f);

    const float cardY = top + 238.0f;
    char targetBudget[24]{};
    char targetSleep[24]{};
    char sampleWindow[24]{};
    std::snprintf(targetBudget, sizeof(targetBudget), "%.1f MS", 1000.0f / std::max(1.0f, gTargetFps));
    std::snprintf(targetSleep, sizeof(targetSleep), "%.0f FPS", gTargetFps);
    std::snprintf(sampleWindow, sizeof(sampleWindow), "%.0f MS", 300.0f);
    const std::array<LocalText, 3> labels{{{"FRAME BUDGET", "帧预算"}, {"SAMPLE WINDOW", "采样窗口"}, {"SLEEP PACE", "休眠节奏"}}};
    const std::array<const char*, 3> values{{targetBudget, sampleWindow, targetSleep}};
    for (int i = 0; i < 3; ++i) {
        const float x = contentX + 30.0f + static_cast<float>(i) * 198.0f;
        drawRect(x, cardY, 178.0f, 44.0f, 15.0f,
                 rgba(1.0f, 1.0f, 1.0f, 0.055f * opacity),
                 {1.0f, rgba(1.0f, 1.0f, 1.0f, 0.09f * opacity)});
        drawTextFit(x + 14.0f, cardY + 8.0f, 150.0f, tr(labels[static_cast<std::size_t>(i)]), 0.80f, tileTextSoft(), opacity, 0.66f);
        drawTextFit(x + 14.0f, cardY + 25.0f, 150.0f, values[static_cast<std::size_t>(i)], 0.98f, tileText(), opacity, 0.76f);
    }

    renderFpsSlider(contentX, contentW, top, opacity);
}

void renderSceneModeSelector(const PanelState& state, float contentX, float contentW, float stageY, float opacity) {
    const core::Color accent = pageAccent(2);
    const std::array<LocalText, 3> labels{{{"MOVE", "位移"}, {"FLIP X", "翻转"}, {"DEPTH", "景深"}}};
    for (int i = 0; i < 3; ++i) {
        const core::Rect r = sceneModeRect(i, contentX, contentW, stageY);
        const bool active = gSceneMode == i;
        const bool pressed = state.pressedSceneMode == i;
        drawRect(r.x, r.y, r.width, r.height, 15.0f,
                 active ? rgba(accent.r, accent.g, accent.b, (0.30f + (pressed ? 0.06f : 0.0f)) * opacity) : softTile(opacity),
                 {1.0f, active ? rgba(accent.r, accent.g, accent.b, 0.48f * opacity) : rgba(1.0f, 1.0f, 1.0f, 0.12f * opacity)});
        drawTextCenteredFit(r.x + r.width * 0.5f, r.y + 9.0f + (pressed ? 1.0f : 0.0f), r.width - 12.0f, tr(labels[static_cast<std::size_t>(i)]), 0.84f,
                            active ? rgba(0.96f, 0.98f, 1.0f, 1.0f) : textSoft(), opacity, 0.66f);
    }
}

void renderPerspectiveCard(float x,
                           float y,
                           float width,
                           float height,
                           const core::Transform& transform,
                           const core::Color& accent,
                           float opacity) {
    const core::TransformMatrix matrix = matrixForTransform({x, y, width, height}, transform);
    core::Gradient gradient;
    gradient.enabled = true;
    gradient.start = rgba(accent.r, accent.g, accent.b, 0.78f * opacity);
    gradient.end = rgba(0.34f, 0.68f, 0.94f, 0.42f * opacity);
    gradient.direction = core::GradientDirection::Horizontal;

    drawRect(x + 16.0f + transform.translate.x * 0.20f, y + height + 20.0f, width - 28.0f, 12.0f, 6.0f,
             rgba(accent.r, accent.g, accent.b, 0.13f * opacity), {}, {}, {}, opacity, 9.0f);
    drawRectMatrix(x, y, width, height, 24.0f,
                   rgba(accent.r, accent.g, accent.b, 0.70f * opacity),
                   matrix,
                   {1.0f, rgba(1.0f, 1.0f, 1.0f, 0.32f * opacity)},
                   {true, {0.0f, 18.0f}, 30.0f, 0.0f, rgba(accent.r, accent.g, accent.b, 0.22f * opacity)},
                   gradient);
    drawRectMatrix(x + 24.0f, y + 24.0f, width * 0.34f, 30.0f, 12.0f,
                   rgba(0.98f, 0.98f, 1.0f, 0.84f * opacity),
                   matrix);
    drawRectMatrix(x + width * 0.56f, y + 24.0f, width * 0.24f, 30.0f, 12.0f,
                   rgba(0.34f, 0.68f, 0.94f, 0.70f * opacity),
                   matrix);
    drawRectMatrix(x + 24.0f, y + height - 54.0f, width * 0.52f, 12.0f, 6.0f,
                   rgba(1.0f, 1.0f, 1.0f, 0.54f * opacity),
                   matrix);
    drawRectMatrix(x + 24.0f, y + height - 32.0f, width * 0.40f, 10.0f, 5.0f,
                   rgba(1.0f, 1.0f, 1.0f, 0.34f * opacity),
                   matrix);
    drawRectMatrix(x + width - 76.0f, y + height - 74.0f, 48.0f, 48.0f, 14.0f,
                   rgba(0.98f, 0.82f, 0.94f, 0.68f * opacity),
                   matrix,
                   {1.0f, rgba(1.0f, 1.0f, 1.0f, 0.28f * opacity)});
}

void renderMotionPage(PanelState& state, float contentX, float contentY, float contentW, float top, float bottom, float scroll, float opacity) {
    (void)contentY;
    const core::Color accent = pageAccent(2);
    const float stageY = top + 14.0f - scroll;
    if (itemVisible(stageY, 226.0f, top, bottom)) {
        drawRect(contentX + 30.0f, stageY, contentW - 60.0f, 226.0f, 28.0f,
                 rgba(accent.r, accent.g, accent.b, 0.080f * opacity),
                 {1.0f, rgba(accent.r, accent.g, accent.b, 0.24f * opacity)});
        drawTextFit(contentX + 52.0f, stageY + 21.0f, 228.0f, tr("PERSPECTIVE CARD", "透视卡片"), 1.16f, tileText(), opacity, 0.82f);
        drawTextFit(contentX + 52.0f, stageY + 50.0f, 248.0f, tr("rotateX / rotateY / translateZ", "旋转与景深矩阵"), 0.86f, tileTextSoft(), opacity, 0.68f);
        renderSceneModeSelector(state, contentX, contentW, stageY, opacity);

        const float t = std::sin(state.launchTime * 1.65f) * 0.5f + 0.5f;
        const float cardX = contentX + 78.0f;
        const float cardY = stageY + 82.0f;
        core::Transform transform;
        transform.origin = {0.5f, 0.5f};
        transform.translate = {gSceneMode == 0 ? t * 92.0f : 42.0f, gSceneMode == 0 ? -6.0f * t : 0.0f};
        transform.rotate = gSceneMode == 0 ? -0.05f + t * 0.10f : 0.02f;
        transform.rotateX = gSceneMode == 1 ? -0.78f + t * 1.56f : -0.18f;
        transform.rotateY = gSceneMode == 2 ? -0.58f + t * 0.32f : (gSceneMode == 1 ? -0.28f : -0.18f + t * 0.36f);
        transform.translateZ = gSceneMode == 2 ? 58.0f + t * 32.0f : 26.0f;
        transform.perspective = 480.0f;
        transform.scale = {gSceneMode == 2 ? 1.04f : 1.0f, gSceneMode == 2 ? 1.04f : 1.0f};
        renderPerspectiveCard(cardX, cardY, 218.0f, 112.0f, transform, accent, opacity);

        drawRect(contentX + 58.0f, stageY + 205.0f, contentW - 116.0f, 2.0f, 1.0f, rgba(1.0f, 1.0f, 1.0f, 0.12f * opacity));
        drawRect(contentX + 58.0f, stageY + 205.0f, (contentW - 116.0f) * (0.18f + t * 0.64f), 2.0f, 1.0f, accent, {}, {}, {}, 0.80f * opacity);
        drawTextFit(contentX + contentW - 220.0f, stageY + 156.0f, 150.0f,
                    gSceneMode == 0 ? tr("Move", "位移") : (gSceneMode == 1 ? tr("Flip X", "翻转") : tr("Depth", "景深")),
                    1.08f,
                    rgba(0.96f, 0.97f, 1.0f, 0.94f),
                    opacity,
                    0.78f);
        drawTextFit(contentX + contentW - 220.0f, stageY + 184.0f, 158.0f,
                    gSceneMode == 0 ? tr("translate + ease", "位移与缓动") : (gSceneMode == 1 ? tr("rotateX + perspective", "X 轴透视") : tr("translateZ + depth", "Z 轴景深")),
                    0.82f,
                    tileTextSoft(),
                    opacity,
                    0.64f);
    }

    renderEffectTiles(contentX, contentW, 2, top + 270.0f, scroll, top, bottom, opacity);

    const float noteY = top + 472.0f - scroll;
    if (itemVisible(noteY, 64.0f, top, bottom)) {
        drawRect(contentX + 30.0f, noteY, contentW - 60.0f, 64.0f, 18.0f,
                 rgba(accent.r, accent.g, accent.b, 0.12f * opacity),
                 {1.0f, rgba(accent.r, accent.g, accent.b, 0.26f * opacity)});
        drawTextFit(contentX + 52.0f, noteY + 17.0f, contentW - 116.0f, tr("LAUNCH  BLUR  ELASTIC SCALE  FADE", "入场  模糊  弹性缩放  淡入"), 0.98f, tileText(), opacity, 0.74f);
        drawTextFit(contentX + 52.0f, noteY + 39.0f, contentW - 116.0f, tr("All motion stays clipped inside the content viewport.", "所有动效保持在内容裁剪区域内"), 0.82f, tileTextSoft(), opacity, 0.64f);
    }
}

void renderSystemPage(float contentX, float contentY, float contentW, float top, float bottom, float scroll, float opacity) {
    (void)contentY;
    const core::Color accent = pageAccent(3);
    const float y0 = top + 16.0f - scroll;
    const std::array<LocalText, 4> rows{{{"ANDROID SURFACE", "安卓表面"}, {"VULKAN BACKEND", "Vulkan 后端"}, {"STATIC STL", "静态 STL"}, {"EMBEDDED PNG", "内嵌 PNG"}}};
    const std::array<LocalText, 4> status{{{"READY", "就绪"}, {"ACTIVE", "运行"}, {"LINKED", "已链接"}, {"IN ELF", "在 ELF 内"}}};
    for (int i = 0; i < 4; ++i) {
        const float y = y0 + static_cast<float>(i) * 56.0f;
        if (!itemVisible(y, 46.0f, top, bottom)) {
            continue;
        }
        drawRect(contentX + 30.0f, y, contentW - 60.0f, 46.0f, 15.0f,
                 rgba(1.0f, 1.0f, 1.0f, 0.065f * opacity),
                 {1.0f, rgba(1.0f, 1.0f, 1.0f, 0.10f * opacity)});
        drawAccentDot(contentX + 52.0f, y + 23.0f, 5.0f, accent, opacity);
        drawTextFit(contentX + 68.0f, y + 14.0f, 330.0f, tr(rows[static_cast<std::size_t>(i)]), 1.04f, tileText(), opacity, 0.78f);
        drawTextRight(contentX + contentW - 54.0f, y + 14.0f, tr(status[static_cast<std::size_t>(i)]), 1.02f, rgba(accent.r, accent.g, accent.b, 1.0f), opacity);
    }

    const float stackY = y0 + 250.0f;
    if (itemVisible(stackY, 172.0f, top, bottom)) {
        drawRect(contentX + 30.0f, stackY, contentW - 60.0f, 172.0f, 24.0f,
                 rgba(accent.r, accent.g, accent.b, 0.090f * opacity),
                 {1.0f, rgba(accent.r, accent.g, accent.b, 0.22f * opacity)});
        drawTextFit(contentX + 54.0f, stackY + 21.0f, 300.0f, tr("DEPLOYMENT SHAPE", "部署形态"), 1.12f, tileText(), opacity, 0.82f);
        drawTextFit(contentX + 54.0f, stackY + 48.0f, contentW - 108.0f, tr("Single executable with embedded image and font fallback.", "单一可执行文件 内嵌图像与字体兜底"), 0.84f, tileTextSoft(), opacity, 0.66f);
        const std::array<LocalText, 3> steps{{{"ROOT ELF", "Root ELF"}, {"SURFACE", "Surface"}, {"VULKAN", "Vulkan"}}};
        for (int i = 0; i < 3; ++i) {
            const float x = contentX + 54.0f + static_cast<float>(i) * 182.0f;
            drawRect(x, stackY + 92.0f, 138.0f, 48.0f, 18.0f, rgba(1.0f, 1.0f, 1.0f, 0.070f * opacity), {1.0f, rgba(1.0f, 1.0f, 1.0f, 0.10f * opacity)});
            drawTextCenteredFit(x + 69.0f, stackY + 108.0f, 112.0f, tr(steps[static_cast<std::size_t>(i)]), 0.84f, tileText(), opacity, 0.66f);
            if (i < 2) {
                drawLine(x + 146.0f, stackY + 116.0f, x + 174.0f, stackY + 116.0f, 2.5f, accent, 0.72f * opacity);
            }
        }
    }
}

float pageScrollLimit(int page) {
    if (page == 0) {
        return 632.0f;
    }
    if (page == 1) {
        return 0.0f;
    }
    if (page == 2) {
        return 168.0f;
    }
    return 54.0f;
}

void renderContent(PanelState& state, float opacity) {
    const float contentX = 258.0f;
    const float contentY = 42.0f;
    const float contentW = 654.0f;
    const float contentH = 540.0f;
    const int page = state.selected;
    const core::Color accent = pageAccent(page);
    const float scrollTop = contentY + 126.0f;
    const float scrollBottom = contentY + contentH - 42.0f;
    const float scroll = std::clamp(state.contentScroll, 0.0f, pageScrollLimit(page));

    drawContentFrame(contentX, contentY, contentW, contentH, accent, opacity);

    setRenderScissor(true, {contentX + 6.0f, scrollTop, contentW - 12.0f, scrollBottom - scrollTop});
    if (page == 0) {
        renderDemoPage(state, contentX, contentY, contentW, scrollTop, scrollBottom, scroll, opacity);
    } else if (page == 1) {
        renderTelemetryPage(state, contentX, contentY, contentW, scrollTop, scrollBottom, scroll, opacity);
    } else if (page == 2) {
        renderMotionPage(state, contentX, contentY, contentW, scrollTop, scrollBottom, scroll, opacity);
    } else {
        renderSystemPage(contentX, contentY, contentW, scrollTop, scrollBottom, scroll, opacity);
    }
    setRenderScissor(false, {});

    drawScrollMasks(contentX, contentY, contentW, contentH, scrollTop, scrollBottom, accent, opacity);
    renderContentHeader(contentX, contentY, contentW, scrollTop, page, accent, opacity);

    const float scrollBarTrackY = scrollTop + 8.0f;
    const float scrollBarH = scrollBottom - scrollTop - 16.0f;
    const float limit = pageScrollLimit(page);
    const float thumbH = limit > 0.0f ? 82.0f : scrollBarH;
    const float thumbT = limit > 0.0f ? std::clamp(scroll / limit, 0.0f, 1.0f) : 0.0f;
    drawRect(contentX + contentW - 22.0f, scrollBarTrackY, 5.0f, scrollBarH, 2.5f, rgba(1.0f, 1.0f, 1.0f, 0.10f * opacity));
    if (limit > 0.0f) {
        const float thumbY = scrollBarTrackY + thumbT * std::max(0.0f, scrollBarH - thumbH);
        drawRect(contentX + contentW - 23.0f, thumbY, 7.0f, thumbH, 3.5f, rgba(accent.r, accent.g, accent.b, 0.72f * opacity));
    }
}

void renderPanel(PanelState& state) {
    const float intro = easeOutBack(state.launchTime / 0.72f);
    const float fade = easeOutCubic(state.launchTime / 0.46f);
    const float blur = (1.0f - clamp01(state.launchTime / 0.58f)) * 18.0f;

    core::Gradient shellGradient;
    shellGradient.enabled = true;
    shellGradient.start = shellStart();
    shellGradient.end = shellEnd();
    shellGradient.direction = core::GradientDirection::Vertical;

    drawIntroRect(0.0f, 0.0f, static_cast<float>(kPanelWidth), static_cast<float>(kPanelHeight), kShellRadius,
                  shellStart(),
                  intro,
                  {1.0f, subtleBorder(fade)},
                  {true, {0.0f, 22.0f}, 52.0f, 0.0f, gNight ? rgba(0.03f, 0.01f, 0.02f, 0.42f * fade) : rgba(0.28f, 0.20f, 0.24f, 0.12f * fade)},
                  shellGradient,
                  fade,
                  blur);

    if (intro < 0.08f) {
        return;
    }

    core::Gradient glow;
    glow.enabled = true;
    glow.start = gNight ? rgba(1.0f, 0.44f, 0.54f, 0.24f * fade) : rgba(1.0f, 0.55f, 0.44f, 0.16f * fade);
    glow.end = gNight ? rgba(0.44f, 0.62f, 1.0f, 0.08f * fade) : rgba(0.36f, 0.58f, 1.0f, 0.10f * fade);
    glow.direction = core::GradientDirection::Horizontal;
    drawRect(18.0f, 18.0f, kPanelWidth - 36.0f, kPanelHeight - 36.0f, 44.0f,
             rgba(1.0f, 1.0f, 1.0f, 0.030f * fade), {}, {}, glow, fade);

    drawRect(28.0f, 32.0f, 206.0f, 550.0f, 42.0f,
             railFill(),
             {1.0f, subtleBorder(fade)});

    renderAvatar(state, intro, fade);
    renderNavigation(state, fade);
    renderContent(state, fade);
    renderLanguageToggle(state, fade);
    renderThemeToggle(state, fade);

    drawRect(282.0f, 588.0f, 342.0f, 8.0f, 4.0f, rgba(1.0f, 1.0f, 1.0f, 0.13f * fade));
    drawRect(282.0f, 588.0f, 86.0f + state.navBlend * 64.0f, 8.0f, 4.0f, rgba(pageAccent(state.selected).r, pageAccent(state.selected).g, pageAccent(state.selected).b, 0.70f * fade));
    drawTextRight(888.0f, 591.0f, tr("SINGLE ELF", "单一 ELF"), 0.92f, rgba(0.72f, 0.76f, 0.86f, 0.82f), fade);
}

void handleInput(PanelState& state, core::window::Handle window, float deltaSeconds) {
    const core::PointerEvent pointer = core::readPointerEvent(window, 1.0f);
    const auto events = core::consumeInputEvents(window);
    const core::ScrollEvent scroll = events.second;

    constexpr float contentX = 258.0f;
    constexpr float contentY = 42.0f;
    constexpr float contentW = 654.0f;
    constexpr float contentH = 540.0f;
    const float scrollTop = contentY + 126.0f;
    const float scrollBottom = contentY + contentH - 42.0f;
    const float scrollValue = std::clamp(state.contentScroll, 0.0f, pageScrollLimit(state.selected));
    const float demoY0 = scrollTop + 16.0f - scrollValue;
    const float motionStageY = scrollTop + 14.0f - scrollValue;

    const core::Rect fpsTrackRect = fpsSliderRect(contentX, contentW, scrollTop);
    const core::Rect fpsRect = expandedRect(fpsTrackRect, 18.0f);
    const core::Rect languageRect = expandedRect(languageToggleRect(), 12.0f);
    const core::Rect themeRect = expandedRect(themeToggleRect(), 12.0f);
    const core::Rect demoButtonArea{contentX + 30.0f, demoY0 + 134.0f, contentW - 60.0f, 82.0f};
    const core::Rect demoSwitchHit = expandedRect(demoSwitchRect(contentX + 30.0f, demoY0 + 224.0f, contentW - 60.0f), 10.0f);
    const core::Rect demoSliderHit = expandedRect(demoSliderRect(contentX + 30.0f, demoY0 + 342.0f, contentW - 60.0f), 10.0f);

    bool interacting = false;
    state.pressedLanguage = pointer.down && contains(languageRect, pointer.x, pointer.y);
    state.pressedTheme = pointer.down && contains(themeRect, pointer.x, pointer.y);
    state.pressedDemoButton = -1;
    state.pressedSceneMode = -1;
    state.pressedDemoSwitch = false;
    state.activeControl = 0;

    if (pointer.down && state.selected == 1 && contains(fpsRect, pointer.x, pointer.y)) {
        const float t = std::clamp(static_cast<float>(pointer.x - fpsTrackRect.x) / fpsTrackRect.width, 0.0f, 1.0f);
        gTargetFps = std::round((kFpsMin + (kFpsMax - kFpsMin) * t) / 5.0f) * 5.0f;
        state.activeControl = 1;
        interacting = true;
    }

    if (pointer.releasedThisFrame && contains(languageRect, pointer.x, pointer.y)) {
        gChinese = !gChinese;
        interacting = true;
    }

    if (pointer.releasedThisFrame && contains(themeRect, pointer.x, pointer.y)) {
        gNight = !gNight;
        interacting = true;
    }

    if (state.selected == 0 && pointer.y >= scrollTop && pointer.y <= scrollBottom) {
        if (pointer.down && contains(demoSliderHit, pointer.x, pointer.y)) {
            state.demoSlider = std::clamp(static_cast<float>(pointer.x - demoSliderHit.x) / demoSliderHit.width, 0.0f, 1.0f);
            state.activeControl = 2;
            interacting = true;
        }
        if (pointer.down && contains(demoSwitchHit, pointer.x, pointer.y)) {
            state.pressedDemoSwitch = true;
            interacting = true;
        }
        if (pointer.releasedThisFrame && contains(demoSwitchHit, pointer.x, pointer.y)) {
            state.demoSwitch = !state.demoSwitch;
            interacting = true;
        }
        for (int i = 0; i < 3; ++i) {
            const core::Rect button = expandedRect(demoButtonRect(i, contentX + 30.0f, demoY0 + 144.0f, contentW - 60.0f), 8.0f);
            if (pointer.down && contains(button, pointer.x, pointer.y)) {
                state.pressedDemoButton = i;
                interacting = true;
            }
            if (pointer.releasedThisFrame && contains(button, pointer.x, pointer.y)) {
                state.demoButton = i;
                interacting = true;
            }
        }
        interacting = interacting || (pointer.down && contains(demoButtonArea, pointer.x, pointer.y));
    }

    if (state.selected == 2) {
        for (int i = 0; i < 3; ++i) {
            const core::Rect sceneRect = expandedRect(sceneModeRect(i, contentX, contentW, motionStageY), 8.0f);
            if (pointer.down && contains(sceneRect, pointer.x, pointer.y)) {
                state.pressedSceneMode = i;
                interacting = true;
            }
            if (pointer.releasedThisFrame && contains(sceneRect, pointer.x, pointer.y)) {
                gSceneMode = i;
                interacting = true;
            }
        }
    }

    const bool inContent = pointer.x >= contentX && pointer.x <= contentX + contentW &&
                           pointer.y >= scrollTop && pointer.y <= scrollBottom;
    if (!interacting && scroll.active() && inContent) {
        state.targetScroll = std::clamp(state.targetScroll - static_cast<float>(scroll.y) * 34.0f,
                                        0.0f,
                                        pageScrollLimit(state.selected));
    }

    state.pressedNav = -1;
    for (int i = 0; i < 4; ++i) {
        if (pointer.down && contains(navRect(i), pointer.x, pointer.y)) {
            state.pressedNav = i;
            interacting = true;
        }
        if (pointer.releasedThisFrame && contains(navRect(i), pointer.x, pointer.y)) {
            state.selected = i;
            state.targetScroll = 0.0f;
            state.contentScroll = 0.0f;
            interacting = true;
        }
    }

    state.targetScroll = std::clamp(state.targetScroll, 0.0f, pageScrollLimit(state.selected));
    state.contentScroll = approach(state.contentScroll, state.targetScroll, deltaSeconds, 13.5f);
    state.navBlend = approach(state.navBlend, static_cast<float>(state.selected), deltaSeconds, 10.0f);
    state.launchTime += deltaSeconds;
    state.previousDown = pointer.down;
}

int drawableWidth(core::window::Handle window) {
    const auto info = core::window::nativeWindowInfo(window);
    auto* nativeWindow = static_cast<ANativeWindow*>(info.platformWindow);
    const int width = nativeWindow != nullptr ? ANativeWindow_getWidth(nativeWindow) : 0;
    return width > 0 ? width : kPanelWidth;
}

int drawableHeight(core::window::Handle window) {
    const auto info = core::window::nativeWindowInfo(window);
    auto* nativeWindow = static_cast<ANativeWindow*>(info.platformWindow);
    const int height = nativeWindow != nullptr ? ANativeWindow_getHeight(nativeWindow) : 0;
    return height > 0 ? height : kPanelHeight;
}

} // namespace

int main() {
    __android_log_print(ANDROID_LOG_INFO, "NeoPanel", "NeoPanel Android ELF started");

    core::render::initializeRenderBackendLoader();

    core::window::WindowCreateRequest request;
    request.width = kPanelWidth;
    request.height = kPanelHeight;
    request.title = "NeoPanel";
    request.renderApi = core::render::windowRenderApi();

    core::window::Handle window = core::window::createWindow(request);
    if (window == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "NeoPanel", "Failed to create window");
        return 1;
    }

    std::unique_ptr<core::render::RenderBackend> backend = core::render::createRenderBackend(window);
    if (!backend || !backend->initialize()) {
        __android_log_print(ANDROID_LOG_ERROR, "NeoPanel", "Failed to initialize Vulkan backend");
        backend.reset();
        core::window::destroyWindow(window);
        return 2;
    }

    PanelState state;
    state.previousTime = core::window::timeSeconds();

    while (true) {
        const double now = core::window::timeSeconds();
        const float deltaSeconds = static_cast<float>(std::clamp(now - state.previousTime, 0.0, 0.050));
        state.previousTime = now;

        handleInput(state, window, deltaSeconds);
        updateRuntimeStats(state, deltaSeconds, now);

        const int width = drawableWidth(window);
        const int height = drawableHeight(window);
        if (width <= 0 || height <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        backend->beginFrame({window, core::window::nativeWindowInfo(window), width, height, 1.0f});
        {
            core::render::ScopedRenderBackend scoped(*backend);
            backend->clear(rgba(0.0f, 0.0f, 0.0f, 0.0f));
            renderPanel(state);
        }
        backend->present();

        const double frameEnd = core::window::timeSeconds();
        const double targetFrameSeconds = 1.0 / std::max(1.0f, gTargetFps);
        const double remainingSeconds = targetFrameSeconds - (frameEnd - now);
        if (remainingSeconds > 0.0005) {
            std::this_thread::sleep_for(std::chrono::duration<double>(remainingSeconds));
        }
    }

    destroyFontTextures(*backend);
    if (state.avatar.handle != nullptr) {
        backend->destroyTexture(state.avatar.handle);
        state.avatar.handle = nullptr;
    }
    core::releaseInputQueue(window);
    backend.reset();
    core::window::destroyWindow(window);
    return 0;
}
