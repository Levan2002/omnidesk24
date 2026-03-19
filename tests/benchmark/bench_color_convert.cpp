#include <benchmark/benchmark.h>
#include "core/simd_utils.h"
#include "core/types.h"
#include <cstring>

using namespace omnidesk;

static void BM_BGRA_to_I420_720p(benchmark::State& state) {
    const int w = 1280, h = 720;
    std::vector<uint8_t> bgra(w * h * 4, 128);
    std::vector<uint8_t> yPlane(w * h);
    std::vector<uint8_t> uPlane(w / 2 * h / 2);
    std::vector<uint8_t> vPlane(w / 2 * h / 2);

    for (auto _ : state) {
        bgraToI420(bgra.data(), w, h, w * 4,
                   yPlane.data(), w,
                   uPlane.data(), w / 2,
                   vPlane.data(), w / 2);
        benchmark::DoNotOptimize(yPlane.data());
    }
    state.SetBytesProcessed(state.iterations() * w * h * 4);
}
BENCHMARK(BM_BGRA_to_I420_720p);

static void BM_BGRA_to_I420_1080p(benchmark::State& state) {
    const int w = 1920, h = 1080;
    std::vector<uint8_t> bgra(w * h * 4, 128);
    std::vector<uint8_t> yPlane(w * h);
    std::vector<uint8_t> uPlane(w / 2 * h / 2);
    std::vector<uint8_t> vPlane(w / 2 * h / 2);

    for (auto _ : state) {
        bgraToI420(bgra.data(), w, h, w * 4,
                   yPlane.data(), w,
                   uPlane.data(), w / 2,
                   vPlane.data(), w / 2);
        benchmark::DoNotOptimize(yPlane.data());
    }
    state.SetBytesProcessed(state.iterations() * w * h * 4);
}
BENCHMARK(BM_BGRA_to_I420_1080p);

static void BM_BGRA_to_I420_4K(benchmark::State& state) {
    const int w = 3840, h = 2160;
    std::vector<uint8_t> bgra(w * h * 4, 128);
    std::vector<uint8_t> yPlane(w * h);
    std::vector<uint8_t> uPlane(w / 2 * h / 2);
    std::vector<uint8_t> vPlane(w / 2 * h / 2);

    for (auto _ : state) {
        bgraToI420(bgra.data(), w, h, w * 4,
                   yPlane.data(), w,
                   uPlane.data(), w / 2,
                   vPlane.data(), w / 2);
        benchmark::DoNotOptimize(yPlane.data());
    }
    state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(w) * h * 4);
}
BENCHMARK(BM_BGRA_to_I420_4K);

static void BM_BGRA_to_NV12_1080p(benchmark::State& state) {
    const int w = 1920, h = 1080;
    std::vector<uint8_t> bgra(w * h * 4, 128);
    std::vector<uint8_t> yPlane(w * h);
    std::vector<uint8_t> uvPlane(w * h / 2);

    for (auto _ : state) {
        bgraToNV12(bgra.data(), w, h, w * 4,
                   yPlane.data(), w,
                   uvPlane.data(), w);
        benchmark::DoNotOptimize(yPlane.data());
    }
    state.SetBytesProcessed(state.iterations() * w * h * 4);
}
BENCHMARK(BM_BGRA_to_NV12_1080p);

static void BM_BGRA_to_I420_DirtyRegion_1080p(benchmark::State& state) {
    const int w = 1920, h = 1080;
    Frame src;
    src.width = w;
    src.height = h;
    src.stride = w * 4;
    src.format = PixelFormat::BGRA;
    src.data.resize(w * h * 4, 128);

    Frame dst;
    std::vector<Rect> dirtyRects = {{100, 100, 200, 150}, {800, 400, 300, 200}};

    for (auto _ : state) {
        convertDirtyRegionsToI420(src, dst, dirtyRects);
        benchmark::DoNotOptimize(dst.data.data());
    }
    // Only converting ~90K pixels instead of ~2M
    state.SetBytesProcessed(state.iterations() * (200 * 150 + 300 * 200) * 4);
}
BENCHMARK(BM_BGRA_to_I420_DirtyRegion_1080p);

static void BM_ResizeI420_1080p_to_720p(benchmark::State& state) {
    Frame src;
    src.width = 1920;
    src.height = 1080;
    src.stride = 1920;
    src.format = PixelFormat::I420;
    src.data.resize(1920 * 1080 * 3 / 2, 128);

    Frame dst;

    for (auto _ : state) {
        resizeI420(src, dst, 1280, 720);
        benchmark::DoNotOptimize(dst.data.data());
    }
    state.SetBytesProcessed(state.iterations() * 1280 * 720 * 3 / 2);
}
BENCHMARK(BM_ResizeI420_1080p_to_720p)->Unit(benchmark::kMillisecond);
