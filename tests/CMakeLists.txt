# Unit tests
# NOTE: test_fec, test_congestion, test_framer, test_protocol removed —
# the transport layer they tested has been deleted (WebRTC migration Task 3).
# test_protocol wire-format tests are covered by signaling/wire_format.h usage.

# Platform-portable unit tests (no GUI or platform-specific input deps)
set(OMNIDESK_UNIT_TEST_SOURCES
    unit/test_ring_buffer.cpp
    unit/test_simd_utils.cpp
    unit/test_rect_merger.cpp
    unit/test_content_classifier.cpp
    unit/test_user_id.cpp
    unit/test_quality_tuner.cpp
)

set(OMNIDESK_UNIT_TEST_LIBS
    omnidesk_core
    omnidesk_diff
    omnidesk_signaling
    omnidesk_codec
    GTest::gtest_main
)

# cursor_predictor and omnidesk_input require platform headers (windows.h)
if(OMNIDESK_PLATFORM_WINDOWS)
    list(APPEND OMNIDESK_UNIT_TEST_SOURCES unit/test_cursor_predictor.cpp)
    list(APPEND OMNIDESK_UNIT_TEST_LIBS omnidesk_input)
endif()

add_executable(omnidesk_tests ${OMNIDESK_UNIT_TEST_SOURCES})
target_link_libraries(omnidesk_tests PRIVATE ${OMNIDESK_UNIT_TEST_LIBS})

include(GoogleTest)
gtest_discover_tests(omnidesk_tests)

# Integration tests
# NOTE: test_transport_loopback removed — the UDP transport layer it tested
# has been deleted (WebRTC migration Task 3).
# NOTE: Integration tests require omnidesk_session which depends on
# omnidesk_render (GL) and omnidesk_input (windows.h) — desktop-only.
if(NOT OMNIDESK_SERVER_ONLY AND TARGET omnidesk_session)
    add_executable(omnidesk_integration_tests
        integration/test_capture_encode_pipeline.cpp
        integration/test_signaling_flow.cpp
        integration/test_data_channel_flow.cpp
        integration/test_webrtc_e2e.cpp
    )

    target_link_libraries(omnidesk_integration_tests PRIVATE
        omnidesk_session
        omnidesk_webrtc
        omnidesk_codec
        GTest::gtest_main
    )

    gtest_discover_tests(omnidesk_integration_tests)
endif()

# Visual quality tests
add_executable(omnidesk_visual_tests
    visual/test_encode_quality.cpp
)

target_include_directories(omnidesk_visual_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(omnidesk_visual_tests PRIVATE
    omnidesk_codec
    omnidesk_diff
    GTest::gtest_main
)

gtest_discover_tests(omnidesk_visual_tests)

# Codec analyzer tool
if(OMNIDESK_BUILD_ANALYZER)
    add_subdirectory(analyzer)
endif()
