#include <benchmark/benchmark.h>
#include "core/simd_utils.h"
#include "core/types.h"
#include <cstring>

using namespace omnidesk;

static void BM_BGRA_to_I420_720p(benchmark::State& state) {
    const int w = 1280, h = 720;
    std::vector<uint8_t> bgra(w * h * 4, 128);
    std::vector<uint8_t> i420(w * h * 3 / 2);

    for (auto _ : state) {
        SIMDUtils::bgraToI420(bgra.data(), w * 4, i420.data(), w, w, h);
        benchmark::DoNotOptimize(i420.data());
    }
    state.SetBytesProcessed(state.iterations() * w * h * 4);
}
BENCHMARK(BM_BGRA_to_I420_720p);

static void BM_BGRA_to_I420_1080p(benchmark::State& state) {
    const int w = 1920, h = 1080;
    std::vector<uint8_t> bgra(w * h * 4, 128);
    std::vector<uint8_t> i420(w * h * 3 / 2);

    for (auto _ : state) {
        SIMDUtils::bgraToI420(bgra.data(), w * 4, i420.data(), w, w, h);
        benchmark::DoNotOptimize(i420.data());
    }
    state.SetBytesProcessed(state.iterations() * w * h * 4);
}
BENCHMARK(BM_BGRA_to_I420_1080p);

static void BM_BGRA_to_I420_4K(benchmark::State& state) {
    const int w = 3840, h = 2160;
    std::vector<uint8_t> bgra(w * h * 4, 128);
    std::vector<uint8_t> i420(w * h * 3 / 2);

    for (auto _ : state) {
        SIMDUtils::bgraToI420(bgra.data(), w * 4, i420.data(), w, w, h);
        benchmark::DoNotOptimize(i420.data());
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(w) * h * 4);
}
BENCHMARK(BM_BGRA_to_I420_4K);
