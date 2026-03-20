# Benchmark tests (requires -DOMNIDESK_BUILD_BENCHMARKS=ON)
# NOTE: bench_fec removed — the FEC transport layer it benchmarked has been
# deleted (WebRTC migration Task 3).
# NOTE: bench_simd_diff and bench_color_convert removed — they reference
# deleted SimdDiff/SIMDUtils classes.

add_executable(omnidesk_benchmarks
    bench_encode.cpp
    bench_pipeline.cpp
    bench_codec_compare.cpp
)

target_link_libraries(omnidesk_benchmarks PRIVATE
    omnidesk_core
    omnidesk_diff
    omnidesk_codec
    benchmark::benchmark
    benchmark::benchmark_main
)
