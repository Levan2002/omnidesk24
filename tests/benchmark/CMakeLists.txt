# Benchmark tests (requires -DOMNIDESK_BUILD_BENCHMARKS=ON)
# NOTE: bench_fec removed — the FEC transport layer it benchmarked has been
# deleted (WebRTC migration Task 3).

add_executable(omnidesk_benchmarks
    bench_simd_diff.cpp
    bench_encode.cpp
    bench_color_convert.cpp
    bench_pipeline.cpp
)

target_link_libraries(omnidesk_benchmarks PRIVATE
    omnidesk_core
    omnidesk_diff
    omnidesk_codec
    benchmark::benchmark
    benchmark::benchmark_main
)
