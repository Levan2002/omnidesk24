#include <benchmark/benchmark.h>
#include "codec/openh264_encoder.h"
#include "codec/openh264_decoder.h"
#include "core/types.h"
#include <cstring>

using namespace omnidesk;

static Frame makeI420Frame(int w, int h) {
    Frame f;
    f.width = w;
    f.height = h;
    f.stride = w;
    f.format = PixelFormat::I420;
    f.data.resize(static_cast<size_t>(w) * h * 3 / 2);
    // Fill with a gradient pattern
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            f.data[y * w + x] = static_cast<uint8_t>((x + y) & 0xFF);
        }
    }
    size_t uvOffset = static_cast<size_t>(w) * h;
    std::memset(f.data.data() + uvOffset, 128, static_cast<size_t>(w / 2) * (h / 2) * 2);
    return f;
}

static void BM_OpenH264Encode_720p(benchmark::State& state) {
    OpenH264Encoder enc;
    EncoderConfig cfg{};
    cfg.width = 1280;
    cfg.height = 720;
    cfg.maxFps = 60;
    cfg.bitrateBps = 2000000;
    if (!enc.init(cfg)) {
        state.SkipWithError("Failed to init encoder");
        return;
    }

    Frame frame = makeI420Frame(1280, 720);
    frame.timestampUs = 0;
    EncodedPacket pkt;
    std::vector<RegionInfo> regions;

    for (auto _ : state) {
        frame.timestampUs += 16667;
        enc.encode(frame, regions, pkt);
        benchmark::DoNotOptimize(pkt);
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["fps"] = benchmark::Counter(
        static_cast<double>(state.iterations()),
        benchmark::Counter::kIsRate);
}
BENCHMARK(BM_OpenH264Encode_720p)->Unit(benchmark::kMillisecond);

static void BM_OpenH264Encode_1080p(benchmark::State& state) {
    OpenH264Encoder enc;
    EncoderConfig cfg{};
    cfg.width = 1920;
    cfg.height = 1080;
    cfg.maxFps = 60;
    cfg.bitrateBps = 4000000;
    if (!enc.init(cfg)) {
        state.SkipWithError("Failed to init encoder");
        return;
    }

    Frame frame = makeI420Frame(1920, 1080);
    frame.timestampUs = 0;
    EncodedPacket pkt;
    std::vector<RegionInfo> regions;

    for (auto _ : state) {
        frame.timestampUs += 16667;
        enc.encode(frame, regions, pkt);
        benchmark::DoNotOptimize(pkt);
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["fps"] = benchmark::Counter(
        static_cast<double>(state.iterations()),
        benchmark::Counter::kIsRate);
}
BENCHMARK(BM_OpenH264Encode_1080p)->Unit(benchmark::kMillisecond);

static void BM_OpenH264Decode_1080p(benchmark::State& state) {
    // Encode one frame to get valid NAL data
    OpenH264Encoder enc;
    EncoderConfig cfg{};
    cfg.width = 1920;
    cfg.height = 1080;
    cfg.maxFps = 30;
    cfg.bitrateBps = 4000000;
    if (!enc.init(cfg)) {
        state.SkipWithError("Failed to init encoder");
        return;
    }

    Frame frame = makeI420Frame(1920, 1080);
    frame.timestampUs = 0;
    EncodedPacket pkt;
    std::vector<RegionInfo> regions;
    enc.encode(frame, regions, pkt);

    OpenH264Decoder dec;
    if (!dec.init(1920, 1080)) {
        state.SkipWithError("Failed to init decoder");
        return;
    }

    Frame decoded;
    for (auto _ : state) {
        dec.decode(pkt.data.data(), pkt.data.size(), decoded);
        benchmark::DoNotOptimize(decoded);
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["fps"] = benchmark::Counter(
        static_cast<double>(state.iterations()),
        benchmark::Counter::kIsRate);
}
BENCHMARK(BM_OpenH264Decode_1080p)->Unit(benchmark::kMillisecond);
