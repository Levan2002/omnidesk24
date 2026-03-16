# Unit tests
add_executable(omnidesk_tests
    unit/test_ring_buffer.cpp
    unit/test_simd_utils.cpp
    unit/test_rect_merger.cpp
    unit/test_content_classifier.cpp
    unit/test_fec.cpp
    unit/test_congestion.cpp
    unit/test_protocol.cpp
    unit/test_user_id.cpp
    unit/test_quality_tuner.cpp
    unit/test_framer.cpp
    unit/test_cursor_predictor.cpp
)

target_link_libraries(omnidesk_tests PRIVATE
    omnidesk_core
    omnidesk_diff
    omnidesk_transport
    omnidesk_signaling
    omnidesk_codec
    omnidesk_input
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(omnidesk_tests)

# Integration tests
add_executable(omnidesk_integration_tests
    integration/test_capture_encode_pipeline.cpp
    integration/test_signaling_flow.cpp
    integration/test_transport_loopback.cpp
)

target_link_libraries(omnidesk_integration_tests PRIVATE
    omnidesk_session
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
