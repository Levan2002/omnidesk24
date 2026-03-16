#include <benchmark/benchmark.h>
#include "diff/simd_diff.h"
#include "core/types.h"
#include <cstring>
#include <random>

using namespace omnidesk;

static Frame makeFrame(int w, int h) {
    Frame f;
    f.width = w;
    f.height = h;
    f.stride = w * 4;
    f.format = PixelFormat::BGRA;
    f.data.resize(static_cast<size_t>(w) * h * 4);
    return f;
}

static void BM_SIMDDiff_1080p_NoDiff(benchmark::State& state) {
    Frame prev = makeFrame(1920, 1080);
    Frame curr = makeFrame(1920, 1080);
    std::memset(prev.data.data(), 128, prev.data.size());
    std::memset(curr.data.data(), 128, curr.data.size());

    SimdDiff diff;
    for (auto _ : state) {
        auto rects = diff.detectDirtyBlocks(prev, curr);
        benchmark::DoNotOptimize(rects);
    }
    state.SetItemsProcessed(state.iterations() * 1920LL * 1080);
}
BENCHMARK(BM_SIMDDiff_1080p_NoDiff);

static void BM_SIMDDiff_1080p_FullDiff(benchmark::State& state) {
    Frame prev = makeFrame(1920, 1080);
    Frame curr = makeFrame(1920, 1080);
    std::memset(prev.data.data(), 0, prev.data.size());
    std::memset(curr.data.data(), 255, curr.data.size());

    SimdDiff diff;
    for (auto _ : state) {
        auto rects = diff.detectDirtyBlocks(prev, curr);
        benchmark::DoNotOptimize(rects);
    }
    state.SetItemsProcessed(state.iterations() * 1920LL * 1080);
}
BENCHMARK(BM_SIMDDiff_1080p_FullDiff);

static void BM_SIMDDiff_4K_SparseDiff(benchmark::State& state) {
    Frame prev = makeFrame(3840, 2160);
    Frame curr = makeFrame(3840, 2160);
    std::memset(prev.data.data(), 100, prev.data.size());
    std::memcpy(curr.data.data(), prev.data.data(), prev.data.size());

    // Dirty a small region (simulating cursor movement)
    for (int y = 500; y < 532; y++) {
        std::memset(curr.data.data() + y * curr.stride + 500 * 4, 200, 32 * 4);
    }

    SimdDiff diff;
    for (auto _ : state) {
        auto rects = diff.detectDirtyBlocks(prev, curr);
        benchmark::DoNotOptimize(rects);
    }
    state.SetItemsProcessed(state.iterations() * 3840LL * 2160);
}
BENCHMARK(BM_SIMDDiff_4K_SparseDiff);
