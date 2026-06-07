# Android arm64 toolchain for running host Ubuntu ARM64 clang with Android NDK sysroot.
# This is useful on aarch64 proot/Ubuntu where official NDK prebuilt clang is x86_64
# and cannot be executed directly.

# Do not use CMAKE_SYSTEM_NAME=Android here: CMake's built-in Android
# platform module expects an executable NDK prebuilt toolchain, which is x86_64
# in the official NDK and cannot run in this ARM64 Ubuntu/proot environment.
# Use a generic cross toolchain and explicitly mark ANDROID for project logic.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_VERSION 26)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(ANDROID TRUE CACHE BOOL "Build for Android/Bionic" FORCE)

set(EUI_ANDROID_NDK "/storage/emulated/0/AndroidNDK/android-ndk-r26d" CACHE PATH "Android NDK root")
set(EUI_ANDROID_SYSROOT "${EUI_ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/sysroot")
set(EUI_ANDROID_RESOURCE_DIR "${EUI_ANDROID_NDK}/toolchains/llvm/prebuilt/linux-x86_64/lib/clang/17")
set(EUI_ANDROID_TRIPLE "aarch64-linux-android26")
set(EUI_ANDROID_LIB_DIR "${EUI_ANDROID_SYSROOT}/usr/lib/aarch64-linux-android")
set(EUI_ANDROID_API_LIB_DIR "${EUI_ANDROID_LIB_DIR}/26")

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

set(CMAKE_SYSROOT "${EUI_ANDROID_SYSROOT}")

set(EUI_ANDROID_COMMON_FLAGS "--target=${EUI_ANDROID_TRIPLE} --sysroot=${EUI_ANDROID_SYSROOT} -resource-dir ${EUI_ANDROID_RESOURCE_DIR}")
set(CMAKE_C_FLAGS_INIT "${EUI_ANDROID_COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${EUI_ANDROID_COMMON_FLAGS}")

set(EUI_ANDROID_COMMON_LINK_FLAGS "--target=${EUI_ANDROID_TRIPLE} --sysroot=${EUI_ANDROID_SYSROOT} -resource-dir ${EUI_ANDROID_RESOURCE_DIR} -fuse-ld=lld --rtlib=compiler-rt -B${EUI_ANDROID_API_LIB_DIR} -L${EUI_ANDROID_API_LIB_DIR} -L${EUI_ANDROID_LIB_DIR}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${EUI_ANDROID_COMMON_LINK_FLAGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${EUI_ANDROID_COMMON_LINK_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${EUI_ANDROID_COMMON_LINK_FLAGS}")

set(CMAKE_FIND_ROOT_PATH "${EUI_ANDROID_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)