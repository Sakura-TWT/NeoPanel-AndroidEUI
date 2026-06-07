cmake_minimum_required(VERSION 3.22)
project(AndroidEUI_NeoPanel LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(NEOPANEL_DEFAULT_EUI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/EUI-NE")
if(NOT EXISTS "${NEOPANEL_DEFAULT_EUI_ROOT}/include/eui_neo.h")
    set(NEOPANEL_DEFAULT_EUI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../EUI-NEO-0.4.0")
endif()

set(EUI_ROOT "${NEOPANEL_DEFAULT_EUI_ROOT}" CACHE PATH "EUI-NE source root")
set(EUI_ANDROID_PORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/eui-neo-android-elf-port-kit" CACHE PATH "EUI Android ELF port kit root")

if(NOT EXISTS "${EUI_ROOT}/include/eui_neo.h")
    message(FATAL_ERROR "EUI_ROOT does not point to EUI-NEO-0.4.0")
endif()
if(NOT EXISTS "${EUI_ANDROID_PORT_ROOT}/core/platform/android/native_surface/ANativeWindowCreator.h")
    message(FATAL_ERROR "EUI_ANDROID_PORT_ROOT does not point to EUI-NEO-Android-ELF-Port-Kit")
endif()

find_package(Threads REQUIRED)

set(NEOPANEL_EMBEDDED_AVATAR "${CMAKE_CURRENT_BINARY_DIR}/embedded_avatar.cpp")
set(NEOPANEL_EMBEDDED_FONT "${CMAKE_CURRENT_BINARY_DIR}/embedded_ui_font.cpp")
add_custom_command(
    OUTPUT "${NEOPANEL_EMBEDDED_AVATAR}"
    COMMAND ${CMAKE_COMMAND}
        -DINPUT="${CMAKE_CURRENT_SOURCE_DIR}/picture/avatar.png"
        -DOUTPUT="${NEOPANEL_EMBEDDED_AVATAR}"
        -DSYMBOL=avatarPng
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/picture/avatar.png" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
    VERBATIM
)
add_custom_command(
    OUTPUT "${NEOPANEL_EMBEDDED_FONT}"
    COMMAND ${CMAKE_COMMAND}
        -DINPUT="${EUI_ROOT}/assets/JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf"
        -DOUTPUT="${NEOPANEL_EMBEDDED_FONT}"
        -DSYMBOL=uiFontTtf
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
    DEPENDS "${EUI_ROOT}/assets/JingNanJunJunTi-JinNanJunJunTi-Bold-2.ttf" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
    VERBATIM
)

add_executable(neopanel_android
    app/main.cpp
    app/android_window_backend_panel.cpp
    "${NEOPANEL_EMBEDDED_AVATAR}"
    "${NEOPANEL_EMBEDDED_FONT}"

    "${EUI_ANDROID_PORT_ROOT}/core/render/render_backend.cpp"
    "${EUI_ANDROID_PORT_ROOT}/core/render/vulkan/vulkan_backend.cpp"

    "${EUI_ROOT}/core/render/primitive.cpp"
    "${EUI_ROOT}/core/render/image.cpp"
    "${EUI_ROOT}/core/render/image_source.cpp"
    "${EUI_ROOT}/core/render/stb_image_impl.cpp"
    "${EUI_ROOT}/core/render/vulkan/vulkan_cache.cpp"
    "${EUI_ROOT}/core/render/vulkan/vulkan_primitives.cpp"
    "${EUI_ROOT}/core/render/vulkan/vulkan_image.cpp"
    "${EUI_ROOT}/core/render/vulkan/vulkan_text.cpp"

    "${EUI_ROOT}/core/platform/async.cpp"
    "${EUI_ROOT}/core/platform/network.cpp"
)

target_include_directories(neopanel_android PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${EUI_ANDROID_PORT_ROOT}"
    "${EUI_ROOT}"
    "${EUI_ROOT}/include"
)

target_compile_definitions(neopanel_android PRIVATE
    EUI_WINDOW_BACKEND_ANDROID=1
    EUI_RENDER_BACKEND_VULKAN=1
    VK_USE_PLATFORM_ANDROID_KHR=1
    SURFACE_LOG_TAG="NeoPanelSurface"
)

target_compile_options(neopanel_android PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_SOURCE_DIR}/cmake/vulkan_portability_compat.h>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -ffunction-sections -fdata-sections>
    $<$<COMPILE_LANGUAGE:C>:-ffunction-sections -fdata-sections>
)

target_link_options(neopanel_android PRIVATE
    -Wl,--gc-sections
)

target_link_libraries(neopanel_android PRIVATE
    Threads::Threads
    android
    log
    vulkan
    dl
)
