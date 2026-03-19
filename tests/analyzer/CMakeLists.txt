# Codec analyzer tool — standalone CLI for encoding performance analysis.
# Build with -DOMNIDESK_BUILD_ANALYZER=ON

add_executable(codec_analyzer
    codec_analyzer.cpp
    analyzer_engine.cpp
    resource_monitor.cpp
    report_generator.cpp
)

target_include_directories(codec_analyzer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(codec_analyzer PRIVATE
    omnidesk_codec
    omnidesk_core
    omnidesk_diff
)
