cmake_minimum_required(VERSION 3.20)
project(omnidesk24 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Server-only mode: builds just the signaling server with no GUI deps
option(OMNIDESK_SERVER_ONLY "Build only the signaling server (no GUI)" OFF)

# Platform detection
include(cmake/Platform.cmake)

# SIMD flags
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    set(OMNIDESK_X86_64 TRUE)
    add_compile_options(-msse2)
    # AVX2 enabled per-file for runtime dispatch
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
    set(OMNIDESK_AARCH64 TRUE)
endif()

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler warnings and optimization flags — applied per-target (via
# omnidesk_core PUBLIC) rather than globally, so FetchContent sub-projects
# (MbedTLS, libsrtp, libjuice, usrsctp) are not affected.
# See omnidesk_core target below for the actual flags.

# Dependencies (skip GUI deps in server-only mode)
if(NOT OMNIDESK_SERVER_ONLY)
    include(cmake/FetchDeps.cmake)
    include(cmake/FetchOpenH264.cmake)
    include(cmake/FetchLibDataChannel.cmake)

    # Optional hardware encoder dependencies
    if(OMNIDESK_PLATFORM_LINUX)
        include(cmake/FindVAAPI.cmake)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
            pkg_check_modules(X11 QUIET x11 xdamage xfixes xext xrandr)
            pkg_check_modules(XCB QUIET xcb xcb-shm xcb-xfixes)
        endif()
    endif()
endif()

# Core library
add_library(omnidesk_core STATIC
    src/core/logger.cpp
    src/core/thread_pool.cpp
    src/core/simd_utils.cpp
    src/core/clock.cpp
)
target_include_directories(omnidesk_core PUBLIC src)
target_compile_options(omnidesk_core PUBLIC -Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(omnidesk_core PUBLIC -O3)
endif()
target_compile_definitions(omnidesk_core PUBLIC
    $<$<BOOL:${OMNIDESK_PLATFORM_LINUX}>:OMNIDESK_PLATFORM_LINUX>
    $<$<BOOL:${OMNIDESK_PLATFORM_WINDOWS}>:OMNIDESK_PLATFORM_WINDOWS>
    $<$<BOOL:${OMNIDESK_X86_64}>:OMNIDESK_X86_64>
    $<$<BOOL:${OMNIDESK_AARCH64}>:OMNIDESK_AARCH64>
)
if(OMNIDESK_PLATFORM_WINDOWS)
    target_link_libraries(omnidesk_core PRIVATE shell32)
endif()

# AVX2 compilation unit (separate flags)
if(OMNIDESK_X86_64)
    add_library(omnidesk_avx2 STATIC src/core/simd_utils_avx2.cpp)
    target_include_directories(omnidesk_avx2 PUBLIC src)
    target_compile_options(omnidesk_avx2 PRIVATE -mavx2)
    target_link_libraries(omnidesk_core PRIVATE omnidesk_avx2)
endif()

# ---- GUI-dependent libraries (skipped in server-only mode) ----
if(NOT OMNIDESK_SERVER_ONLY)

# Screen capture
set(CAPTURE_SOURCES src/capture/capture_factory.cpp)
if(OMNIDESK_PLATFORM_LINUX)
    if(X11_FOUND)
        list(APPEND CAPTURE_SOURCES src/capture/x11_capture.cpp)
    endif()
    if(PIPEWIRE_FOUND)
        list(APPEND CAPTURE_SOURCES src/capture/pipewire_capture.cpp)
    endif()
endif()
if(OMNIDESK_PLATFORM_WINDOWS)
    list(APPEND CAPTURE_SOURCES src/capture/dxgi_capture.cpp)
endif()

add_library(omnidesk_capture STATIC ${CAPTURE_SOURCES})
target_link_libraries(omnidesk_capture PUBLIC omnidesk_core)
if(OMNIDESK_PLATFORM_WINDOWS)
    target_link_libraries(omnidesk_capture PRIVATE d3d11 dxgi)
endif()
if(X11_FOUND)
    target_include_directories(omnidesk_capture PRIVATE ${X11_INCLUDE_DIRS})
    target_link_libraries(omnidesk_capture PRIVATE ${X11_LIBRARIES})
    target_compile_definitions(omnidesk_capture PRIVATE OMNIDESK_HAS_X11)
endif()
if(PIPEWIRE_FOUND)
    target_include_directories(omnidesk_capture PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
    target_link_libraries(omnidesk_capture PRIVATE ${PIPEWIRE_LIBRARIES})
    target_compile_definitions(omnidesk_capture PRIVATE OMNIDESK_HAS_PIPEWIRE)
endif()

# Dirty region detection
add_library(omnidesk_diff STATIC
    src/diff/simd_diff.cpp
    src/diff/rect_merger.cpp
    src/diff/content_classifier.cpp
)
target_link_libraries(omnidesk_diff PUBLIC omnidesk_core)

# Codec
set(CODEC_SOURCES
    src/codec/openh264_loader.cpp
    src/codec/openh264_encoder.cpp
    src/codec/openh264_decoder.cpp
    src/codec/codec_factory.cpp
    src/codec/rate_control.cpp
    src/codec/quality_tuner.cpp
)
if(OMNIDESK_PLATFORM_LINUX AND VAAPI_FOUND)
    list(APPEND CODEC_SOURCES src/codec/vaapi_encoder.cpp)
endif()
if(OMNIDESK_PLATFORM_LINUX)
    list(APPEND CODEC_SOURCES src/codec/nvenc_encoder.cpp)
endif()
if(OMNIDESK_PLATFORM_WINDOWS)
    list(APPEND CODEC_SOURCES src/codec/nvenc_encoder.cpp)
    list(APPEND CODEC_SOURCES src/codec/mf_encoder.cpp)
endif()

add_library(omnidesk_codec STATIC ${CODEC_SOURCES})
target_link_libraries(omnidesk_codec PUBLIC omnidesk_core openh264)
if(VAAPI_FOUND)
    target_link_libraries(omnidesk_codec PRIVATE ${VAAPI_LIBRARIES})
    target_include_directories(omnidesk_codec PRIVATE ${VAAPI_INCLUDE_DIRS})
    target_compile_definitions(omnidesk_codec PRIVATE OMNIDESK_HAS_VAAPI)
endif()
# NVENC: compile the source unconditionally on supported platforms;
# the #ifdef OMNIDESK_HAS_NVENC guard inside the file keeps it a no-op
# unless the NVENC library is detected at runtime.
if(OMNIDESK_PLATFORM_LINUX OR OMNIDESK_PLATFORM_WINDOWS)
    target_compile_definitions(omnidesk_codec PRIVATE OMNIDESK_HAS_NVENC)
    if(OMNIDESK_PLATFORM_LINUX)
        target_link_libraries(omnidesk_codec PRIVATE dl)
    endif()
endif()
if(OMNIDESK_PLATFORM_WINDOWS)
    target_link_libraries(omnidesk_codec PRIVATE mfplat mfuuid mf wmcodecdspuuid)
endif()

endif() # NOT OMNIDESK_SERVER_ONLY

# Signaling
add_library(omnidesk_signaling STATIC
    src/signaling/signaling_server.cpp
    src/signaling/signaling_client.cpp
    src/signaling/tcp_channel.cpp
    src/signaling/user_id.cpp
)
target_link_libraries(omnidesk_signaling PUBLIC omnidesk_core)
if(OMNIDESK_PLATFORM_LINUX)
    target_link_libraries(omnidesk_signaling PRIVATE pthread)
endif()
if(OMNIDESK_PLATFORM_WINDOWS)
    target_link_libraries(omnidesk_signaling PRIVATE ws2_32)
endif()

# ---- GUI app targets (skipped in server-only mode) ----
if(NOT OMNIDESK_SERVER_ONLY)

# WebRTC integration (libdatachannel)
add_library(omnidesk_webrtc STATIC
    src/webrtc/webrtc_session.cpp
)
target_link_libraries(omnidesk_webrtc PUBLIC
    omnidesk_core omnidesk_signaling datachannel-static
)

# Render
add_library(omnidesk_render STATIC
    src/render/gl_renderer.cpp
    src/render/cursor_overlay.cpp
    src/render/sharpening.cpp
    src/render/gl_proc.cpp
)
target_link_libraries(omnidesk_render PUBLIC omnidesk_core glfw)
if(OMNIDESK_PLATFORM_LINUX)
    target_link_libraries(omnidesk_render PRIVATE GL)
endif()
if(OMNIDESK_PLATFORM_WINDOWS)
    target_link_libraries(omnidesk_render PRIVATE opengl32)
endif()

# Input
add_library(omnidesk_input STATIC
    src/input/input_handler.cpp
    src/input/cursor_predictor.cpp
)
target_link_libraries(omnidesk_input PUBLIC omnidesk_core glfw)

# UI
add_library(omnidesk_ui STATIC
    src/ui/app.cpp
    src/ui/theme.cpp
    src/ui/dashboard.cpp
    src/ui/session_view.cpp
    src/ui/settings_panel.cpp
    src/ui/connection_dialog.cpp
    src/ui/stats_overlay.cpp
    src/ui/history_panel.cpp
)
target_link_libraries(omnidesk_ui PUBLIC omnidesk_core imgui glfw)

# Session
add_library(omnidesk_session STATIC
    src/session/host_session.cpp
    src/session/viewer_session.cpp
)
target_link_libraries(omnidesk_session PUBLIC
    omnidesk_core omnidesk_capture omnidesk_diff
    omnidesk_codec omnidesk_webrtc omnidesk_signaling
    omnidesk_render omnidesk_input
)

# UI depends on session, signaling, and webrtc (app.cpp orchestrates everything)
target_link_libraries(omnidesk_ui PUBLIC omnidesk_session omnidesk_signaling omnidesk_render omnidesk_webrtc)

# Main executable (GUI app) — WIN32 suppresses the console window on Windows
if(OMNIDESK_PLATFORM_WINDOWS)
    add_executable(omnidesk24 WIN32 src/main.cpp)
else()
    add_executable(omnidesk24 src/main.cpp)
endif()
target_link_libraries(omnidesk24 PRIVATE omnidesk_ui omnidesk_session)
if(OMNIDESK_PLATFORM_WINDOWS)
    target_link_libraries(omnidesk24 PRIVATE shell32)
    # Statically link MinGW runtimes so the exe has no external DLL dependencies
    target_link_options(omnidesk24 PRIVATE
        -static-libgcc
        -static-libstdc++
        -Wl,-Bstatic,--whole-archive -lpthread -Wl,--no-whole-archive,-Bdynamic
    )
endif()

endif() # NOT OMNIDESK_SERVER_ONLY

# Standalone signaling server (no GUI dependencies)
option(OMNIDESK_BUILD_SERVER "Build standalone signaling server" ON)
if(OMNIDESK_BUILD_SERVER)
    if(NOT PkgConfig_FOUND)
        find_package(PkgConfig QUIET)
    endif()
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBPQ QUIET libpq)
    endif()

    add_library(omnidesk_database STATIC src/signaling/database.cpp)
    target_include_directories(omnidesk_database PUBLIC src)
    target_link_libraries(omnidesk_database PUBLIC omnidesk_core)
    if(LIBPQ_FOUND)
        target_include_directories(omnidesk_database PRIVATE ${LIBPQ_INCLUDE_DIRS})
        target_link_libraries(omnidesk_database PRIVATE ${LIBPQ_LIBRARIES})
    else()
        # Try direct linking (works on most systems with libpq-dev installed)
        target_link_libraries(omnidesk_database PRIVATE pq)
    endif()

    add_executable(omnidesk24-server src/server_main.cpp)
    target_link_libraries(omnidesk24-server PRIVATE
        omnidesk_signaling omnidesk_database
    )
endif()

# Tests (require full build, not server-only)
option(OMNIDESK_BUILD_TESTS "Build tests" ON)
if(OMNIDESK_BUILD_TESTS AND NOT OMNIDESK_SERVER_ONLY)
    enable_testing()
    add_subdirectory(tests)
endif()

# Benchmarks
option(OMNIDESK_BUILD_BENCHMARKS "Build benchmarks" OFF)
if(OMNIDESK_BUILD_BENCHMARKS AND NOT OMNIDESK_SERVER_ONLY)
    add_subdirectory(tests/benchmark)
endif()
