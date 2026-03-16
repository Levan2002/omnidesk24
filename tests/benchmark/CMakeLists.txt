# Benchmark tests (requires -DOMNIDESK_BUILD_BENCHMARKS=ON)

add_executable(omnidesk_benchmarks
    bench_simd_diff.cpp
    bench_encode.cpp
    bench_color_convert.cpp
    bench_fec.cpp
    bench_pipeline.cpp
)

target_link_libraries(omnidesk_benchmarks PRIVATE
    omnidesk_core
    omnidesk_diff
    omnidesk_codec
    omnidesk_transport
    benchmark::benchmark
    benchmark::benchmark_main
)
