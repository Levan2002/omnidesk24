#include <benchmark/benchmark.h>
#include "core/ring_buffer.h"
#include "core/types.h"
#include "diff/rect_merger.h"
#include "diff/content_classifier.h"
#include <cstring>

using namespace omnidesk;

static void BM_RingBuffer_PushPop(benchmark::State& state) {
    RingBuffer<int, 1024> rb;
    int val = 0;

    for (auto _ : state) {
        rb.push(42);
        rb.pop(val);
        benchmark::DoNotOptimize(val);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RingBuffer_PushPop);

static void BM_RectMerger_ManyRects(benchmark::State& state) {
    RectMerger merger;
    std::vector<Rect> rects;
    // Simulate many small dirty rects (scattered cursor trails, UI updates)
    for (int i = 0; i < 50; i++) {
        rects.push_back({(i * 37) % 1900, (i * 23) % 1060, 16, 16});
    }

    for (auto _ : state) {
        auto merged = merger.merge(rects);
        benchmark::DoNotOptimize(merged);
    }
    state.SetItemsProcessed(state.iterations() * 50);
}
BENCHMARK(BM_RectMerger_ManyRects);

static void BM_ContentClassifier_1080p(benchmark::State& state) {
    Frame frame;
    frame.width = 1920;
    frame.height = 1080;
    frame.stride = 1920 * 4;
    frame.format = PixelFormat::BGRA;
    frame.data.resize(1920 * 1080 * 4);
    // Simulate text-like content: high contrast edges
    for (int y = 0; y < 1080; y++) {
        for (int x = 0; x < 1920; x++) {
            uint8_t val = ((x / 8 + y / 16) % 2 == 0) ? 0 : 255;
            size_t idx = (y * 1920 + x) * 4;
            frame.data[idx] = val;
            frame.data[idx + 1] = val;
            frame.data[idx + 2] = val;
            frame.data[idx + 3] = 255;
        }
    }

    ContentClassifier classifier;
    Rect region{0, 0, 1920, 1080};

    for (auto _ : state) {
        auto type = classifier.classify(frame, region);
        benchmark::DoNotOptimize(type);
    }
}
BENCHMARK(BM_ContentClassifier_1080p)->Unit(benchmark::kMillisecond);
