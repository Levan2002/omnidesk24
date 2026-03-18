# Unit tests
# NOTE: test_fec, test_congestion, test_framer, test_protocol removed —
# the transport layer they tested has been deleted (WebRTC migration Task 3).
# test_protocol wire-format tests are covered by signaling/wire_format.h usage.
add_executable(omnidesk_tests
    unit/test_ring_buffer.cpp
    unit/test_simd_utils.cpp
    unit/test_rect_merger.cpp
    unit/test_content_classifier.cpp
    unit/test_user_id.cpp
    unit/test_quality_tuner.cpp
    unit/test_cursor_predictor.cpp
)

target_link_libraries(omnidesk_tests PRIVATE
    omnidesk_core
    omnidesk_diff
    omnidesk_signaling
    omnidesk_codec
    omnidesk_input
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(omnidesk_tests)

# Integration tests
# NOTE: test_transport_loopback removed — the UDP transport layer it tested
# has been deleted (WebRTC migration Task 3).
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

# Visual quality tests
add_executable(omnidesk_visual_tests
    visual/test_encode_quality.cpp
)

target_link_libraries(omnidesk_visual_tests PRIVATE
    omnidesk_codec
    omnidesk_diff
    GTest::gtest_main
)

gtest_discover_tests(omnidesk_visual_tests)
