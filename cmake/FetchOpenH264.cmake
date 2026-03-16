include(FetchContent)

# Cisco OpenH264 - royalty-free H.264 implementation
set(OPENH264_VERSION "2.4.1")

# Try to find system-installed OpenH264 first
find_library(OPENH264_LIBRARY NAMES openh264
    HINTS /usr/lib /usr/lib64 /usr/local/lib
)
find_path(OPENH264_INCLUDE_DIR NAMES codec_api.h
    HINTS /usr/include/openh264 /usr/local/include/openh264
          /usr/include/wels /usr/local/include/wels
)

if(OPENH264_LIBRARY AND OPENH264_INCLUDE_DIR)
    message(STATUS "Found system OpenH264: ${OPENH264_LIBRARY}")
    add_library(openh264 INTERFACE)
    target_include_directories(openh264 INTERFACE ${OPENH264_INCLUDE_DIR})
    target_link_libraries(openh264 INTERFACE ${OPENH264_LIBRARY})
else()
    message(STATUS "System OpenH264 not found, fetching headers from source...")

    # Fetch only the source for headers (we'll link dynamically at runtime)
    FetchContent_Declare(
        openh264_src
        GIT_REPOSITORY https://github.com/cisco/openh264.git
        GIT_TAG v${OPENH264_VERSION}
        GIT_SHALLOW TRUE
    )

    FetchContent_GetProperties(openh264_src)
    if(NOT openh264_src_POPULATED)
        FetchContent_Populate(openh264_src)
    endif()

    add_library(openh264 INTERFACE)
    target_include_directories(openh264 INTERFACE
        ${openh264_src_SOURCE_DIR}/codec/api/wels
    )

    # Try to link against system lib, or leave for runtime loading
    find_library(OPENH264_LIB2 NAMES openh264)
    if(OPENH264_LIB2)
        target_link_libraries(openh264 INTERFACE ${OPENH264_LIB2})
    else()
        message(STATUS "OpenH264 library not found - will use dlopen at runtime")
        target_compile_definitions(openh264 INTERFACE OMNIDESK_OPENH264_DLOPEN)
    endif()
endif()

message(STATUS "OpenH264 version: ${OPENH264_VERSION}")
