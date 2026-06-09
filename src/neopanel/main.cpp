#include "src/neopanel/embedded_assets.h"

#include "core/input/input_state.h"
#include "core/render/primitive.h"
#include "core/render/render_backend.h"
#include "core/window/window_backend.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "3rd/stb_truetype.h"
#include "3rd/stb_image.h"

#include <android/log.h>
#include <android/native_window.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <csignal>
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
constexpr float kPi = 3.14159265358979323846f;
constexpr int kStarTextureSize = 288;
constexpr int kStarOutlinePointCount = 64;
constexpr int kStarFacetPointCount = 10;
constexpr int kStarVolumeLayerCount = 7;
constexpr int kStarVolumeStride = 2;
constexpr int kStarMeshSegmentCount = 72;
constexpr int kStarMeshRingCount = 6;
constexpr int kStarMeshBackRingCount = 5;
constexpr int kStarMeshSideBandCount = 5;
constexpr float kStarEntrySpinDuration = 1.42f;
constexpr float kStarRestYaw = -0.12f;
constexpr float kStarRestPitch = -0.10f;
constexpr float kExitAnimationDuration = 0.96f;

bool gChinese = false;
bool gNight = true;
float gTargetFps = 60.0f;
volatile std::sig_atomic_t gExitRequested = 0;

void requestShutdown(int) {
    gExitRequested = 1;
}

void installSignalHandlers() {
    std::signal(SIGINT, requestShutdown);
    std::signal(SIGTERM, requestShutdown);
#if defined(SIGHUP)
    std::signal(SIGHUP, requestShutdown);
#endif
}

struct CpuCoreInfo {
    int index = 0;
    int score = 0;
};

int readSysInt(const char* path) {
    std::FILE* file = std::fopen(path, "r");
    if (file == nullptr) {
        return -1;
    }

    int value = -1;
    const int matched = std::fscanf(file, "%d", &value);
    std::fclose(file);
    return matched == 1 ? value : -1;
}

std::vector<CpuCoreInfo> discoverCpuCores() {
    long count = sysconf(_SC_NPROCESSORS_CONF);
    if (count <= 0) {
        count = static_cast<long>(std::max(1u, std::thread::hardware_concurrency()));
    }
    count = std::max<long>(1, std::min<long>(count, CPU_SETSIZE));

    std::vector<CpuCoreInfo> cores;
    cores.reserve(static_cast<std::size_t>(count));
    for (long i = 0; i < count; ++i) {
        char path[160]{};
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/online", i);
        const int online = i == 0 ? 1 : readSysInt(path);
        if (online == 0) {
            continue;
        }

        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/cpu_capacity", i);
        const int capacity = readSysInt(path);
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", i);
        const int maxFreq = readSysInt(path);

        int score = static_cast<int>(i) + 1;
        if (capacity > 0) {
            score += capacity * 1000;
        }
        if (maxFreq > 0) {
            score += maxFreq / 100;
        }
        cores.push_back({static_cast<int>(i), score});
    }

    return cores;
}

std::vector<int> selectRenderCpus(std::vector<CpuCoreInfo> cores) {
    if (cores.size() < 4) {
        return {};
    }

    std::sort(cores.begin(), cores.end(), [](const CpuCoreInfo& a, const CpuCoreInfo& b) {
        if (a.score == b.score) {
            return a.index > b.index;
        }
        return a.score > b.score;
    });

    const std::size_t targetCount = cores.size() >= 8 ? 3u : 2u;
    std::vector<int> selected;
    selected.reserve(targetCount);
    for (std::size_t i = 0; i < std::min(targetCount, cores.size()); ++i) {
        selected.push_back(cores[i].index);
    }
    return selected;
}

bool applyCurrentThreadAffinity(const std::vector<int>& cpus) {
    if (cpus.empty()) {
        return false;
    }

    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (const int cpu : cpus) {
        if (cpu >= 0 && cpu < CPU_SETSIZE) {
            CPU_SET(cpu, &mask);
        }
    }
    return sched_setaffinity(0, sizeof(mask), &mask) == 0;
}

void formatCpuList(const std::vector<int>& cpus, char* output, std::size_t outputSize) {
    if (output == nullptr || outputSize == 0) {
        return;
    }

    output[0] = '\0';
    for (std::size_t i = 0; i < cpus.size(); ++i) {
        char part[16]{};
        std::snprintf(part, sizeof(part), "%s%d", i == 0 ? "" : ",", cpus[i]);
        const std::size_t length = std::strlen(output);
        if (length + std::strlen(part) + 1 >= outputSize) {
            break;
        }
        std::strncat(output, part, outputSize - length - 1);
    }
}

void applyRenderThreadSchedulingHints() {
    static bool applied = false;
    if (applied) {
        return;
    }
    applied = true;

#if defined(PR_SET_NAME)
    prctl(PR_SET_NAME, "NeoPanelRender", 0, 0, 0);
#endif
    if (setpriority(PRIO_PROCESS, 0, -4) != 0) {
        __android_log_print(ANDROID_LOG_INFO, "NeoPanel", "Render nice priority unchanged: %s", std::strerror(errno));
    }

    const std::vector<int> renderCpus = selectRenderCpus(discoverCpuCores());
    if (!renderCpus.empty() && applyCurrentThreadAffinity(renderCpus)) {
        char cpuList[96]{};
        formatCpuList(renderCpus, cpuList, sizeof(cpuList));
        __android_log_print(ANDROID_LOG_INFO, "NeoPanel", "Render thread affinity: cpu%s", cpuList);
    } else {
        __android_log_print(ANDROID_LOG_INFO, "NeoPanel", "Render thread affinity left to Android scheduler");
    }
}

struct EmbeddedTexture {
    core::render::RenderBackend::TextureHandle handle = nullptr;
    int width = 0;
    int height = 0;
    bool decoded = false;
    bool uploaded = false;
    std::vector<unsigned char> pixels;
};

struct StarRenderTexture {
    core::render::RenderBackend::TextureHandle handle = nullptr;
    int width = 0;
    int height = 0;
    bool baseReady = false;
    int drawHoldFrames = 0;
    float shadedYaw = 1000.0f;
    float shadedPitch = 1000.0f;
    float shadedPhase = -1000.0f;
    bool shadedPressed = false;
    std::vector<unsigned char> pixels;
    std::vector<float> alpha;
    std::vector<float> glow;
    std::vector<float> heightMap;
    std::vector<float> edge;
    std::vector<float> normalX;
    std::vector<float> normalY;
    std::vector<float> normalZ;
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

struct StarMeshVertex {
    core::Vec3 position{};
    core::Vec3 normal{};
};

struct StarProjectedVertex {
    core::Vec2 screen{};
    core::Vec3 world{};
    core::Vec3 normal{};
    float depth = 0.0f;
};

struct StarMeshTriangle {
    StarProjectedVertex a{};
    StarProjectedVertex b{};
    StarProjectedVertex c{};
    core::Color color{};
    float opacity = 1.0f;
    float sortDepth = 0.0f;
};

enum class StarMeshMaterial {
    Front,
    FrontGlow,
    Side,
    Back,
    BackGlow
};

struct PanelState {
    int selected = 0;
    int pressedNav = -1;
    int capturedNav = -1;
    int pressedDemoButton = -1;
    int capturedDemoButton = -1;
    int demoButton = 0;
    int activeControl = 0;
    bool pressedLanguage = false;
    bool capturedLanguage = false;
    bool pressedTheme = false;
    bool capturedTheme = false;
    bool pressedExit = false;
    bool capturedExit = false;
    bool exitAnimationActive = false;
    bool pressedDemoSwitch = false;
    bool capturedDemoSwitch = false;
    bool capturedDemoSlider = false;
    bool capturedFpsSlider = false;
    bool pressedStar = false;
    bool capturedStar = false;
    bool demoSwitch = true;
    float demoSlider = 0.44f;
    float starYaw = kStarRestYaw;
    float starPitch = kStarRestPitch;
    float starTargetYaw = kStarRestYaw;
    float starTargetPitch = kStarRestPitch;
    float starYawVelocity = 0.0f;
    float starPitchVelocity = 0.0f;
    float starPressAmount = 0.0f;
    bool starStageSeen = false;
    float starEntrySpinTime = 0.0f;
    float starIdleTime = 0.0f;
    float exitAnimationTime = 0.0f;
    float navBlend = 0.0f;
    float contentScroll = 0.0f;
    float targetScroll = 0.0f;
    float launchTime = 0.0f;
    double previousTime = 0.0;
    bool previousDown = false;
    bool staticResourcesPrepared = false;
    EmbeddedTexture avatar;
    RuntimeStats stats;
    std::array<float, kGraphSamples> fpsSamples{};
    std::array<float, kGraphSamples> cpuSamples{};
    int graphCursor = 0;
    StarRenderTexture starTexture;
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
    {"EFFECT STAGE", "Premium star depth, pearl light and gesture-driven motion.", "MOTION",
     "动效舞台", "高级版星形深度 光泽与手势动效", "动效"},
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

float lerpFloat(float a, float b, float t) {
    return a + (b - a) * clamp01(t);
}

float approach(float current, float target, float deltaSeconds, float sharpness) {
    const float t = 1.0f - std::exp(-sharpness * std::max(0.0f, deltaSeconds));
    return current + (target - current) * t;
}

float springApproach(float current, float target, float& velocity, float deltaSeconds, float stiffness, float damping) {
    const float dt = std::clamp(deltaSeconds, 0.0f, 0.050f);
    const float acceleration = (target - current) * stiffness - velocity * damping;
    velocity += acceleration * dt;
    current += velocity * dt;
    if (std::fabs(target - current) < 0.00035f && std::fabs(velocity) < 0.00035f) {
        velocity = 0.0f;
        return target;
    }
    return current;
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

float smoothstep(float edge0, float edge1, float value) {
    const float t = std::clamp((value - edge0) / std::max(0.0001f, edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float fract(float value) {
    return value - std::floor(value);
}

unsigned char byteFromFloat(float value) {
    return static_cast<unsigned char>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

bool contains(const core::Rect& rect, double x, double y) {
    return rect.contains(x, y);
}

void clearPointerCaptures(PanelState& state) {
    state.capturedNav = -1;
    state.capturedDemoButton = -1;
    state.capturedLanguage = false;
    state.capturedTheme = false;
    state.capturedExit = false;
    state.capturedDemoSwitch = false;
    state.capturedDemoSlider = false;
    state.capturedFpsSlider = false;
    state.capturedStar = false;
}

bool hasPointerCapture(const PanelState& state) {
    return state.capturedNav >= 0 ||
           state.capturedDemoButton >= 0 ||
           state.capturedLanguage ||
           state.capturedTheme ||
           state.capturedExit ||
           state.capturedDemoSwitch ||
           state.capturedDemoSlider ||
           state.capturedFpsSlider ||
           state.capturedStar;
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

core::Rect premiumStarStageRect(float contentX, float contentW, float stageY) {
    return {contentX + 30.0f, stageY, contentW - 60.0f, 284.0f};
}

core::Rect premiumStarVisualRect(float contentX, float contentW, float stageY) {
    constexpr float size = 244.0f;
    return {contentX + contentW * 0.5f - size * 0.5f, stageY + 36.0f, size, size};
}

core::Rect premiumStarHitRect(float contentX, float contentW, float stageY) {
    return expandedRect(premiumStarStageRect(contentX, contentW, stageY), 8.0f);
}

core::Rect systemExitButtonRect(float contentX, float contentW, float top, float scroll) {
    return {contentX + 30.0f, top + 244.0f - scroll, contentW - 60.0f, 64.0f};
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

void drawPolygonAbs(const core::Vec2* points, std::size_t count, const core::Color& color, float opacity = 1.0f) {
    if (points == nullptr || count < 3 || opacity <= 0.001f || color.a <= 0.001f) {
        return;
    }

    float minX = points[0].x;
    float minY = points[0].y;
    float maxX = points[0].x;
    float maxY = points[0].y;
    for (std::size_t i = 0; i < count; ++i) {
        minX = std::min(minX, points[i].x);
        minY = std::min(minY, points[i].y);
        maxX = std::max(maxX, points[i].x);
        maxY = std::max(maxY, points[i].y);
    }
    if (maxX <= minX || maxY <= minY) {
        return;
    }

    static thread_local std::vector<core::Vec2> relative;
    relative.clear();
    relative.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        relative.push_back({points[i].x - minX, points[i].y - minY});
    }

    core::PolygonPrimitive primitive;
    primitive.initialize();
    primitive.setBounds(minX, minY, maxX - minX, maxY - minY);
    primitive.setPoints(relative);
    primitive.setColor(color);
    primitive.setOpacity(opacity);
    primitive.render(kPanelWidth, kPanelHeight);
}

void drawPolygonAbs(const std::vector<core::Vec2>& points, const core::Color& color, float opacity = 1.0f) {
    drawPolygonAbs(points.data(), points.size(), color, opacity);
}

void drawPolygonAbs(const std::array<core::Vec2, 4>& points, const core::Color& color, float opacity = 1.0f) {
    drawPolygonAbs(points.data(), points.size(), color, opacity);
}

void drawPolygonAbs(const std::array<core::Vec2, 3>& points, const core::Color& color, float opacity = 1.0f) {
    drawPolygonAbs(points.data(), points.size(), color, opacity);
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
    int width = 0;
    int height = 0;
    int atlasX = 0;
    int atlasY = 0;
    float xOffset = 0.0f;
    float yOffset = 0.0f;
    float advance = 0.0f;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    bool packed = false;
};

struct FontAtlas {
    core::render::RenderBackend::TextureHandle handle = nullptr;
    int width = 512;
    int height = 512;
    int cursorX = 1;
    int cursorY = 1;
    int rowHeight = 0;
    bool dirty = false;
    std::vector<unsigned char> pixels;
};

struct FontRuntime {
    bool attempted = false;
    bool ready = false;
    stbtt_fontinfo font{};
    const unsigned char* fontData = nullptr;
    std::size_t fontDataSize = 0;
    int fontIndex = 0;
    std::unordered_map<std::uint64_t, FontGlyph> glyphs;
    std::unordered_map<int, FontAtlas> atlases;
    std::unordered_map<std::string, float> widthCache;
};

FontRuntime& fontRuntime() {
    static FontRuntime runtime;
    return runtime;
}

bool ensureTextureUploaded(EmbeddedTexture& texture);

void destroyFontTextures(core::render::RenderBackend& backend) {
    FontRuntime& runtime = fontRuntime();
    for (auto& entry : runtime.atlases) {
        if (entry.second.handle != nullptr) {
            backend.destroyTexture(entry.second.handle);
            entry.second.handle = nullptr;
        }
    }
    runtime.glyphs.clear();
    runtime.atlases.clear();
    runtime.widthCache.clear();
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

bool ensureFontRuntime() {
    FontRuntime& runtime = fontRuntime();
    if (runtime.attempted) {
        return runtime.ready;
    }
    runtime.attempted = true;

    runtime.ready = false;
    for (int index = 0; index < 8; ++index) {
        if (tryInitFont(runtime,
                        neopanel::assets::uiFontTtf,
                        neopanel::assets::uiFontTtfSize,
                        index,
                        "embedded-pingfang-ui-font",
                        true)) {
            runtime.ready = true;
            break;
        }
    }
    if (!runtime.ready) {
        __android_log_print(ANDROID_LOG_ERROR, "NeoPanel", "Failed to initialize UI font");
    }
    return runtime.ready;
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
    return std::clamp(static_cast<int>(std::round(scale * 18.5f)), 16, 78);
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
    std::string cacheKey;
    cacheKey.reserve(std::strlen(text) + 16u);
    cacheKey.append(std::to_string(pixelHeight));
    cacheKey.push_back(':');
    cacheKey.append(text);
    if (auto it = runtime.widthCache.find(cacheKey); it != runtime.widthCache.end()) {
        return it->second;
    }

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
    if (runtime.widthCache.size() > 512u) {
        runtime.widthCache.clear();
    }
    runtime.widthCache.emplace(std::move(cacheKey), width);
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

void drawTextureQuadMatrix(core::render::RenderBackend::TextureHandle handle,
                           float x,
                           float y,
                           float width,
                           float height,
                           const core::TransformMatrix& matrix,
                           const core::Color& tint,
                           float radius = 0.0f) {
    if (handle == nullptr || width <= 0.0f || height <= 0.0f || tint.a <= 0.001f) {
        return;
    }
    core::render::RenderBackend* backend = core::render::activeRenderBackend();
    if (backend == nullptr) {
        return;
    }
    const float localPositions[4][2] = {
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
        const core::Vec3 projected = core::transformPointWithW(matrix, localPositions[index][0], localPositions[index][1]);
        vertices[static_cast<std::size_t>(offset + 0)] = projected.x;
        vertices[static_cast<std::size_t>(offset + 1)] = projected.y;
        vertices[static_cast<std::size_t>(offset + 2)] = projected.z;
        vertices[static_cast<std::size_t>(offset + 3)] = localPositions[index][0];
        vertices[static_cast<std::size_t>(offset + 4)] = localPositions[index][1];
        vertices[static_cast<std::size_t>(offset + 5)] = uvs[index][0];
        vertices[static_cast<std::size_t>(offset + 6)] = uvs[index][1];
    }
    backend->drawTexture(handle, vertices.data(), vertices.size(), tint, {x, y, width, height}, radius, kPanelWidth, kPanelHeight);
}

void drawTextureSubQuad(core::render::RenderBackend::TextureHandle handle,
                        float x,
                        float y,
                        float width,
                        float height,
                        float u0,
                        float v0,
                        float u1,
                        float v1,
                        const core::Color& tint) {
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
    const float uvs[4][2] = {
        {u0, v0},
        {u1, v0},
        {u1, v1},
        {u0, v1},
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
    backend->drawTexture(handle, vertices.data(), vertices.size(), tint, {x, y, width, height}, 0.0f, kPanelWidth, kPanelHeight);
}

float hash01(float value) {
    return fract(std::sin(value * 12.9898f + 78.233f) * 43758.5453f);
}

core::Vec3 normalize3(core::Vec3 value) {
    const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
    if (length <= 0.0001f) {
        return {0.0f, 0.0f, 1.0f};
    }
    const float inv = 1.0f / length;
    return {value.x * inv, value.y * inv, value.z * inv};
}

float dot3(const core::Vec3& a, const core::Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

core::Vec3 cross3(const core::Vec3& a, const core::Vec3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

core::Vec3 add3(const core::Vec3& a, const core::Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

core::Vec3 rotateNormal(float x, float y, float z, float pitch, float yaw) {
    const float cosY = std::cos(yaw);
    const float sinY = std::sin(yaw);
    const float cosX = std::cos(pitch);
    const float sinX = std::sin(pitch);

    const float rx = x * cosY + z * sinY;
    const float rz = -x * sinY + z * cosY;
    return {
        rx,
        y * cosX - rz * sinX,
        y * sinX + rz * cosX
    };
}

float premiumStarRadius(float angle) {
    const float wave = 0.5f + 0.5f * std::cos(5.0f * (angle + kPi * 0.5f));
    const float point = std::pow(std::max(0.0f, wave), 1.38f);
    const float secondary = std::pow(std::max(0.0f, wave), 4.2f);
    return 0.405f + point * 0.405f + secondary * 0.050f;
}

void buildPremiumStarBase(StarRenderTexture& texture) {
    if (texture.baseReady) {
        return;
    }

    texture.width = kStarTextureSize;
    texture.height = kStarTextureSize;
    const int count = texture.width * texture.height;
    texture.pixels.assign(static_cast<std::size_t>(count) * 4u, 0u);
    texture.alpha.assign(static_cast<std::size_t>(count), 0.0f);
    texture.glow.assign(static_cast<std::size_t>(count), 0.0f);
    texture.heightMap.assign(static_cast<std::size_t>(count), 0.0f);
    texture.edge.assign(static_cast<std::size_t>(count), 0.0f);
    texture.normalX.assign(static_cast<std::size_t>(count), 0.0f);
    texture.normalY.assign(static_cast<std::size_t>(count), 0.0f);
    texture.normalZ.assign(static_cast<std::size_t>(count), 1.0f);

    for (int y = 0; y < texture.height; ++y) {
        for (int x = 0; x < texture.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(texture.width) * 2.0f - 1.0f;
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(texture.height) * 2.0f - 1.0f;
            const float radius = std::sqrt(u * u + v * v);
            const float angle = std::atan2(v, u);
            const float starRadius = premiumStarRadius(angle);
            const float signedDistance = radius - starRadius;
            const float alpha = 1.0f - smoothstep(-0.028f, 0.020f, signedDistance);
            const float outsideGlow = std::exp(-std::max(0.0f, signedDistance) * std::max(0.0f, signedDistance) * 58.0f);
            const float innerDistance = std::max(0.0f, -signedDistance);
            const float edge = smoothstep(0.0f, 0.175f, innerDistance);
            const float ridge = std::pow(0.5f + 0.5f * std::cos(5.0f * (angle + kPi * 0.5f)), 2.5f);
            const float dome = std::pow(std::max(0.0f, 1.0f - radius * 0.62f), 1.46f);
            const float tipLift = ridge * smoothstep(0.36f, 0.82f, radius / std::max(0.001f, starRadius));
            const float valleyFold = (1.0f - ridge) * smoothstep(0.40f, 0.95f, radius / std::max(0.001f, starRadius));
            const float centerFold = std::pow(std::max(0.0f, 0.5f + 0.5f * std::cos(5.0f * (angle + kPi * 0.5f))), 8.0f) *
                                     smoothstep(0.08f, 0.68f, radius / std::max(0.001f, starRadius));
            const float bevelShelf = smoothstep(0.020f, 0.155f, innerDistance) * (1.0f - smoothstep(0.155f, 0.38f, innerDistance));
            const float height = alpha * std::clamp(0.10f + edge * 0.48f + dome * 0.40f + tipLift * 0.23f +
                                                        centerFold * 0.20f + bevelShelf * 0.14f - valleyFold * 0.055f,
                                                    0.0f,
                                                    1.0f);
            const std::size_t index = static_cast<std::size_t>(y * texture.width + x);
            texture.alpha[index] = alpha;
            texture.glow[index] = outsideGlow * 0.20f * (1.0f - alpha);
            texture.heightMap[index] = height;
            texture.edge[index] = edge;
        }
    }

    for (int y = 0; y < texture.height; ++y) {
        for (int x = 0; x < texture.width; ++x) {
            const int left = std::max(0, x - 1);
            const int right = std::min(texture.width - 1, x + 1);
            const int up = std::max(0, y - 1);
            const int down = std::min(texture.height - 1, y + 1);
            const float dx = texture.heightMap[static_cast<std::size_t>(y * texture.width + right)] -
                             texture.heightMap[static_cast<std::size_t>(y * texture.width + left)];
            const float dy = texture.heightMap[static_cast<std::size_t>(down * texture.width + x)] -
                             texture.heightMap[static_cast<std::size_t>(up * texture.width + x)];
            const core::Vec3 normal = normalize3({-dx * 5.8f, -dy * 5.8f, 1.0f});
            const std::size_t index = static_cast<std::size_t>(y * texture.width + x);
            texture.normalX[index] = normal.x;
            texture.normalY[index] = normal.y;
            texture.normalZ[index] = normal.z;
        }
    }

    texture.baseReady = true;
}

void shadePremiumStar(StarRenderTexture& texture, float pitch, float yaw, float timeSeconds, bool pressed) {
    buildPremiumStarBase(texture);

    core::render::RenderBackend* backend = core::render::activeRenderBackend();
    if (backend == nullptr) {
        return;
    }

    if (texture.handle != nullptr) {
        return;
    }

    const float phase = 0.42f;
    yaw = kStarRestYaw;
    pitch = kStarRestPitch;
    pressed = false;
    (void)timeSeconds;

    const core::Vec3 light = normalize3({-0.42f + yaw * 0.30f, -0.66f - pitch * 0.18f, 1.05f});
    const core::Vec3 view = {0.0f, 0.0f, 1.0f};
    const core::Vec3 halfVector = normalize3({light.x + view.x, light.y + view.y, light.z + view.z});
    const core::Vec3 coolLight = normalize3({0.72f, -0.22f, 0.84f});
    const core::Vec3 warmLight = normalize3({-0.58f, 0.30f, 0.88f});
    const float pressGlow = pressed ? 1.0f : 0.0f;

    for (int y = 0; y < texture.height; ++y) {
        for (int x = 0; x < texture.width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y * texture.width + x);
            const std::size_t pixel = index * 4u;
            const float alpha = texture.alpha[index];
            const float glow = texture.glow[index];
            if (alpha <= 0.001f && glow <= 0.001f) {
                texture.pixels[pixel + 0] = 232u;
                texture.pixels[pixel + 1] = 228u;
                texture.pixels[pixel + 2] = 255u;
                texture.pixels[pixel + 3] = 0u;
                continue;
            }

            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(texture.width) * 2.0f - 1.0f;
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(texture.height) * 2.0f - 1.0f;
            const core::Vec3 normal = normalize3(rotateNormal(texture.normalX[index],
                                                              texture.normalY[index],
                                                              texture.normalZ[index],
                                                              pitch * 0.58f,
                                                              yaw * 0.72f));
            const float diffuse = std::max(0.0f, dot3(normal, light));
            const float broad = std::pow(std::max(0.0f, dot3(normal, halfVector)), 4.2f);
            const float spec = std::pow(std::max(0.0f, dot3(normal, halfVector)), 28.0f);
            const float coolSpec = std::pow(std::max(0.0f, dot3(normal, coolLight)), 11.0f);
            const float warmSpec = std::pow(std::max(0.0f, dot3(normal, warmLight)), 9.0f);
            const float edgeTint = 1.0f - texture.edge[index];
            const float radius = std::sqrt(u * u + v * v);
            const float angle = std::atan2(v, u);
            const float ridge = std::pow(std::max(0.0f, 0.5f + 0.5f * std::cos(5.0f * (angle + kPi * 0.5f))), 3.0f);
            const float valley = std::pow(std::max(0.0f, 1.0f - ridge), 1.28f) * smoothstep(0.18f, 0.86f, radius);
            const float diagonal = 1.0f - smoothstep(0.010f, 0.110f, std::fabs(u * 0.72f + v * 0.48f + 0.11f - yaw * 0.06f));
            const float crossSheen = 1.0f - smoothstep(0.008f, 0.085f, std::fabs(u * -0.50f + v * 0.86f - 0.07f));
            const float topFlash = std::exp(-((u + 0.24f - yaw * 0.10f) * (u + 0.24f - yaw * 0.10f) * 12.0f +
                                             (v + 0.36f + pitch * 0.08f) * (v + 0.36f + pitch * 0.08f) * 18.0f));
            const float coolSheen = std::exp(-((u - 0.32f) * (u - 0.32f) * 16.0f + (v + 0.02f) * (v + 0.02f) * 20.0f));
            const float warmSheen = std::exp(-((u + 0.40f) * (u + 0.40f) * 18.0f + (v - 0.04f) * (v - 0.04f) * 15.0f));
            const float lowerViolet = smoothstep(-0.08f, 0.82f, v + pitch * 0.18f) * (0.070f + edgeTint * 0.052f);
            const float pearlWave = 0.5f + 0.5f * std::sin((u * 5.8f - v * 4.6f) + phase + ridge * 1.25f);
            const float rim = std::pow(std::max(0.0f, 1.0f - normal.z), 1.55f) * 0.20f + edgeTint * 0.15f;
            const float shade = diffuse * 0.20f + broad * 0.16f + spec * 0.34f + coolSpec * 0.18f + warmSpec * 0.12f +
                                topFlash * 0.24f + diagonal * 0.090f + crossSheen * 0.040f + ridge * 0.040f;

            float r = 0.63f + shade * 0.32f + rim * 0.14f + topFlash * 0.12f + warmSheen * 0.13f +
                      warmSpec * 0.12f + pearlWave * 0.032f;
            float g = 0.68f + shade * 0.27f + rim * 0.11f + coolSheen * 0.070f + topFlash * 0.09f +
                      coolSpec * 0.080f + pearlWave * 0.028f;
            float b = 0.91f + shade * 0.16f + rim * 0.15f + coolSheen * 0.15f + diagonal * 0.060f +
                      crossSheen * 0.050f + coolSpec * 0.12f + pearlWave * 0.045f;
            r += pressGlow * 0.025f;
            g += pressGlow * 0.018f;
            b += pressGlow * 0.020f;

            r -= lowerViolet * 0.105f + valley * 0.035f;
            g -= lowerViolet * 0.060f + valley * 0.030f;
            b += lowerViolet * 0.055f - valley * 0.012f;

            if (alpha <= 0.001f) {
                texture.pixels[pixel + 0] = byteFromFloat(0.72f + 0.10f * pearlWave);
                texture.pixels[pixel + 1] = byteFromFloat(0.68f + 0.14f * pearlWave);
                texture.pixels[pixel + 2] = byteFromFloat(1.0f);
                texture.pixels[pixel + 3] = byteFromFloat(glow * (0.18f + pressGlow * 0.10f));
                continue;
            }

            texture.pixels[pixel + 0] = byteFromFloat(r);
            texture.pixels[pixel + 1] = byteFromFloat(g);
            texture.pixels[pixel + 2] = byteFromFloat(b);
            texture.pixels[pixel + 3] = byteFromFloat(std::max(alpha, glow * 0.42f));
        }
    }

    if (texture.handle == nullptr) {
        texture.handle = backend->createTexture(texture.pixels.data(), texture.width, texture.height);
        if (texture.handle != nullptr) {
            texture.drawHoldFrames = 2;
            texture.pixels.clear();
            texture.pixels.shrink_to_fit();
            texture.alpha.clear();
            texture.alpha.shrink_to_fit();
            texture.glow.clear();
            texture.glow.shrink_to_fit();
            texture.heightMap.clear();
            texture.heightMap.shrink_to_fit();
            texture.edge.clear();
            texture.edge.shrink_to_fit();
            texture.normalX.clear();
            texture.normalX.shrink_to_fit();
            texture.normalY.clear();
            texture.normalY.shrink_to_fit();
            texture.normalZ.clear();
            texture.normalZ.shrink_to_fit();
        }
    }
    texture.shadedYaw = yaw;
    texture.shadedPitch = pitch;
    texture.shadedPhase = phase;
    texture.shadedPressed = pressed;
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
        FontAtlas& atlas = runtime.atlases[pixelHeight];
        if (atlas.pixels.empty()) {
            atlas.pixels.assign(static_cast<std::size_t>(atlas.width) * static_cast<std::size_t>(atlas.height) * 4u, 0u);
            for (std::size_t i = 0; i < atlas.pixels.size(); i += 4u) {
                atlas.pixels[i + 0u] = 255u;
                atlas.pixels[i + 1u] = 255u;
                atlas.pixels[i + 2u] = 255u;
            }
        }

        constexpr int padding = 1;
        if (atlas.cursorX + width + padding >= atlas.width) {
            atlas.cursorX = padding;
            atlas.cursorY += atlas.rowHeight + padding;
            atlas.rowHeight = 0;
        }
        if (atlas.cursorY + height + padding < atlas.height) {
            glyph.atlasX = atlas.cursorX;
            glyph.atlasY = atlas.cursorY;
            glyph.u0 = static_cast<float>(glyph.atlasX) / static_cast<float>(atlas.width);
            glyph.v0 = static_cast<float>(glyph.atlasY) / static_cast<float>(atlas.height);
            glyph.u1 = static_cast<float>(glyph.atlasX + width) / static_cast<float>(atlas.width);
            glyph.v1 = static_cast<float>(glyph.atlasY + height) / static_cast<float>(atlas.height);
            glyph.packed = true;

            for (int row = 0; row < height; ++row) {
                for (int col = 0; col < width; ++col) {
                    const std::size_t src = static_cast<std::size_t>(row * width + col);
                    const std::size_t dst = (static_cast<std::size_t>(glyph.atlasY + row) * static_cast<std::size_t>(atlas.width) +
                                             static_cast<std::size_t>(glyph.atlasX + col)) * 4u;
                    atlas.pixels[dst + 3u] = bitmap[src];
                }
            }
            atlas.cursorX += width + padding;
            atlas.rowHeight = std::max(atlas.rowHeight, height + padding);
            atlas.dirty = true;
        } else {
            __android_log_print(ANDROID_LOG_WARN,
                                "NeoPanel",
                                "Font atlas full for pixel height %d; codepoint U+%04X skipped",
                                pixelHeight,
                                static_cast<unsigned int>(codepoint));
        }
    }
    if (bitmap != nullptr) {
        stbtt_FreeBitmap(bitmap, nullptr);
    }

    auto [it, inserted] = runtime.glyphs.emplace(key, glyph);
    (void)inserted;
    return &it->second;
}

bool ensureFontAtlasUploaded(int pixelHeight) {
    FontRuntime& runtime = fontRuntime();
    auto it = runtime.atlases.find(pixelHeight);
    if (it == runtime.atlases.end() || it->second.pixels.empty()) {
        return false;
    }

    FontAtlas& atlas = it->second;
    if (atlas.handle != nullptr && !atlas.dirty) {
        return true;
    }
    core::render::RenderBackend* backend = core::render::activeRenderBackend();
    if (backend == nullptr) {
        return atlas.handle != nullptr;
    }
    if (atlas.handle == nullptr) {
        atlas.handle = backend->createTexture(atlas.pixels.data(), atlas.width, atlas.height);
        atlas.dirty = atlas.handle == nullptr;
    } else if (atlas.dirty) {
        atlas.dirty = !backend->updateTexture(atlas.handle, atlas.pixels.data(), atlas.width, atlas.height);
    }
    return atlas.handle != nullptr;
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

    const char* warm = text;
    std::uint32_t warmCodepoint = 0;
    while (nextCodepoint(warm, warmCodepoint)) {
        if (warmCodepoint != ' ') {
            ensureFontGlyph(warmCodepoint, pixelHeight);
        }
    }
    if (!ensureFontAtlasUploaded(pixelHeight)) {
        return;
    }
    FontAtlas& atlas = runtime.atlases[pixelHeight];

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
        if (glyph->packed && atlas.handle != nullptr && glyph->width > 0 && glyph->height > 0) {
            drawTextureSubQuad(atlas.handle,
                               cursor + glyph->xOffset,
                               baseline + glyph->yOffset,
                               static_cast<float>(glyph->width),
                               static_cast<float>(glyph->height),
                               glyph->u0,
                               glyph->v0,
                               glyph->u1,
                               glyph->v1,
                               rgba(color.r, color.g, color.b, color.a * opacity));
        }
        cursor += glyph->advance;
    }
}

void prepareFontText(const char* text, float scale) {
    if (text == nullptr || !ensureFontRuntime()) {
        return;
    }
    const int pixelHeight = fontPixelHeight(scale);
    const char* p = text;
    std::uint32_t codepoint = 0;
    while (nextCodepoint(p, codepoint)) {
        if (codepoint != ' ') {
            ensureFontGlyph(codepoint, pixelHeight);
        }
    }
    fontTextWidth(text, scale);
}

void uploadDirtyFontAtlases() {
    FontRuntime& runtime = fontRuntime();
    std::vector<int> pixelHeights;
    pixelHeights.reserve(runtime.atlases.size());
    for (const auto& entry : runtime.atlases) {
        if (entry.second.handle == nullptr || entry.second.dirty) {
            pixelHeights.push_back(entry.first);
        }
    }
    for (int pixelHeight : pixelHeights) {
        ensureFontAtlasUploaded(pixelHeight);
    }
}

void prepareStaticResources(PanelState& state) {
    if (state.staticResourcesPrepared) {
        return;
    }

    ensureTextureUploaded(state.avatar);

    for (const NavItem& item : kNavItems) {
        prepareFontText(item.labelEn, 1.20f);
        prepareFontText(item.detailEn, 0.76f);
        prepareFontText(item.labelZh, 1.20f);
        prepareFontText(item.detailZh, 0.76f);
    }
    for (const PageInfo& page : kPages) {
        prepareFontText(page.titleEn, 1.30f);
        prepareFontText(page.subtitleEn, 0.78f);
        prepareFontText(page.tagEn, 0.72f);
        prepareFontText(page.titleZh, 1.30f);
        prepareFontText(page.subtitleZh, 0.78f);
        prepareFontText(page.tagZh, 0.72f);
    }

    constexpr std::array<const char*, 28> hotLabels{{
        "PREMIUM STAR",
        "FLOAT",
        "DEPTH",
        "SINGLE ELF",
        "PEARL FOLD  GLASS STAGE  MOTION FIELD",
        "Soft bloom and depth light stay inside the stage frame.",
        "FPS",
        "CPU",
        "RAM",
        "LOAD",
        "ANDROID SURFACE",
        "VULKAN BACKEND",
        "STATIC STL",
        "EMBEDDED PNG",
        "READY",
        "ACTIVE",
        "LINKED",
        "IN ELF",
        "DEPLOYMENT SHAPE",
        "ROOT ELF",
        "SURFACE",
        "VULKAN",
        "高级版星形深度 光泽与手势动效",
        "动效舞台",
        "单一 ELF",
        "部署形态",
        "安卓表面",
        "运行",
    }};
    for (const char* text : hotLabels) {
        prepareFontText(text, 1.04f);
        prepareFontText(text, 0.84f);
    }
    uploadDirtyFontAtlases();

    state.staticResourcesPrepared = true;
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
        drawTextFit(r.x + 72.0f, r.y + 15.0f, 78.0f, tr(item.labelEn, item.labelZh), 1.54f, textMain(), opacity, 1.02f);
        drawTextFit(r.x + 72.0f, r.y + 48.0f, 78.0f, tr(item.detailEn, item.detailZh), 0.96f, textSoft(), opacity * 0.82f, 0.78f);
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
    drawTextFit(contentX + 28.0f, contentY + 96.0f, contentW - 88.0f, tr(info.subtitleEn, info.subtitleZh), 1.03f, textSoft(), opacity, 0.82f);
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

void drawStarSparkle(float x, float y, float size, const core::Color& color, float opacity) {
    if (opacity <= 0.001f || size <= 0.5f) {
        return;
    }
    drawLine(x, y - size, x, y + size, size * 0.12f, color, opacity);
    drawLine(x - size, y, x + size, y, size * 0.12f, color, opacity);
    drawLine(x - size * 0.46f, y - size * 0.46f, x + size * 0.46f, y + size * 0.46f, size * 0.075f, color, opacity * 0.62f);
    drawLine(x + size * 0.46f, y - size * 0.46f, x - size * 0.46f, y + size * 0.46f, size * 0.075f, color, opacity * 0.62f);
    drawAccentDot(x, y, size * 0.11f, rgba(1.0f, 1.0f, 1.0f, color.a), opacity);
}

void drawStarDustStreak(float x, float y, float height, float opacity, float phase) {
    if (opacity <= 0.001f || height <= 1.0f) {
        return;
    }
    const float shimmer = 0.64f + 0.36f * std::sin(phase);
    drawLine(x,
             y,
             x,
             y + height,
             1.0f,
             rgba(0.88f, 0.76f, 1.0f, (0.10f + shimmer * 0.16f) * opacity),
             1.0f);
    drawAccentDot(x, y + height * 0.22f, 1.2f, rgba(1.0f, 0.94f, 1.0f, 0.18f), opacity * shimmer);
}

void drawPremiumStarParticle(float x,
                             float y,
                             float size,
                             float twinkle,
                             const core::Color& color,
                             float opacity) {
    if (opacity <= 0.001f || size <= 0.25f) {
        return;
    }
    const float shimmer = smoothstep(0.10f, 1.0f, twinkle);
    const float coreOpacity = (0.18f + shimmer * 0.56f) * opacity;
    drawAccentDot(x, y, size * (0.62f + shimmer * 0.34f), color, coreOpacity);
    if (size > 2.2f || shimmer > 0.72f) {
        drawLine(x - size * 1.18f, y, x + size * 1.18f, y, std::max(0.45f, size * 0.11f), color, coreOpacity * 0.46f);
        drawLine(x, y - size * 1.18f, x, y + size * 1.18f, std::max(0.45f, size * 0.11f), color, coreOpacity * 0.46f);
    }
}

void drawSafeStarGlow(float x, float y, float width, float height, float radius, const core::Color& color, float opacity) {
    if (opacity <= 0.001f) {
        return;
    }
    drawRect(x - 24.0f,
             y - 16.0f,
             width + 48.0f,
             height + 32.0f,
             radius + 18.0f,
             rgba(color.r, color.g, color.b, color.a * 0.32f),
             {},
             {},
             {},
             opacity);
    drawRect(x - 10.0f,
             y - 6.0f,
             width + 20.0f,
             height + 12.0f,
             radius + 8.0f,
             rgba(color.r, color.g, color.b, color.a * 0.58f),
             {},
             {},
             {},
             opacity);
    drawRect(x,
             y,
             width,
             height,
             radius,
             color,
             {},
             {},
             {},
             opacity);
}

void drawCenteredStarGlow(const core::Rect& visual,
                          float yaw,
                          float pitch,
                          float pressed,
                          float pulse,
                          float opacity) {
    const float centerX = visual.x + visual.width * 0.5f + yaw * 8.0f;
    const float centerY = visual.y + visual.height * 0.5f + pitch * 6.0f - pressed * 3.0f;
    drawSafeStarGlow(centerX - visual.width * 0.58f,
                     centerY - visual.height * 0.47f,
                     visual.width * 1.16f,
                     visual.height * 0.94f,
                     46.0f,
                     rgba(0.58f, 0.52f, 1.0f, 0.017f + pressed * 0.010f),
                     opacity);
    drawSafeStarGlow(centerX - visual.width * 0.43f - yaw * 3.0f,
                     centerY - visual.height * 0.36f - pitch * 4.0f,
                     visual.width * 0.86f,
                     visual.height * 0.72f,
                     42.0f,
                     rgba(1.0f, 0.82f, 0.98f, 0.010f + pulse * 0.006f),
                     opacity);
}

float starRotationY(float yaw, float spinAngle) {
    return spinAngle + yaw * 0.92f;
}

core::Transform starSurfaceTransform(float yaw, float pitch, float pressed, float depth, float spinAngle = 0.0f) {
    const float depthT = clamp01(depth);
    const float depthCurve = smoothstep(0.0f, 1.0f, depthT);
    const float rotationY = starRotationY(yaw, spinAngle);
    const float sideAmount = std::fabs(std::sin(rotationY));
    const float normalShift = std::sin(rotationY) * depthCurve * (58.0f + sideAmount * 36.0f);
    core::Transform transform;
    transform.origin = {0.5f, 0.5f};
    transform.rotate = yaw * 0.032f + std::sin(spinAngle) * 0.020f;
    transform.rotateX = pitch * 0.78f + std::sin(spinAngle * 0.5f) * 0.030f;
    transform.rotateY = rotationY;
    transform.translate = {
        yaw * (14.0f - depthCurve * 18.0f) - normalShift,
        pitch * (8.0f + depthCurve * 22.0f) - pressed * (3.6f - depthCurve * 1.4f) +
            std::sin(spinAngle * 0.75f) * depthCurve * 3.0f
    };
    transform.translateZ = 84.0f + pressed * 18.0f - depthCurve * (122.0f + sideAmount * 34.0f);
    transform.perspective = 500.0f;
    const float bevel = std::sin(depthCurve * kPi) * (0.010f + sideAmount * 0.010f);
    const float scale = 1.030f + pressed * 0.024f - depthCurve * (0.050f + sideAmount * 0.022f) + bevel;
    transform.scale = {scale, scale};
    return transform;
}

core::Vec2 projectPoint(const core::TransformMatrix& matrix, const core::Vec2& point) {
    const core::Vec3 p = core::transformPointWithW(matrix, point.x, point.y);
    return {p.x, p.y};
}

std::array<core::Vec2, kStarOutlinePointCount> starOutlinePoints(const core::Rect& visual, float radiusScale) {
    std::array<core::Vec2, kStarOutlinePointCount> points{};
    const float centerX = visual.x + visual.width * 0.5f;
    const float centerY = visual.y + visual.height * 0.5f;
    for (int i = 0; i < kStarOutlinePointCount; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kStarOutlinePointCount);
        const float angle = -kPi * 0.5f + t * kPi * 2.0f;
        const float radius = premiumStarRadius(angle) * radiusScale;
        points[static_cast<std::size_t>(i)] = {
            centerX + std::cos(angle) * radius * visual.width * 0.5f,
            centerY + std::sin(angle) * radius * visual.height * 0.5f
        };
    }
    return points;
}

std::array<core::Vec2, kStarOutlinePointCount> starOutlinePoints(const core::Rect& visual) {
    return starOutlinePoints(visual, 0.985f);
}

std::array<core::Vec2, kStarFacetPointCount> starFacetPoints(const core::Rect& visual) {
    std::array<core::Vec2, kStarFacetPointCount> points{};
    const float centerX = visual.x + visual.width * 0.5f;
    const float centerY = visual.y + visual.height * 0.5f;
    for (int i = 0; i < kStarFacetPointCount; ++i) {
        const float angle = -kPi * 0.5f + static_cast<float>(i) * (kPi / 5.0f);
        const float radius = premiumStarRadius(angle) * (i % 2 == 0 ? 0.988f : 0.965f);
        points[static_cast<std::size_t>(i)] = {
            centerX + std::cos(angle) * radius * visual.width * 0.5f,
            centerY + std::sin(angle) * radius * visual.height * 0.5f
        };
    }
    return points;
}

std::array<core::Vec2, kStarOutlinePointCount> projectStarOutline(
    const std::array<core::Vec2, kStarOutlinePointCount>& points,
    const core::TransformMatrix& matrix) {
    std::array<core::Vec2, kStarOutlinePointCount> projected{};
    for (int i = 0; i < kStarOutlinePointCount; ++i) {
        projected[static_cast<std::size_t>(i)] = projectPoint(matrix, points[static_cast<std::size_t>(i)]);
    }
    return projected;
}

std::array<core::Vec2, kStarFacetPointCount> projectStarFacets(
    const std::array<core::Vec2, kStarFacetPointCount>& points,
    const core::TransformMatrix& matrix) {
    std::array<core::Vec2, kStarFacetPointCount> projected{};
    for (int i = 0; i < kStarFacetPointCount; ++i) {
        projected[static_cast<std::size_t>(i)] = projectPoint(matrix, points[static_cast<std::size_t>(i)]);
    }
    return projected;
}

float telegramStarRadiusUnit(float angle) {
    return premiumStarRadius(angle) / 0.86f;
}

float starMeshSurfaceZ(float ring, float angle, bool front) {
    const float radial = clamp01(ring);
    const float wave = std::pow(std::max(0.0f, 0.5f + 0.5f * std::cos(5.0f * (angle + kPi * 0.5f))), 1.8f);
    const float dome = std::pow(std::max(0.0f, 1.0f - radial * 0.92f), 1.75f);
    const float ridge = (0.45f + wave * 0.55f) * std::pow(std::max(0.0f, 1.0f - radial), 0.80f);
    const float sign = front ? 1.0f : -1.0f;
    const float base = front ? 0.018f : 0.014f;
    return sign * (base + dome * (front ? 0.108f : 0.026f) + ridge * (front ? 0.036f : 0.010f));
}

StarMeshVertex makeStarMeshVertex(float ring, float angle, bool front) {
    const float radius = telegramStarRadiusUnit(angle) * ring;
    const float x = std::cos(angle) * radius;
    const float y = std::sin(angle) * radius;
    const float z = starMeshSurfaceZ(ring, angle, front);
    const float sign = front ? 1.0f : -1.0f;
    const float slope = (1.0f - ring) * (front ? 0.62f : 0.28f);
    const core::Vec3 normal = normalize3({x * slope, y * slope, sign * (1.08f - ring * 0.18f)});
    return {{x, y, z}, normal};
}

core::Vec3 rotateStarMeshPoint(const core::Vec3& p, float pitch, float yaw, float roll) {
    const float cosY = std::cos(yaw);
    const float sinY = std::sin(yaw);
    const float cosX = std::cos(pitch);
    const float sinX = std::sin(pitch);
    const float cosZ = std::cos(roll);
    const float sinZ = std::sin(roll);

    const float yx = p.x * cosY + p.z * sinY;
    const float yz = -p.x * sinY + p.z * cosY;
    const float xy = p.y * cosX - yz * sinX;
    const float xz = p.y * sinX + yz * cosX;
    return {
        yx * cosZ - xy * sinZ,
        yx * sinZ + xy * cosZ,
        xz
    };
}

StarProjectedVertex projectStarMeshVertex(const StarMeshVertex& vertex,
                                          const core::Rect& visual,
                                          float pitch,
                                          float yaw,
                                          float roll,
                                          float pressed,
                                          float floatPhase) {
    const core::Vec3 rotated = rotateStarMeshPoint(vertex.position, pitch, yaw, roll);
    const core::Vec3 normal = normalize3(rotateStarMeshPoint(vertex.normal, pitch, yaw, roll));
    const float scale = visual.width * (0.53f + pressed * 0.010f);
    const float perspective = 3.15f;
    const float cameraZ = perspective - rotated.z;
    const float perspectiveScale = perspective / std::max(0.35f, cameraZ);
    const float centerX = visual.x + visual.width * 0.5f + std::sin(floatPhase) * 2.0f;
    const float centerY = visual.y + visual.height * 0.5f - pressed * 2.2f + std::cos(floatPhase * 0.76f) * 1.4f;
    return {
        {centerX + rotated.x * scale * perspectiveScale,
         centerY + rotated.y * scale * perspectiveScale},
        rotated,
        normal,
        rotated.z
    };
}

core::Color shadeStarMeshTriangle(const StarProjectedVertex& a,
                                  const StarProjectedVertex& b,
                                  const StarProjectedVertex& c,
                                  StarMeshMaterial material,
                                  float pressed,
                                  float phase) {
    const core::Vec3 faceNormal = normalize3(cross3({b.world.x - a.world.x, b.world.y - a.world.y, b.world.z - a.world.z},
                                                    {c.world.x - a.world.x, c.world.y - a.world.y, c.world.z - a.world.z}));
    const core::Vec3 avgNormal = normalize3(add3(add3(a.normal, b.normal), c.normal));
    const core::Vec3 normal = normalize3(add3({faceNormal.x * 0.45f, faceNormal.y * 0.45f, faceNormal.z * 0.45f},
                                              {avgNormal.x * 0.55f, avgNormal.y * 0.55f, avgNormal.z * 0.55f}));
    const core::Vec3 lightKey = normalize3({-0.55f, -0.72f, 1.25f});
    const core::Vec3 lightCool = normalize3({0.80f, -0.18f, 0.58f});
    const core::Vec3 lightWarm = normalize3({-0.80f, 0.42f, 0.72f});
    const core::Vec3 view = {0.0f, 0.0f, 1.0f};
    const core::Vec3 halfVector = normalize3(add3(lightKey, view));
    const float diffuse = std::max(0.0f, dot3(normal, lightKey));
    const float cool = std::max(0.0f, dot3(normal, lightCool));
    const float warm = std::max(0.0f, dot3(normal, lightWarm));
    const float spec = std::pow(std::max(0.0f, dot3(normal, halfVector)), 26.0f);
    const float rim = std::pow(std::max(0.0f, 1.0f - std::fabs(dot3(normal, view))), 1.55f);
    const float fresnel = std::pow(std::max(0.0f, 1.0f - std::max(0.0f, dot3(normal, view))), 2.2f);
    const float shimmer = 0.5f + 0.5f * std::sin((a.world.x + b.world.x + c.world.x) * 4.2f -
                                                 (a.world.y + b.world.y + c.world.y) * 3.1f + phase);
    const float pearlWave = 0.5f + 0.5f * std::sin((a.world.x + b.world.x + c.world.x) * 7.4f +
                                                   (a.world.z + b.world.z + c.world.z) * 18.0f -
                                                   phase * 1.35f);

    core::Color base = rgba(0.88f, 0.84f, 0.99f, 1.0f);
    core::Color pearl = rgba(1.0f, 0.965f, 1.0f, 1.0f);
    if (material == StarMeshMaterial::FrontGlow) {
        base = rgba(0.94f, 0.90f, 1.0f, 1.0f);
        pearl = rgba(1.0f, 0.985f, 1.0f, 1.0f);
    } else if (material == StarMeshMaterial::Side) {
        base = rgba(0.84f, 0.78f, 0.98f, 1.0f);
        pearl = rgba(0.985f, 0.955f, 1.0f, 1.0f);
    } else if (material == StarMeshMaterial::Back || material == StarMeshMaterial::BackGlow) {
        base = rgba(0.86f, 0.80f, 0.98f, 1.0f);
        pearl = rgba(0.99f, 0.955f, 1.0f, 1.0f);
    }

    const float materialBoost = material == StarMeshMaterial::FrontGlow ? 0.30f :
                                material == StarMeshMaterial::Back ? 0.16f :
                                material == StarMeshMaterial::BackGlow ? 0.26f : 0.0f;
    const float pearlMix = std::clamp(0.38f + diffuse * 0.38f + spec * 0.35f + rim * 0.24f + materialBoost,
                                      0.0f,
                                      1.0f);
    core::Color color = mix(base, pearl, pearlMix);
    const float backSheen = material == StarMeshMaterial::Back || material == StarMeshMaterial::BackGlow
        ? std::pow(std::max(0.0f, 1.0f - std::sqrt((a.world.x + b.world.x + c.world.x) * (a.world.x + b.world.x + c.world.x) * 0.035f +
                                                   (a.world.y + b.world.y + c.world.y) * (a.world.y + b.world.y + c.world.y) * 0.035f)),
                   1.6f)
        : 0.0f;
    const float frontGlow = material == StarMeshMaterial::FrontGlow ? 0.11f + pearlWave * 0.055f : 0.0f;
    const float sideFresnel = material == StarMeshMaterial::Side ? fresnel * 0.18f : 0.0f;
    color.r = std::clamp(color.r + warm * 0.050f + spec * 0.16f + shimmer * 0.018f + pearlWave * 0.014f +
                             backSheen * 0.055f + sideFresnel + frontGlow + pressed * 0.018f,
                         0.0f,
                         1.0f);
    color.g = std::clamp(color.g + diffuse * 0.040f + spec * 0.13f + shimmer * 0.014f + pearlWave * 0.012f +
                             backSheen * 0.045f + sideFresnel * 0.82f + frontGlow * 0.92f + pressed * 0.012f,
                         0.0f,
                         1.0f);
    color.b = std::clamp(color.b + cool * 0.060f + rim * 0.070f + shimmer * 0.026f + pearlWave * 0.020f +
                             backSheen * 0.050f + sideFresnel * 0.94f + frontGlow + pressed * 0.016f,
                         0.0f,
                         1.0f);
    color.a = 1.0f;
    return color;
}

void pushStarMeshTriangle(std::vector<StarMeshTriangle>& triangles,
                          const StarProjectedVertex& a,
                          const StarProjectedVertex& b,
                          const StarProjectedVertex& c,
                          StarMeshMaterial material,
                          float pressed,
                          float phase,
                          float opacity) {
    const float area = (b.screen.x - a.screen.x) * (c.screen.y - a.screen.y) -
                       (b.screen.y - a.screen.y) * (c.screen.x - a.screen.x);
    if (std::fabs(area) < 0.005f) {
        return;
    }
    const core::Vec3 faceNormal = normalize3(cross3({b.world.x - a.world.x, b.world.y - a.world.y, b.world.z - a.world.z},
                                                    {c.world.x - a.world.x, c.world.y - a.world.y, c.world.z - a.world.z}));
    if (material == StarMeshMaterial::Front && faceNormal.z < -0.045f) {
        return;
    }
    StarMeshTriangle tri;
    tri.a = a;
    tri.b = b;
    tri.c = c;
    tri.color = shadeStarMeshTriangle(a, b, c, material, pressed, phase);
    const float materialOpacity = material == StarMeshMaterial::Front ? 1.0f :
                                  material == StarMeshMaterial::FrontGlow ? 0.46f :
                                  material == StarMeshMaterial::Side ? 1.0f :
                                  material == StarMeshMaterial::BackGlow ? 0.58f : 0.980f;
    tri.opacity = opacity * materialOpacity;
    tri.sortDepth = (a.depth + b.depth + c.depth) / 3.0f;
    triangles.push_back(tri);
}

void drawPremiumStarMeshModel(const core::Rect& visual,
                              float yaw,
                              float pitch,
                              float pressed,
                              float opacity,
                              float spinAngle,
                              float timeSeconds) {
    if (opacity <= 0.001f) {
        return;
    }

    const float rotationY = spinAngle + yaw * 0.96f;
    const float rotationX = pitch * 0.70f + std::sin(timeSeconds * 0.62f) * 0.020f;
    const float roll = yaw * 0.020f + std::sin(timeSeconds * 0.44f) * 0.016f;
    const float phase = timeSeconds * 1.24f + spinAngle * 0.60f;
    static thread_local std::vector<StarMeshTriangle> triangles;
    triangles.clear();
    triangles.reserve(2200);

    std::array<std::array<StarProjectedVertex, kStarMeshSegmentCount>, kStarMeshRingCount + 1> front{};
    std::array<std::array<StarProjectedVertex, kStarMeshSegmentCount>, kStarMeshBackRingCount + 1> back{};
    for (int ring = 0; ring <= kStarMeshRingCount; ++ring) {
        const float r = static_cast<float>(ring) / static_cast<float>(kStarMeshRingCount);
        for (int i = 0; i < kStarMeshSegmentCount; ++i) {
            const float angle = -kPi * 0.5f + static_cast<float>(i) / static_cast<float>(kStarMeshSegmentCount) * kPi * 2.0f;
            front[static_cast<std::size_t>(ring)][static_cast<std::size_t>(i)] =
                projectStarMeshVertex(makeStarMeshVertex(r, angle, true), visual, rotationX, rotationY, roll, pressed, phase);
        }
    }
    for (int ring = 0; ring <= kStarMeshBackRingCount; ++ring) {
        const float r = static_cast<float>(ring) / static_cast<float>(kStarMeshBackRingCount);
        for (int i = 0; i < kStarMeshSegmentCount; ++i) {
            const float angle = -kPi * 0.5f + static_cast<float>(i) / static_cast<float>(kStarMeshSegmentCount) * kPi * 2.0f;
            back[static_cast<std::size_t>(ring)][static_cast<std::size_t>(i)] =
                projectStarMeshVertex(makeStarMeshVertex(r, angle, false), visual, rotationX, rotationY, roll, pressed, phase);
        }
    }

    for (int ring = 0; ring < kStarMeshBackRingCount; ++ring) {
        for (int i = 0; i < kStarMeshSegmentCount; ++i) {
            const int next = (i + 1) % kStarMeshSegmentCount;
            const auto& a = back[static_cast<std::size_t>(ring)][static_cast<std::size_t>(i)];
            const auto& b = back[static_cast<std::size_t>(ring + 1)][static_cast<std::size_t>(i)];
            const auto& c = back[static_cast<std::size_t>(ring + 1)][static_cast<std::size_t>(next)];
            const auto& d = back[static_cast<std::size_t>(ring)][static_cast<std::size_t>(next)];
            pushStarMeshTriangle(triangles, a, b, c, StarMeshMaterial::Back, pressed, phase, opacity * 0.98f);
            pushStarMeshTriangle(triangles, a, c, d, StarMeshMaterial::Back, pressed, phase, opacity * 0.98f);
            if (ring < kStarMeshBackRingCount - 1) {
                pushStarMeshTriangle(triangles, a, b, c, StarMeshMaterial::BackGlow, pressed, phase + 0.8f, opacity * 0.16f);
                pushStarMeshTriangle(triangles, a, c, d, StarMeshMaterial::BackGlow, pressed, phase + 0.8f, opacity * 0.16f);
            }
        }
    }

    for (int i = 0; i < kStarMeshSegmentCount; ++i) {
        const int next = (i + 1) % kStarMeshSegmentCount;
        const float angleA = -kPi * 0.5f + static_cast<float>(i) / static_cast<float>(kStarMeshSegmentCount) * kPi * 2.0f;
        const float angleB = -kPi * 0.5f + static_cast<float>(next) / static_cast<float>(kStarMeshSegmentCount) * kPi * 2.0f;
        for (int band = 0; band < kStarMeshSideBandCount; ++band) {
            const float t0 = static_cast<float>(band) / static_cast<float>(kStarMeshSideBandCount);
            const float t1 = static_cast<float>(band + 1) / static_cast<float>(kStarMeshSideBandCount);
            const float bevel0 = smoothstep(0.0f, 1.0f, t0);
            const float bevel1 = smoothstep(0.0f, 1.0f, t1);
            StarMeshVertex va = makeStarMeshVertex(1.0f - std::sin(t0 * kPi) * 0.016f, angleA, true);
            StarMeshVertex vb = makeStarMeshVertex(1.0f - std::sin(t0 * kPi) * 0.016f, angleB, true);
            StarMeshVertex vc = makeStarMeshVertex(1.0f - std::sin(t1 * kPi) * 0.016f, angleB, true);
            StarMeshVertex vd = makeStarMeshVertex(1.0f - std::sin(t1 * kPi) * 0.016f, angleA, true);
            const float za0 = starMeshSurfaceZ(1.0f, angleA, true) * (1.0f - bevel0) + starMeshSurfaceZ(1.0f, angleA, false) * bevel0;
            const float zb0 = starMeshSurfaceZ(1.0f, angleB, true) * (1.0f - bevel0) + starMeshSurfaceZ(1.0f, angleB, false) * bevel0;
            const float zc1 = starMeshSurfaceZ(1.0f, angleB, true) * (1.0f - bevel1) + starMeshSurfaceZ(1.0f, angleB, false) * bevel1;
            const float zd1 = starMeshSurfaceZ(1.0f, angleA, true) * (1.0f - bevel1) + starMeshSurfaceZ(1.0f, angleA, false) * bevel1;
            va.position.z = za0;
            vb.position.z = zb0;
            vc.position.z = zc1;
            vd.position.z = zd1;
            const core::Vec3 edgeNormalA = normalize3({std::cos(angleA) * 0.78f, std::sin(angleA) * 0.78f, 0.18f - t0 * 0.36f});
            const core::Vec3 edgeNormalB = normalize3({std::cos(angleB) * 0.78f, std::sin(angleB) * 0.78f, 0.18f - t0 * 0.36f});
            va.normal = edgeNormalA;
            vb.normal = edgeNormalB;
            vc.normal = normalize3({std::cos(angleB) * 0.78f, std::sin(angleB) * 0.78f, 0.18f - t1 * 0.36f});
            vd.normal = normalize3({std::cos(angleA) * 0.78f, std::sin(angleA) * 0.78f, 0.18f - t1 * 0.36f});
            const StarProjectedVertex a = projectStarMeshVertex(va, visual, rotationX, rotationY, roll, pressed, phase);
            const StarProjectedVertex b = projectStarMeshVertex(vb, visual, rotationX, rotationY, roll, pressed, phase);
            const StarProjectedVertex c = projectStarMeshVertex(vc, visual, rotationX, rotationY, roll, pressed, phase);
            const StarProjectedVertex d = projectStarMeshVertex(vd, visual, rotationX, rotationY, roll, pressed, phase);
            pushStarMeshTriangle(triangles, a, b, c, StarMeshMaterial::Side, pressed, phase, opacity);
            pushStarMeshTriangle(triangles, a, c, d, StarMeshMaterial::Side, pressed, phase, opacity);
        }
    }

    for (int ring = 0; ring < kStarMeshRingCount; ++ring) {
        for (int i = 0; i < kStarMeshSegmentCount; ++i) {
            const int next = (i + 1) % kStarMeshSegmentCount;
            const auto& a = front[static_cast<std::size_t>(ring)][static_cast<std::size_t>(i)];
            const auto& b = front[static_cast<std::size_t>(ring + 1)][static_cast<std::size_t>(i)];
            const auto& c = front[static_cast<std::size_t>(ring + 1)][static_cast<std::size_t>(next)];
            const auto& d = front[static_cast<std::size_t>(ring)][static_cast<std::size_t>(next)];
            pushStarMeshTriangle(triangles, a, b, c, StarMeshMaterial::Front, pressed, phase, opacity);
            pushStarMeshTriangle(triangles, a, c, d, StarMeshMaterial::Front, pressed, phase, opacity);
            if (ring < kStarMeshRingCount - 2) {
                const float glowOpacity = opacity * (0.12f + (1.0f - static_cast<float>(ring) / static_cast<float>(kStarMeshRingCount)) * 0.12f);
                pushStarMeshTriangle(triangles, a, b, c, StarMeshMaterial::FrontGlow, pressed, phase + 1.6f, glowOpacity);
                pushStarMeshTriangle(triangles, a, c, d, StarMeshMaterial::FrontGlow, pressed, phase + 1.6f, glowOpacity);
            }
        }
    }

    std::sort(triangles.begin(), triangles.end(), [](const StarMeshTriangle& a, const StarMeshTriangle& b) {
        return a.sortDepth < b.sortDepth;
    });

    for (const StarMeshTriangle& tri : triangles) {
        const std::array<core::Vec2, 3> points{{tri.a.screen, tri.b.screen, tri.c.screen}};
        drawPolygonAbs(points, tri.color, tri.opacity);
    }

    const auto& rim = front[kStarMeshRingCount];
    const auto& backRim = back[kStarMeshBackRingCount];
    for (int i = 0; i < kStarMeshSegmentCount; i += 2) {
        const int next = (i + 2) % kStarMeshSegmentCount;
        const float angle = -kPi * 0.5f + (static_cast<float>(i) + 1.0f) / static_cast<float>(kStarMeshSegmentCount) * kPi * 2.0f;
        const float glint = std::clamp(0.32f + std::cos(angle - rotationY) * 0.38f + std::sin(phase + angle * 2.0f) * 0.12f,
                                       0.0f,
                                       1.0f);
        drawLine(rim[static_cast<std::size_t>(i)].screen.x,
                 rim[static_cast<std::size_t>(i)].screen.y,
                 rim[static_cast<std::size_t>(next)].screen.x,
                 rim[static_cast<std::size_t>(next)].screen.y,
                 0.85f + glint * 1.15f,
                 rgba(1.0f, 0.96f, 1.0f, 0.72f),
                 (0.020f + glint * 0.085f + pressed * 0.018f) * opacity);
        const float backGlint = std::clamp(0.26f - std::cos(angle - rotationY) * 0.30f + std::sin(phase * 0.7f + angle) * 0.08f,
                                           0.0f,
                                           1.0f);
        drawLine(backRim[static_cast<std::size_t>(i)].screen.x,
                 backRim[static_cast<std::size_t>(i)].screen.y,
                 backRim[static_cast<std::size_t>(next)].screen.x,
                 backRim[static_cast<std::size_t>(next)].screen.y,
                 0.70f + backGlint * 0.90f,
                 rgba(0.98f, 0.92f, 1.0f, 0.62f),
                 (0.018f + backGlint * 0.066f + pressed * 0.012f) * opacity);
    }

    const auto& center = front[0][0];
    for (int i = 0; i < kStarMeshSegmentCount; i += 8) {
        const auto& p = front[kStarMeshRingCount][static_cast<std::size_t>(i)];
        const float spoke = 0.5f + 0.5f * std::sin(phase + static_cast<float>(i) * 0.37f);
        drawLine(center.screen.x,
                 center.screen.y,
                 p.screen.x,
                 p.screen.y,
                 0.70f + spoke * 0.45f,
                 rgba(1.0f, 0.94f, 1.0f, 0.50f),
                 (0.005f + spoke * 0.014f) * opacity);
    }

    const core::Vec2 flash = front[2][static_cast<std::size_t>(kStarMeshSegmentCount / 10)].screen;
    drawStarSparkle(flash.x, flash.y, 8.0f + pressed * 1.4f, rgba(1.0f, 0.97f, 1.0f, 0.88f), 0.20f * opacity);
}

void drawStarVolumeBackCap(const core::Rect& visual, float yaw, float pitch, float pressed, float opacity, float spinAngle) {
    if (opacity <= 0.001f) {
        return;
    }

    const auto facets = starFacetPoints(visual);
    const core::TransformMatrix backMatrix = matrixForTransform(visual, starSurfaceTransform(yaw, pitch, pressed, 1.0f, spinAngle));
    const auto back = projectStarFacets(facets, backMatrix);
    const core::Vec2 center = projectPoint(backMatrix, {visual.x + visual.width * 0.5f, visual.y + visual.height * 0.5f});
    const float rotationY = starRotationY(yaw, spinAngle);
    const float sideAmount = std::fabs(std::sin(rotationY));

    for (int i = 0; i < kStarFacetPointCount; ++i) {
        const int next = (i + 1) % kStarFacetPointCount;
        const float angle = -kPi * 0.5f + (static_cast<float>(i) + 0.5f) * (kPi / 5.0f);
        const float facing = std::clamp(0.50f + (-std::sin(rotationY) * std::cos(angle) + pitch * std::sin(angle) * 0.50f) * 0.58f,
                                        0.0f,
                                        1.0f);
        const core::Color backColor = mix(rgba(0.48f, 0.40f, 0.72f, 1.0f),
                                          rgba(0.90f, 0.82f, 1.0f, 1.0f),
                                          std::pow(facing, 0.92f));
        const std::array<core::Vec2, 3> tri{{
            center,
            back[static_cast<std::size_t>(i)],
            back[static_cast<std::size_t>(next)]
        }};
        drawPolygonAbs(tri,
                       rgba(backColor.r,
                            backColor.g,
                            backColor.b,
                            0.018f + sideAmount * 0.030f + facing * 0.036f + pressed * 0.012f),
                       opacity);
    }
}

void drawStarSolidVolume(const core::Rect& visual, float yaw, float pitch, float pressed, float opacity, float spinAngle) {
    if (opacity <= 0.001f) {
        return;
    }

    const float rotationY = starRotationY(yaw, spinAngle);
    const float sideAmount = std::fabs(std::sin(rotationY));
    const auto outline = starOutlinePoints(visual, 0.998f);
    std::array<std::array<core::Vec2, kStarOutlinePointCount>, kStarVolumeLayerCount> shell{};

    for (int layer = 0; layer < kStarVolumeLayerCount; ++layer) {
        const float depth = static_cast<float>(layer) / static_cast<float>(kStarVolumeLayerCount - 1);
        const core::TransformMatrix matrix = matrixForTransform(visual, starSurfaceTransform(yaw, pitch, pressed, depth, spinAngle));
        shell[static_cast<std::size_t>(layer)] = projectStarOutline(outline, matrix);
    }

    drawStarVolumeBackCap(visual, yaw, pitch, pressed, opacity, spinAngle);

    for (int layer = kStarVolumeLayerCount - 2; layer >= 0; --layer) {
        const auto& frontLayer = shell[static_cast<std::size_t>(layer)];
        const auto& backLayer = shell[static_cast<std::size_t>(layer + 1)];
        const float depthMid = (static_cast<float>(layer) + 0.5f) / static_cast<float>(kStarVolumeLayerCount - 1);
        const float depthLight = 1.0f - depthMid * 0.30f;

        for (int i = 0; i < kStarOutlinePointCount; i += kStarVolumeStride) {
            const int next = (i + kStarVolumeStride) % kStarOutlinePointCount;
            const float t = (static_cast<float>(i) + static_cast<float>(kStarVolumeStride) * 0.5f) /
                            static_cast<float>(kStarOutlinePointCount);
            const float angle = -kPi * 0.5f + t * kPi * 2.0f;
            const float nx = std::cos(angle);
            const float ny = std::sin(angle);
            const float facing = std::clamp(0.46f + (-std::sin(rotationY) * nx + pitch * ny * 0.55f) * 0.90f,
                                            0.0f,
                                            1.0f);
            const float highlight = std::pow(std::clamp(0.34f + facing * 0.42f + sideAmount * 0.44f - depthMid * 0.18f,
                                                        0.0f,
                                                        1.0f),
                                             1.20f);
            const core::Color shadow = rgba(0.48f, 0.40f, 0.72f, 1.0f);
            const core::Color mid = rgba(0.78f, 0.68f, 0.98f, 1.0f);
            const core::Color pearl = rgba(1.0f, 0.97f, 1.0f, 1.0f);
            const core::Color sideColor = mix(mix(shadow, mid, facing), pearl, highlight);
            const float alpha = (0.020f + sideAmount * 0.062f + facing * 0.070f + highlight * 0.050f + pressed * 0.018f) *
                                depthLight;
            const std::array<core::Vec2, 4> quad{{
                backLayer[static_cast<std::size_t>(i)],
                backLayer[static_cast<std::size_t>(next)],
                frontLayer[static_cast<std::size_t>(next)],
                frontLayer[static_cast<std::size_t>(i)]
            }};
            drawPolygonAbs(quad, rgba(sideColor.r, sideColor.g, sideColor.b, alpha), opacity);
        }
    }

    const auto innerOutline = starOutlinePoints(visual, 0.54f);
    const core::TransformMatrix innerFrontMatrix = matrixForTransform(visual, starSurfaceTransform(yaw, pitch, pressed, 0.18f, spinAngle));
    const core::TransformMatrix innerBackMatrix = matrixForTransform(visual, starSurfaceTransform(yaw, pitch, pressed, 0.86f, spinAngle));
    const auto innerFront = projectStarOutline(innerOutline, innerFrontMatrix);
    const auto innerBack = projectStarOutline(innerOutline, innerBackMatrix);
    const float spineAlpha = sideAmount * (0.060f + pressed * 0.018f);

    for (int i = 0; i < kStarOutlinePointCount; i += 4) {
        const int next = (i + 4) % kStarOutlinePointCount;
        const float t = (static_cast<float>(i) + 2.0f) / static_cast<float>(kStarOutlinePointCount);
        const float angle = -kPi * 0.5f + t * kPi * 2.0f;
        const float facing = std::clamp(0.48f + (-std::sin(rotationY) * std::cos(angle) + pitch * std::sin(angle) * 0.45f) * 0.84f,
                                        0.0f,
                                        1.0f);
        const core::Color ridgeColor = mix(rgba(0.70f, 0.58f, 0.94f, 1.0f),
                                           rgba(1.0f, 0.96f, 1.0f, 1.0f),
                                           std::pow(facing, 0.90f));
        const std::array<core::Vec2, 4> quad{{
            innerBack[static_cast<std::size_t>(i)],
            innerBack[static_cast<std::size_t>(next)],
            innerFront[static_cast<std::size_t>(next)],
            innerFront[static_cast<std::size_t>(i)]
        }};
        drawPolygonAbs(quad, rgba(ridgeColor.r, ridgeColor.g, ridgeColor.b, 0.010f + spineAlpha + facing * 0.034f), opacity);
    }

    const core::Vec2 centerFront = projectPoint(innerFrontMatrix, {visual.x + visual.width * 0.5f, visual.y + visual.height * 0.5f});
    const core::Vec2 centerBack = projectPoint(innerBackMatrix, {visual.x + visual.width * 0.5f, visual.y + visual.height * 0.5f});
    drawLine(centerBack.x,
             centerBack.y,
             centerFront.x,
             centerFront.y,
             8.0f + sideAmount * 6.0f + pressed * 1.5f,
             rgba(1.0f, 0.95f, 1.0f, 0.30f),
             sideAmount * 0.40f * opacity);
    drawLine(centerBack.x,
             centerBack.y,
             centerFront.x,
             centerFront.y,
             2.2f + sideAmount * 2.0f,
             rgba(0.72f, 0.58f, 0.96f, 0.44f),
             sideAmount * 0.28f * opacity);

    const auto& frontRim = shell[0];
    const auto& backRim = shell[kStarVolumeLayerCount - 1];
    for (int i = 0; i < kStarOutlinePointCount; i += kStarVolumeStride) {
        const int next = (i + kStarVolumeStride) % kStarOutlinePointCount;
        const float t = (static_cast<float>(i) + static_cast<float>(kStarVolumeStride) * 0.5f) /
                        static_cast<float>(kStarOutlinePointCount);
        const float angle = -kPi * 0.5f + t * kPi * 2.0f;
        const float facing = std::clamp(0.42f + (-std::sin(rotationY) * std::cos(angle) + pitch * std::sin(angle) * 0.46f) * 0.90f,
                                        0.0f,
                                        1.0f);
        const float frontAlpha = (0.022f + facing * 0.072f + sideAmount * 0.036f + pressed * 0.016f) * opacity;
        drawLine(frontRim[static_cast<std::size_t>(i)].x,
                 frontRim[static_cast<std::size_t>(i)].y,
                 frontRim[static_cast<std::size_t>(next)].x,
                 frontRim[static_cast<std::size_t>(next)].y,
                 0.90f + facing * 0.95f,
                 rgba(1.0f, 0.95f, 1.0f, 0.82f),
                 frontAlpha);
        if (sideAmount > 0.24f) {
            drawLine(backRim[static_cast<std::size_t>(i)].x,
                     backRim[static_cast<std::size_t>(i)].y,
                     backRim[static_cast<std::size_t>(next)].x,
                     backRim[static_cast<std::size_t>(next)].y,
                     0.70f + facing * 0.55f,
                     rgba(0.88f, 0.78f, 1.0f, 0.58f),
                     (0.012f + facing * 0.034f) * sideAmount * opacity);
        }
    }
}

void drawStarFaceFacets(const core::Rect& visual, float yaw, float pitch, float pressed, float opacity, float spinAngle) {
    if (opacity <= 0.001f) {
        return;
    }

    const auto facets = starFacetPoints(visual);
    const core::TransformMatrix matrix = matrixForTransform(visual, starSurfaceTransform(yaw, pitch, pressed, 0.0f, spinAngle));
    const auto front = projectStarFacets(facets, matrix);
    const core::Vec2 center = projectPoint(matrix, {visual.x + visual.width * 0.5f, visual.y + visual.height * 0.5f});
    const float rotationY = starRotationY(yaw, spinAngle);
    const core::Vec3 light = normalize3({-0.46f + std::sin(rotationY) * 0.20f, -0.74f - pitch * 0.12f, 1.0f});

    for (int i = 0; i < kStarFacetPointCount; ++i) {
        const int next = (i + 1) % kStarFacetPointCount;
        const float midAngle = -kPi * 0.5f + (static_cast<float>(i) + 0.5f) * (kPi / 5.0f);
        const bool outerFacet = (i % 2) == 0;
        const core::Vec3 facetNormal = normalize3({std::cos(midAngle) * (outerFacet ? 0.52f : 0.34f) - std::sin(rotationY) * 0.20f,
                                                   std::sin(midAngle) * (outerFacet ? 0.52f : 0.34f) + pitch * 0.16f,
                                                   1.02f});
        const float lit = std::clamp(dot3(facetNormal, light) * 0.5f + 0.5f, 0.0f, 1.0f);
        const float ridgePulse = outerFacet ? 0.018f : 0.0f;
        const float alpha = 0.016f + lit * 0.050f + ridgePulse + pressed * 0.016f;
        const core::Color facetColor = mix(rgba(0.48f, 0.38f, 0.78f, 1.0f),
                                           rgba(1.0f, 0.96f, 1.0f, 1.0f),
                                           std::pow(lit, 1.35f));
        const std::array<core::Vec2, 3> tri{{
            center,
            front[static_cast<std::size_t>(i)],
            front[static_cast<std::size_t>(next)]
        }};
        const float valleyShadow = std::pow(1.0f - lit, 1.22f) * (outerFacet ? 0.050f : 0.068f);
        if (valleyShadow > 0.010f) {
            drawPolygonAbs(tri, rgba(0.30f, 0.22f, 0.48f, valleyShadow + pressed * 0.010f), opacity);
        }
        drawPolygonAbs(tri, rgba(facetColor.r, facetColor.g, facetColor.b, alpha), opacity);

        if (outerFacet || lit > 0.62f) {
            const float lineAlpha = (0.030f + lit * 0.075f + pressed * 0.014f) * opacity;
            drawLine(center.x,
                     center.y,
                     front[static_cast<std::size_t>(i)].x,
                     front[static_cast<std::size_t>(i)].y,
                     outerFacet ? 1.15f : 0.70f,
                     rgba(1.0f, 0.94f, 1.0f, 0.72f),
                     lineAlpha);
        } else {
            const float shadowAlpha = (0.020f + (1.0f - lit) * 0.045f) * opacity;
            drawLine(center.x,
                     center.y,
                     front[static_cast<std::size_t>(i)].x,
                     front[static_cast<std::size_t>(i)].y,
                     0.80f,
                     rgba(0.38f, 0.26f, 0.58f, 0.62f),
                     shadowAlpha);
        }
    }

    for (int i = 0; i < kStarFacetPointCount; ++i) {
        const int next = (i + 1) % kStarFacetPointCount;
        const float edgeAlpha = ((i % 2) == 0 ? 0.045f : 0.026f) * opacity;
        drawLine(front[static_cast<std::size_t>(i)].x,
                 front[static_cast<std::size_t>(i)].y,
                 front[static_cast<std::size_t>(next)].x,
                 front[static_cast<std::size_t>(next)].y,
                 (i % 2) == 0 ? 1.10f : 0.72f,
                 rgba(1.0f, 0.93f, 1.0f, 0.78f),
                 edgeAlpha + pressed * 0.018f * opacity);
    }

    const core::Vec2 flashA = projectPoint(matrix, {visual.x + visual.width * 0.43f, visual.y + visual.height * 0.30f});
    const core::Vec2 flashB = projectPoint(matrix, {visual.x + visual.width * 0.64f, visual.y + visual.height * 0.42f});
    drawStarSparkle(flashA.x, flashA.y, 8.0f + pressed * 1.8f, rgba(1.0f, 0.96f, 1.0f, 0.90f), 0.22f * opacity);
    drawAccentDot(flashB.x, flashB.y, 3.2f + pressed * 0.8f, rgba(0.88f, 0.78f, 1.0f, 0.70f), 0.24f * opacity);
}

void drawStarFace(core::render::RenderBackend::TextureHandle handle,
                  const core::Rect& visual,
                  float yaw,
                  float pitch,
                  float pressed,
                  float opacity,
                  float spinAngle) {
    if (handle == nullptr || opacity <= 0.001f) {
        return;
    }
    const core::TransformMatrix matrix = matrixForTransform(visual, starSurfaceTransform(yaw, pitch, pressed, 0.0f, spinAngle));
    drawTextureQuadMatrix(handle,
                          visual.x,
                          visual.y,
                          visual.width,
                          visual.height,
                          matrix,
                          rgba(1.0f, 1.0f, 1.0f, opacity),
                          0.0f);
    drawTextureQuadMatrix(handle,
                          visual.x,
                          visual.y,
                          visual.width,
                          visual.height,
                          matrix,
                          rgba(1.0f, 0.92f, 1.0f, (0.070f + pressed * 0.045f) * opacity),
                          0.0f);
}

float premiumStarSpinAngle(const PanelState& state) {
    float spinAngle = 0.0f;
    if (state.starEntrySpinTime > 0.0f) {
        const float progress = 1.0f - std::clamp(state.starEntrySpinTime / kStarEntrySpinDuration, 0.0f, 1.0f);
        const float eased = smoothstep(0.0f, 1.0f, progress);
        spinAngle += eased * kPi * 2.0f;
    } else {
        spinAngle += std::sin(state.starIdleTime * 0.54f) * 0.070f;
    }
    return spinAngle;
}

void renderPremiumStarStage(PanelState& state, float contentX, float contentW, float stageY, float opacity) {
    const core::Color accent = pageAccent(2);
    const core::Rect stage = premiumStarStageRect(contentX, contentW, stageY);
    const core::Rect visual = premiumStarVisualRect(contentX, contentW, stageY);
    const float pressed = state.starPressAmount;
    const float pulse = std::sin(state.launchTime * 2.1f) * 0.5f + 0.5f;
    const float spinAngle = premiumStarSpinAngle(state);
    const float entryProgress = state.starEntrySpinTime > 0.0f
        ? 1.0f - std::clamp(state.starEntrySpinTime / kStarEntrySpinDuration, 0.0f, 1.0f)
        : 1.0f;
    const float entryLift = state.starEntrySpinTime > 0.0f ? std::sin(entryProgress * kPi) : 0.0f;
    const float driftYaw = state.starYaw + std::sin(spinAngle) * 0.018f;
    const float driftPitch = state.starPitch + std::cos(spinAngle * 0.7f) * 0.012f - entryLift * 0.050f;

    core::Gradient stageGradient;
    stageGradient.enabled = true;
    stageGradient.start = gNight ? rgba(0.20f, 0.28f, 0.88f, 0.26f * opacity) : rgba(0.48f, 0.58f, 1.0f, 0.16f * opacity);
    stageGradient.end = gNight ? rgba(0.98f, 0.32f, 0.73f, 0.31f * opacity) : rgba(0.98f, 0.46f, 0.72f, 0.19f * opacity);
    stageGradient.direction = core::GradientDirection::Horizontal;
    drawRect(stage.x, stage.y, stage.width, stage.height, 30.0f,
             rgba(accent.r, accent.g, accent.b, (0.076f + pressed * 0.028f) * opacity),
             {1.0f, rgba(1.0f, 1.0f, 1.0f, (0.12f + pressed * 0.06f) * opacity)},
             {true, {0.0f, 18.0f}, 34.0f, 0.0f, rgba(0.10f, 0.04f, 0.15f, 0.16f * opacity)},
             stageGradient);

    drawRect(stage.x + 30.0f, stage.y + 30.0f, stage.width - 60.0f, stage.height - 62.0f, 28.0f,
             rgba(1.0f, 1.0f, 1.0f, 0.026f * opacity),
             {1.0f, rgba(1.0f, 1.0f, 1.0f, 0.044f * opacity)});

    drawCenteredStarGlow(visual, driftYaw, driftPitch, pressed, pulse, opacity);

    for (int i = 0; i < 28; ++i) {
        const float seed = static_cast<float>(i) * 17.0f + 3.0f;
        const float depth = 0.35f + hash01(seed + 11.0f) * 0.95f;
        const float px = stage.x + 34.0f + hash01(seed) * (stage.width - 68.0f);
        const float py = stage.y + 38.0f + hash01(seed + 9.0f) * (stage.height - 86.0f);
        const float twinkle = 0.5f + 0.5f * std::sin(state.launchTime * (1.4f + hash01(seed + 4.0f) * 2.8f) + seed);
        const float driftX = driftYaw * (5.0f + depth * 18.0f) + std::sin(spinAngle + depth) * 1.6f;
        const float driftY = -driftPitch * (4.0f + depth * 12.0f);
        const float size = 0.9f + hash01(seed + 2.0f) * 3.9f;
        const float op = (0.12f + depth * 0.08f) * opacity;
        drawPremiumStarParticle(px + driftX,
                                py + driftY,
                                size,
                                twinkle,
                                rgba(0.96f, 0.90f + hash01(seed + 7.0f) * 0.08f, 1.0f, 0.86f),
                                op);
    }
    for (int i = 0; i < 10; ++i) {
        const float seed = 100.0f + static_cast<float>(i) * 13.0f;
        const float depth = 0.45f + hash01(seed + 6.0f) * 0.80f;
        const float x = stage.x + 56.0f + hash01(seed) * (stage.width - 112.0f) + driftYaw * (7.0f + depth * 15.0f);
        const float y = stage.y + 34.0f + hash01(seed + 2.0f) * (stage.height - 110.0f) - driftPitch * (5.0f + depth * 8.0f);
        const float h = 7.0f + hash01(seed + 3.0f) * 26.0f;
        drawStarDustStreak(x, y, h, (0.045f + hash01(seed + 4.0f) * 0.070f) * opacity, state.launchTime * (0.70f + hash01(seed + 5.0f) * 0.60f) + seed);
    }

    drawSafeStarGlow(visual.x + visual.width * 0.18f + driftYaw * 14.0f,
                     visual.y + visual.height * 0.69f + driftPitch * 8.0f,
                     visual.width * 0.62f,
                     visual.height * 0.12f,
                     32.0f,
                     rgba(0.58f, 0.48f, 0.86f, 0.012f + pressed * 0.007f),
                     opacity);
    drawPremiumStarMeshModel(visual, driftYaw, driftPitch, pressed, opacity, spinAngle, state.launchTime);

    drawStarSparkle(visual.x + visual.width * (0.58f + driftYaw * 0.08f),
                    visual.y + visual.height * (0.31f + driftPitch * 0.06f),
                    13.5f + pressed * 2.0f,
                    rgba(1.0f, 0.95f, 1.0f, 0.90f),
                    0.30f * opacity);
    drawStarSparkle(visual.x + visual.width * 0.35f + driftYaw * 12.0f,
                    visual.y + visual.height * 0.27f + driftPitch * 10.0f,
                    10.5f + pulse * 2.4f,
                    rgba(1.0f, 1.0f, 1.0f, 0.82f),
                    0.28f * opacity);
    drawTextFit(stage.x + 26.0f, stage.y + 20.0f, 190.0f, "PREMIUM STAR", 1.04f, tileText(), opacity, 0.78f);
    drawTextRight(stage.x + stage.width - 26.0f,
                  stage.y + 20.0f,
                  state.starEntrySpinTime > 0.0f ? "SPIN" : (state.pressedStar ? "FLOAT" : "DEPTH"),
                  0.92f,
                  rgba(0.98f, 0.96f, 1.0f, 0.86f),
                  opacity);
}

void renderMotionPage(PanelState& state, float contentX, float contentY, float contentW, float top, float bottom, float scroll, float opacity) {
    (void)contentY;
    const core::Color accent = pageAccent(2);
    const float stageY = top + 14.0f - scroll;
    if (itemVisible(stageY, 284.0f, top, bottom)) {
        renderPremiumStarStage(state, contentX, contentW, stageY, opacity);
    }

    renderEffectTiles(contentX, contentW, 2, top + 328.0f, scroll, top, bottom, opacity);

    const float premiumNoteY = top + 530.0f - scroll;
    if (itemVisible(premiumNoteY, 64.0f, top, bottom)) {
        drawRect(contentX + 30.0f, premiumNoteY, contentW - 60.0f, 64.0f, 18.0f,
                 rgba(accent.r, accent.g, accent.b, 0.12f * opacity),
                 {1.0f, rgba(accent.r, accent.g, accent.b, 0.26f * opacity)});
        drawTextFit(contentX + 52.0f, premiumNoteY + 17.0f, contentW - 116.0f, "PEARL FOLD  GLASS STAGE  MOTION FIELD", 0.98f, tileText(), opacity, 0.74f);
        drawTextFit(contentX + 52.0f, premiumNoteY + 39.0f, contentW - 116.0f, "Soft bloom and depth light stay inside the stage frame.", 0.82f, tileTextSoft(), opacity, 0.64f);
    }
}

void drawSystemExitButton(const core::Rect& r, bool pressed, float opacity) {
    const core::Color accent = pageAccent(3);
    const float press = pressed ? 2.0f : 0.0f;
    const float glow = pressed ? 1.0f : 0.0f;

    core::Gradient fill;
    fill.enabled = true;
    fill.start = rgba(accent.r, accent.g, accent.b, (0.17f + glow * 0.05f) * opacity);
    fill.end = gNight ? rgba(1.0f, 0.42f, 0.57f, (0.080f + glow * 0.030f) * opacity)
                       : rgba(1.0f, 0.70f, 0.58f, (0.11f + glow * 0.02f) * opacity);
    fill.direction = core::GradientDirection::Horizontal;

    drawRect(r.x, r.y + press, r.width, r.height, 22.0f,
             rgba(1.0f, 1.0f, 1.0f, 0.050f * opacity),
             {1.0f, rgba(accent.r, accent.g, accent.b, (0.30f + glow * 0.22f) * opacity)},
             {true, {0.0f, 12.0f}, 34.0f, 0.0f, rgba(accent.r, accent.g, accent.b, (0.12f + glow * 0.08f) * opacity)},
             fill,
             opacity);

    drawAccentDot(r.x + 34.0f, r.y + 32.0f + press, 12.0f, rgba(accent.r, accent.g, accent.b, 0.22f), opacity);
    drawLine(r.x + 29.0f, r.y + 32.0f + press, r.x + 39.0f, r.y + 32.0f + press, 2.5f, rgba(0.98f, 0.99f, 1.0f, 0.92f), opacity);
    drawLine(r.x + 34.0f, r.y + 27.0f + press, r.x + 34.0f, r.y + 37.0f + press, 2.5f, rgba(0.98f, 0.99f, 1.0f, 0.92f), opacity);

    drawTextFit(r.x + 58.0f, r.y + 16.0f + press, 250.0f, "EXIT PANEL", 1.02f, tileText(), opacity, 0.76f);
    drawTextRight(r.x + r.width - 30.0f, r.y + 17.0f + press, "FLY OUT", 0.90f, rgba(accent.r, accent.g, accent.b, 0.96f), opacity);

    const float ax = r.x + r.width - 92.0f;
    const float ay = r.y + 42.0f + press;
    drawLine(ax, ay, ax + 24.0f, ay - 16.0f, 2.2f, rgba(0.94f, 0.96f, 1.0f, 0.46f), opacity);
    drawLine(ax + 24.0f, ay - 16.0f, ax + 20.0f, ay - 5.0f, 2.2f, rgba(0.94f, 0.96f, 1.0f, 0.46f), opacity);
    drawLine(ax + 24.0f, ay - 16.0f, ax + 12.0f, ay - 16.0f, 2.2f, rgba(0.94f, 0.96f, 1.0f, 0.46f), opacity);
}

void renderSystemPage(PanelState& state, float contentX, float contentY, float contentW, float top, float bottom, float scroll, float opacity) {
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

    const core::Rect exitRect = systemExitButtonRect(contentX, contentW, top, scroll);
    if (itemVisible(exitRect.y, exitRect.height, top, bottom)) {
        drawSystemExitButton(exitRect, state.pressedExit, opacity);
    }

    const float stackY = y0 + 306.0f;
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
        return 226.0f;
    }
    return 122.0f;
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
        renderSystemPage(state, contentX, contentY, contentW, scrollTop, scrollBottom, scroll, opacity);
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

void renderExitFlight(float progress) {
    const float t = clamp01(progress);
    const core::Color accent = pageAccent(3);
    const float gather = smoothstep(0.0f, 0.54f, t);
    const float lift = smoothstep(0.32f, 1.0f, t);
    const float fadeOut = 1.0f - smoothstep(0.78f, 1.0f, t);
    const float cx = lerpFloat(kPanelWidth * 0.5f, kPanelWidth * 0.5f - 28.0f, lift) + std::sin(t * kPi) * 18.0f;
    const float cy = lerpFloat(kPanelHeight * 0.5f, -56.0f, lift);
    const float w = lerpFloat(kPanelWidth - 36.0f, 30.0f, gather);
    const float h = lerpFloat(kPanelHeight - 36.0f, 30.0f, gather);
    const float radius = lerpFloat(44.0f, 15.0f, gather);
    const float opacity = fadeOut * (0.42f + gather * 0.58f);

    core::Gradient shell;
    shell.enabled = true;
    shell.start = rgba(accent.r, accent.g, accent.b, (0.26f + gather * 0.18f) * opacity);
    shell.end = rgba(1.0f, 0.52f, 0.66f, (0.14f + gather * 0.18f) * opacity);
    shell.direction = core::GradientDirection::Horizontal;

    if (lift > 0.02f) {
        const float trailOpacity = (1.0f - smoothstep(0.60f, 1.0f, t)) * lift;
        for (int i = 0; i < 4; ++i) {
            const float step = static_cast<float>(i);
            const float ty = cy + 36.0f + step * (20.0f + lift * 10.0f);
            const float tw = 42.0f + step * 18.0f;
            drawRect(cx - tw * 0.5f + step * 5.0f,
                     ty,
                     tw,
                     5.0f,
                     2.5f,
                     rgba(accent.r, accent.g, accent.b, (0.18f - step * 0.028f) * trailOpacity),
                     {},
                     {},
                     {},
                     opacity);
        }
    }

    drawRect(cx - w * 0.5f,
             cy - h * 0.5f,
             w,
             h,
             radius,
             rgba(1.0f, 1.0f, 1.0f, 0.040f * opacity),
             {1.0f, rgba(1.0f, 1.0f, 1.0f, 0.16f * opacity)},
             {true, {0.0f, 18.0f}, 48.0f, 0.0f, rgba(accent.r, accent.g, accent.b, 0.22f * opacity)},
             shell,
             opacity,
             gather * 3.5f);

    const float halo = lerpFloat(92.0f, 52.0f, gather) * fadeOut;
    drawAccentDot(cx, cy, halo, rgba(accent.r, accent.g, accent.b, 0.026f + gather * 0.050f), opacity);
    drawAccentDot(cx, cy, lerpFloat(18.0f, 10.0f, gather), rgba(0.98f, 0.99f, 1.0f, 0.88f), opacity);
    drawAccentDot(cx - 4.0f, cy - 5.0f, 4.0f, rgba(1.0f, 0.82f, 0.95f, 0.82f), opacity);
}

void renderPanel(PanelState& state) {
    const float intro = easeOutBack(state.launchTime / 0.72f);
    float fade = easeOutCubic(state.launchTime / 0.46f);
    const float exitProgress = state.exitAnimationActive
        ? std::clamp(state.exitAnimationTime / kExitAnimationDuration, 0.0f, 1.0f)
        : 0.0f;
    if (state.exitAnimationActive) {
        fade *= 1.0f - smoothstep(0.04f, 0.48f, exitProgress);
    }
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
    if (state.exitAnimationActive) {
        renderExitFlight(exitProgress);
    }
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
    const core::Rect starHitRect = premiumStarHitRect(contentX, contentW, motionStageY);
    const core::Rect starVisualRect = premiumStarVisualRect(contentX, contentW, motionStageY);

    const core::Rect fpsTrackRect = fpsSliderRect(contentX, contentW, scrollTop);
    const core::Rect fpsRect = expandedRect(fpsTrackRect, 18.0f);
    const core::Rect languageRect = expandedRect(languageToggleRect(), 12.0f);
    const core::Rect themeRect = expandedRect(themeToggleRect(), 12.0f);
    const core::Rect exitRect = expandedRect(systemExitButtonRect(contentX, contentW, scrollTop, scrollValue), 8.0f);
    const core::Rect demoButtonArea{contentX + 30.0f, demoY0 + 134.0f, contentW - 60.0f, 82.0f};
    const core::Rect demoSwitchHit = expandedRect(demoSwitchRect(contentX + 30.0f, demoY0 + 224.0f, contentW - 60.0f), 10.0f);
    const core::Rect demoSliderTrack = demoSliderRect(contentX + 30.0f, demoY0 + 342.0f, contentW - 60.0f);
    const core::Rect demoSliderHit = expandedRect(demoSliderTrack, 10.0f);

    if (state.exitAnimationActive) {
        state.exitAnimationTime = std::min(kExitAnimationDuration, state.exitAnimationTime + deltaSeconds);
        state.pressedExit = false;
        state.launchTime += deltaSeconds;
        state.previousDown = pointer.down;
        if (state.exitAnimationTime >= kExitAnimationDuration) {
            gExitRequested = 1;
        }
        return;
    }

    if (state.selected == 2) {
        state.starIdleTime += deltaSeconds;
        state.starEntrySpinTime = std::max(0.0f, state.starEntrySpinTime - deltaSeconds);
    }

    if (pointer.pressedThisFrame) {
        clearPointerCaptures(state);

        if (contains(languageRect, pointer.x, pointer.y)) {
            state.capturedLanguage = true;
        } else if (contains(themeRect, pointer.x, pointer.y)) {
            state.capturedTheme = true;
        } else if (state.selected == 1 && contains(fpsRect, pointer.x, pointer.y)) {
            state.capturedFpsSlider = true;
        } else if (state.selected == 0 && pointer.y >= scrollTop && pointer.y <= scrollBottom) {
            if (contains(demoSliderHit, pointer.x, pointer.y)) {
                state.capturedDemoSlider = true;
            } else if (contains(demoSwitchHit, pointer.x, pointer.y)) {
                state.capturedDemoSwitch = true;
            } else {
                for (int i = 0; i < 3; ++i) {
                    const core::Rect button = expandedRect(demoButtonRect(i, contentX + 30.0f, demoY0 + 144.0f, contentW - 60.0f), 8.0f);
                    if (contains(button, pointer.x, pointer.y)) {
                        state.capturedDemoButton = i;
                        break;
                    }
                }
            }
        } else if (state.selected == 2) {
            if (contains(starHitRect, pointer.x, pointer.y) && pointer.y >= scrollTop - 8.0f && pointer.y <= scrollBottom + 8.0f) {
                state.capturedStar = true;
            }
        } else if (state.selected == 3 && pointer.y >= scrollTop && pointer.y <= scrollBottom && contains(exitRect, pointer.x, pointer.y)) {
            state.capturedExit = true;
        }

        if (!hasPointerCapture(state)) {
            for (int i = 0; i < 4; ++i) {
                if (contains(navRect(i), pointer.x, pointer.y)) {
                    state.capturedNav = i;
                    break;
                }
            }
        }
    } else if (!pointer.down && !pointer.releasedThisFrame) {
        clearPointerCaptures(state);
    }

    bool interacting = hasPointerCapture(state) && (pointer.down || pointer.releasedThisFrame);
    state.pressedLanguage = pointer.down && state.capturedLanguage && contains(languageRect, pointer.x, pointer.y);
    state.pressedTheme = pointer.down && state.capturedTheme && contains(themeRect, pointer.x, pointer.y);
    state.pressedExit = pointer.down && state.capturedExit && contains(exitRect, pointer.x, pointer.y);
    state.pressedDemoButton = -1;
    state.pressedDemoSwitch = pointer.down && state.capturedDemoSwitch && contains(demoSwitchHit, pointer.x, pointer.y);
    state.pressedStar = pointer.down && state.capturedStar;
    state.activeControl = 0;

    if (pointer.down && state.capturedFpsSlider) {
        const float t = std::clamp(static_cast<float>(pointer.x - fpsTrackRect.x) / fpsTrackRect.width, 0.0f, 1.0f);
        gTargetFps = std::round((kFpsMin + (kFpsMax - kFpsMin) * t) / 5.0f) * 5.0f;
        state.activeControl = 1;
        interacting = true;
    }

    if (pointer.releasedThisFrame && state.capturedLanguage && contains(languageRect, pointer.x, pointer.y)) {
        gChinese = !gChinese;
        interacting = true;
    }

    if (pointer.releasedThisFrame && state.capturedTheme && contains(themeRect, pointer.x, pointer.y)) {
        gNight = !gNight;
        interacting = true;
    }

    if (pointer.releasedThisFrame && state.capturedExit && contains(exitRect, pointer.x, pointer.y)) {
        state.exitAnimationActive = true;
        state.exitAnimationTime = 0.0f;
        state.pressedExit = false;
        interacting = true;
    }

    if (state.selected == 0 && pointer.y >= scrollTop && pointer.y <= scrollBottom) {
        if (pointer.down && state.capturedDemoSlider) {
            state.demoSlider = std::clamp(static_cast<float>(pointer.x - demoSliderTrack.x) / demoSliderTrack.width, 0.0f, 1.0f);
            state.activeControl = 2;
            interacting = true;
        }
        if (pointer.releasedThisFrame && state.capturedDemoSwitch && contains(demoSwitchHit, pointer.x, pointer.y)) {
            state.demoSwitch = !state.demoSwitch;
            interacting = true;
        }
        for (int i = 0; i < 3; ++i) {
            const core::Rect button = expandedRect(demoButtonRect(i, contentX + 30.0f, demoY0 + 144.0f, contentW - 60.0f), 8.0f);
            if (pointer.down && state.capturedDemoButton == i && contains(button, pointer.x, pointer.y)) {
                state.pressedDemoButton = i;
                interacting = true;
            }
            if (pointer.releasedThisFrame && state.capturedDemoButton == i && contains(button, pointer.x, pointer.y)) {
                state.demoButton = i;
                interacting = true;
            }
        }
        interacting = interacting || (pointer.down && contains(demoButtonArea, pointer.x, pointer.y));
    }

    if (state.selected == 2) {
        if (pointer.down && state.capturedStar) {
            state.starEntrySpinTime = 0.0f;
            const float centerX = starVisualRect.x + starVisualRect.width * 0.5f;
            const float centerY = starVisualRect.y + starVisualRect.height * 0.5f;
            const float relX = std::clamp(static_cast<float>(pointer.x - centerX) / (starHitRect.width * 0.5f), -1.0f, 1.0f);
            const float relY = std::clamp(static_cast<float>(pointer.y - centerY) / (starHitRect.height * 0.5f), -1.0f, 1.0f);
            state.starTargetYaw = std::clamp(kStarRestYaw + relX * 0.58f, -0.70f, 0.48f);
            state.starTargetPitch = std::clamp(kStarRestPitch - relY * 0.52f, -0.62f, 0.42f);
            interacting = true;
        } else if (!pointer.down) {
            if (state.starEntrySpinTime > 0.0f) {
                state.starTargetYaw = approach(state.starTargetYaw, kStarRestYaw, deltaSeconds, 2.45f);
                state.starTargetPitch = approach(state.starTargetPitch, kStarRestPitch, deltaSeconds, 2.45f);
            } else {
                const float idleYaw = kStarRestYaw + std::sin(state.starIdleTime * 0.62f) * 0.092f;
                const float idlePitch = kStarRestPitch + std::sin(state.starIdleTime * 0.50f + 1.4f) * 0.056f;
                state.starTargetYaw = approach(state.starTargetYaw, idleYaw, deltaSeconds, 1.35f);
                state.starTargetPitch = approach(state.starTargetPitch, idlePitch, deltaSeconds, 1.35f);
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
        if (pointer.down && state.capturedNav == i && contains(navRect(i), pointer.x, pointer.y)) {
            state.pressedNav = i;
            interacting = true;
        }
        if (pointer.releasedThisFrame && state.capturedNav == i && contains(navRect(i), pointer.x, pointer.y)) {
            const int previousPage = state.selected;
            state.selected = i;
            state.targetScroll = 0.0f;
            state.contentScroll = 0.0f;
            state.pressedStar = false;
            if (i == 2 && !state.starStageSeen) {
                state.starStageSeen = true;
                state.starEntrySpinTime = kStarEntrySpinDuration;
                state.starIdleTime = 0.0f;
                state.starTargetYaw = kStarRestYaw;
                state.starTargetPitch = kStarRestPitch;
                state.starYawVelocity = 0.0f;
                state.starPitchVelocity = 0.0f;
            } else if (previousPage != 2 && i == 2) {
                state.starIdleTime = 0.0f;
            }
            interacting = true;
        }
    }

    if (pointer.releasedThisFrame) {
        clearPointerCaptures(state);
    }

    state.targetScroll = std::clamp(state.targetScroll, 0.0f, pageScrollLimit(state.selected));
    state.contentScroll = approach(state.contentScroll, state.targetScroll, deltaSeconds, 13.5f);
    state.navBlend = approach(state.navBlend, static_cast<float>(state.selected), deltaSeconds, 10.0f);
    state.starPressAmount = approach(state.starPressAmount, state.pressedStar ? 1.0f : 0.0f, deltaSeconds, 13.0f);
    if (state.pressedStar) {
        state.starYaw = approach(state.starYaw, state.starTargetYaw, deltaSeconds, 20.0f);
        state.starPitch = approach(state.starPitch, state.starTargetPitch, deltaSeconds, 20.0f);
        state.starYawVelocity = 0.0f;
        state.starPitchVelocity = 0.0f;
    } else {
        state.starYaw = springApproach(state.starYaw, state.starTargetYaw, state.starYawVelocity, deltaSeconds, 58.0f, 11.0f);
        state.starPitch = springApproach(state.starPitch, state.starTargetPitch, state.starPitchVelocity, deltaSeconds, 58.0f, 11.0f);
    }
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

void sleepUntilFrameBudget(double frameStart, double frameEnd) {
    const double targetFrameSeconds = 1.0 / std::max(1.0f, gTargetFps);
    const double targetTime = frameStart + targetFrameSeconds;
    double remainingSeconds = targetTime - frameEnd;
    if (remainingSeconds <= 0.00045) {
        return;
    }

    if (remainingSeconds > 0.0022) {
        std::this_thread::sleep_for(std::chrono::duration<double>(remainingSeconds - 0.0011));
    }

    while (true) {
        remainingSeconds = targetTime - core::window::timeSeconds();
        if (remainingSeconds <= 0.00022) {
            break;
        }
        if (remainingSeconds > 0.00070) {
            std::this_thread::sleep_for(std::chrono::microseconds(180));
        } else {
            std::this_thread::yield();
        }
    }
}

} // namespace

int main() {
    __android_log_print(ANDROID_LOG_INFO, "NeoPanel", "NeoPanel Android ELF started");
    installSignalHandlers();

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
    applyRenderThreadSchedulingHints();

    PanelState state;
    state.previousTime = core::window::timeSeconds();

    while (gExitRequested == 0) {
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
            prepareStaticResources(state);
            renderPanel(state);
        }
        backend->present();

        sleepUntilFrameBudget(now, core::window::timeSeconds());
    }

    __android_log_print(ANDROID_LOG_INFO, "NeoPanel", "NeoPanel Android ELF shutting down");
    destroyFontTextures(*backend);
    if (state.avatar.handle != nullptr) {
        backend->destroyTexture(state.avatar.handle);
        state.avatar.handle = nullptr;
    }
    if (state.starTexture.handle != nullptr) {
        backend->destroyTexture(state.starTexture.handle);
        state.starTexture.handle = nullptr;
    }
    backend.reset();
    core::window::destroyWindow(window);
    core::releaseInputQueue(window);
    return 0;
}
